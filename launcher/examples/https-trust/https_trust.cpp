// ps5-native-app-boilerplate - Bounded root-CA import through the platform API.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "https_trust.hpp"
#include <array>
#include <cstddef>
#include <string>

namespace
{
// Public API data layout: pointer followed by a 64-bit byte length.
struct Certificate
{
    const void *data;
    std::size_t size;
};
extern "C" int sceHttpsLoadCert(int context, int count, const Certificate *const *roots,
                                const Certificate *client, const Certificate *key);
struct BuiltinRoots
{
    Certificate *certificates;
    std::size_t count;
    void *buffer;
};
extern "C" int sceSslGetCaCerts(int ssl_context, BuiltinRoots *roots);
extern "C" int sceSslFreeCaCerts(int ssl_context, BuiltinRoots *roots);
} // namespace
namespace https_trust
{
int read_builtin_roots(int ssl_context, std::string &out)
{
    BuiltinRoots roots{};
    const int queried = sceSslGetCaCerts(ssl_context, &roots);
    if (queried < 0)
        return queried;
    bool valid = roots.certificates && roots.count > 0 && roots.count <= 256;
    std::string pem;
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; valid && index < roots.count; ++index)
    {
        const auto &certificate = roots.certificates[index];
        if (!certificate.data || certificate.size == 0 || certificate.size > 12u * 1024 ||
            pem.size() + certificate.size * 2 + 100 > (512u << 10))
        {
            valid = false;
            break;
        }
        const auto *bytes = static_cast<const unsigned char *>(certificate.data);
        const std::string_view encoded(static_cast<const char *>(certificate.data),
                                       certificate.size);
        if (encoded.starts_with("-----BEGIN CERTIFICATE-----"))
        {
            pem.append(encoded);
            if (pem.back() == '\0')
                pem.pop_back();
            pem += '\n';
            continue;
        }
        pem += "-----BEGIN CERTIFICATE-----\n";
        unsigned column = 0;
        for (std::size_t i = 0; i < certificate.size; i += 3)
        {
            const unsigned value = (unsigned(bytes[i]) << 16) |
                                   (i + 1 < certificate.size ? unsigned(bytes[i + 1]) << 8 : 0) |
                                   (i + 2 < certificate.size ? unsigned(bytes[i + 2]) : 0);
            pem += alphabet[(value >> 18) & 63];
            pem += alphabet[(value >> 12) & 63];
            pem += i + 1 < certificate.size ? alphabet[(value >> 6) & 63] : '=';
            pem += i + 2 < certificate.size ? alphabet[value & 63] : '=';
            column += 4;
            if (column == 64)
            {
                pem += '\n';
                column = 0;
            }
        }
        if (column)
            pem += '\n';
        pem += "-----END CERTIFICATE-----\n";
    }
    const int freed = sceSslFreeCaCerts(ssl_context, &roots);
    if (!valid || freed < 0)
        return freed < 0 ? freed : -1;
    out = std::move(pem);
    return 0;
}
int load_builtin_roots(int ssl_context, int http_context)
{
    std::string pem;
    const int result = read_builtin_roots(ssl_context, pem);
    return result < 0 ? result : load_pem_roots(http_context, pem);
}
int load_pem_roots(int http_context, std::string_view pem)
{
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    if (http_context < 0 || pem.empty() || pem.size() > (512u << 10) ||
        pem.find('\0') != std::string_view::npos)
        return -1;
    std::array<Certificate, 256> certificates{};
    std::array<const Certificate *, 256> pointers{};
    std::size_t count = 0;
    while (true)
    {
        const auto first = pem.find(begin);
        if (first == std::string_view::npos)
            break;
        pem.remove_prefix(first);
        const auto last = pem.find(end);
        if (last == std::string_view::npos || last + end.size() > 16384 ||
            count == certificates.size())
            return -1;
        const auto length = last + end.size();
        if (pem.substr(begin.size(), last - begin.size()).find(begin) != std::string_view::npos)
            return -1;
        certificates[count] = {pem.data(), length};
        pointers[count] = &certificates[count];
        ++count;
        pem.remove_prefix(length);
    }
    if (count == 0)
        return -1;
    return sceHttpsLoadCert(http_context, static_cast<int>(count), pointers.data(), nullptr,
                            nullptr);
}
} // namespace https_trust

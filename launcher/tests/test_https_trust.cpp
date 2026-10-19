// ps5-native-app-boilerplate - Certificate import ABI and input boundary checks.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../examples/https-trust/https_trust.hpp"
#include <cassert>
#include <cstddef>
#include <string>
struct Certificate
{
    const void *data;
    std::size_t size;
};
int imports = 0;
struct BuiltinRoots
{
    Certificate *certificates;
    std::size_t count;
    void *buffer;
};
int frees = 0;
bool builtin = false;
bool pem_format = false;
extern "C" int sceSslGetCaCerts(int context, BuiltinRoots *out)
{
    assert(context == 3);
    static const unsigned char first[] = {1, 2, 3};
    static const unsigned char second[] = {255};
    static Certificate roots[] = {{first, sizeof(first)}, {second, sizeof(second)}};
    static const char pem[] = "-----BEGIN CERTIFICATE-----\nAQID\n-----END CERTIFICATE-----";
    roots[0] = pem_format ? Certificate{pem, sizeof(pem)} : Certificate{first, sizeof(first)};
    *out = {roots, 2, nullptr};
    return 0;
}
extern "C" int sceSslFreeCaCerts(int context, BuiltinRoots *out)
{
    assert(context == 3 && out->count == 2);
    ++frees;
    return 0;
}
extern "C" int sceHttpsLoadCert(int context, int count, const Certificate *const *roots,
                                const Certificate *client, const Certificate *key)
{
    assert(context == 7 && count == 2 && !client && !key);
    for (int i = 0; i < count; ++i)
    {
        const std::string_view pem(static_cast<const char *>(roots[i]->data), roots[i]->size);
        assert(pem.starts_with("-----BEGIN CERTIFICATE-----") &&
               pem.ends_with("-----END CERTIFICATE-----"));
        if (builtin)
            assert(pem.find(i == 0 ? "\nAQID\n" : "\n/w==\n") != std::string_view::npos);
    }
    ++imports;
    return 123;
}
int main()
{
    const std::string cert = "-----BEGIN CERTIFICATE-----\nAQ==\n-----END CERTIFICATE-----";
    assert(https_trust::load_pem_roots(7, "# system roots\n" + cert + "\r\n" + cert) == 123);
    assert(imports == 1);
    assert(https_trust::load_pem_roots(7, "") < 0);
    assert(https_trust::load_pem_roots(7, cert.substr(0, cert.size() - 1)) < 0);
    assert(https_trust::load_pem_roots(7, std::string(512 * 1024 + 1, 'x')) < 0);
    assert(https_trust::load_pem_roots(7, cert + std::string(1, '\0')) < 0);
    std::string many;
    for (int i = 0; i < 257; ++i)
        many += cert;
    assert(https_trust::load_pem_roots(7, many) < 0);
    assert(imports == 1);
    builtin = true;
    assert(https_trust::load_builtin_roots(3, 7) == 123);
    assert(imports == 2 && frees == 1);
    pem_format = true;
    assert(https_trust::load_builtin_roots(3, 7) == 123);
    assert(imports == 3 && frees == 2);
}

// Prospero BlackBox - A finished SHA-256 in hex.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "third_party/picosha2/picosha2.h"
#include <array>
#include <cstdint>
#include <string>

namespace store::library
{
// Lower-case hex of a finished hash. picosha2's own string helpers go through
// std::ostringstream, whose locale code the console app can't link (it has no libc.a).
inline std::string sha256_hex(const picosha2::hash256_one_by_one &hasher)
{
    std::array<std::uint8_t, 32> digest{};
    hasher.get_hash_bytes(digest.begin(), digest.end());
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const auto byte : digest)
    {
        hex += digits[byte >> 4];
        hex += digits[byte & 0xF];
    }
    return hex;
}
} // namespace store::library

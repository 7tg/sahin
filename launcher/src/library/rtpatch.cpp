// Prospero BlackBox - A publisher's RTPatch update applied without running it.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 0xB59C codec (Bits, Model, decompress) and the MODIFY program follow rtptool
// (https://github.com/bwrsandman/rtptool, MIT, (c) 2026 Sandy Carter) by way of
// fighters-codex's port (https://github.com/jomkz/fighters-codex, lib/src/rtpatch.cpp,
// MIT, (c) 2026 John McKenzie). Their notice applies to that part:
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify,
//   merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following
//   conditions: The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#include "library/rtpatch.hpp"
#include "library/base_game.hpp"
#include "library/install_plan.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store::library
{
namespace rtpatch
{
int Bits::bit()
{
    if (pos >= end)
    {
        bad = true;
        return 0;
    }
    const int value = (data[pos] >> (left - 1)) & 1;
    if (--left == 0)
    {
        ++pos;
        left = 8;
    }
    return value;
}

std::uint32_t Bits::bits(int count)
{
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i)
        value = value << 1 | static_cast<std::uint32_t>(bit());
    return value;
}

namespace
{
// A damaged stream can send the rebuild loops round far longer than a real one ever
// does (thousands of steps); past this the model is given up on.
constexpr long kRebuildGuard = 100000;

std::int16_t wrap16(int value)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
} // namespace

// The tables live in one byte buffer at the offsets the original decoder used: counters
// at 0x00-0x32, then per level a group count, per level a pointer into the slot list,
// the slot list (pointers to weights), the weights, and the decoding limits.
Model::Model(int escape_bits, int levels, int initial_period, int update_period)
{
    const int alpha = 1 << escape_bits;
    const std::uint32_t groups = 0x34, symbols = static_cast<std::uint32_t>(levels * 2 + 0x34),
                        slots = static_cast<std::uint32_t>(levels * 6 + 0x38),
                        weights = static_cast<std::uint32_t>(levels * 6 + 0x40 + alpha * 4),
                        limit = weights + 4 + static_cast<std::uint32_t>(alpha * 2);
    m_.assign(limit + 0xC0 + 0x100, 0);
    for (const std::uint32_t at : {0x32u, 0x02u, 0x00u})
        w16(at, initial_period);
    for (const std::uint32_t at : {0x30u, 0x2Eu, 0x2Cu})
        w16(at, update_period);
    w16(0x06, escape_bits);
    w16(0x04, levels);
    w32(0x0C, groups);
    w32(0x20, symbols);
    w32(0x1C, slots);
    w32(0x18, weights);
    w32(0x14, limit);
    w32(0x10, limit);
    w16(0x0A, 1);
    w16(0x08, 1);
    w16(groups, 2);
    w32(symbols, slots);
    for (int i = 1; i <= levels; ++i)
        w32(symbols + static_cast<std::uint32_t>(i) * 4, slots + 8);
    const std::uint32_t base = weights + static_cast<std::uint32_t>(alpha) * 2;
    w32(slots, base);
    for (int i = 1; i < alpha + 2; ++i)
        w32(slots + static_cast<std::uint32_t>(i) * 4, base + 2);
    for (int j = 0; j < alpha; ++j)
        w16(weights + static_cast<std::uint32_t>(j) * 2, 0x8000);
    w16(0x24, alpha);
    limits(0);
}

std::uint16_t Model::r16(std::uint32_t at) const
{
    if (static_cast<std::size_t>(at) + 1 >= m_.size())
        return 0;
    return static_cast<std::uint16_t>(m_[at] | m_[at + 1] << 8);
}

std::int16_t Model::s16(std::uint32_t at) const
{
    return static_cast<std::int16_t>(r16(at));
}

void Model::w16(std::uint32_t at, int value)
{
    if (static_cast<std::size_t>(at) + 1 >= m_.size())
        return;
    const auto v = static_cast<std::uint16_t>(value);
    m_[at] = static_cast<std::uint8_t>(v);
    m_[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

std::uint32_t Model::r32(std::uint32_t at) const
{
    if (static_cast<std::size_t>(at) + 3 >= m_.size())
        return 0;
    return static_cast<std::uint32_t>(m_[at]) | static_cast<std::uint32_t>(m_[at + 1]) << 8 |
           static_cast<std::uint32_t>(m_[at + 2]) << 16 |
           static_cast<std::uint32_t>(m_[at + 3]) << 24;
}

void Model::w32(std::uint32_t at, std::uint32_t value)
{
    if (static_cast<std::size_t>(at) + 3 >= m_.size())
        return;
    for (int i = 0; i < 4; ++i)
        m_[at + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
}

bool Model::known(std::uint32_t symbol) const
{
    return symbol < (1u << r16(0x06)) && r16(r32(0x18) + symbol * 2) != 0x8000;
}

// Kept to 16 bits throughout, as the original's registers were.
void Model::limits(int start)
{
    const std::uint32_t groups = r32(0x0C), limit = r32(0x10);
    const int levels = r16(0x04);
    int s3 = start == 0 ? 2 : wrap16(s16(limit + static_cast<std::uint32_t>(start - 1) * 8) * 2);
    for (int level = start; level < levels; ++level)
    {
        const std::uint32_t at = limit + static_cast<std::uint32_t>(level) * 8;
        const int s4 = wrap16(s3 - s16(groups + static_cast<std::uint32_t>(level) * 2));
        w16(at, s4);
        s3 = wrap16(s4 * 2);
        w16(at + 2, s3);
        w16(at + 4, s4 * 4);
        w16(at + 6, s4 * 16);
    }
}

std::uint32_t Model::decode(Bits &bits)
{
    if (broken_ || bits.pos >= bits.end)
    {
        bits.bad = true;
        return 0;
    }
    std::uint32_t value = ((1u << bits.left) - 1) & bits.data[bits.pos];
    int index = bits.left - 1, total = bits.left;
    const std::uint32_t limit = r32(0x10);
    if (value < r16(limit + static_cast<std::uint32_t>(index) * 8))
        for (;;)
        {
            if (++bits.pos >= bits.end)
            {
                bits.bad = true;
                return 0;
            }
            index += 8;
            total += 8;
            value = ((value & 0xFF) << 8 | bits.data[bits.pos]) & 0xFFFF;
            if (value >= r16(limit + static_cast<std::uint32_t>(index) * 8))
                break;
        }
    --index;
    int count = (total - 1) & 0xFF, shifted = 0;
    while (count != 0)
    {
        const std::uint32_t at = limit + 2 + static_cast<std::uint32_t>(index) * 8;
        if (value < (at < m_.size() - 1 ? r16(at) : 0u))
            break;
        --index;
        --count;
        value >>= 1;
        ++shifted;
    }
    const auto level = static_cast<std::uint16_t>(index + 1);
    const std::uint32_t slots = r32(r32(0x20) + level * 4u);
    const auto k =
        static_cast<std::uint16_t>(value - static_cast<std::uint16_t>(s16(limit + level * 8u)));
    const std::uint32_t symbol = (r32(slots + k * 4u) - r32(0x18)) >> 1;
    if (shifted == 0)
    {
        shifted = 8;
        ++bits.pos;
    }
    bits.left = shifted;

    const std::uint32_t weights = r32(0x18);
    w16(weights + symbol * 2, r16(weights + symbol * 2) + 1);
    const int countdown = s16(0x00) - 1;
    w16(0x00, countdown);
    if ((countdown & 0xFFFF) == 0)
    {
        rebuild();
        if (broken_)
        {
            bits.bad = true;
            return 0;
        }
        limits(0);
    }
    if (symbol == r16(0x24))
    {
        const std::uint32_t raw = bits.bits(r16(0x06));
        limits(add(raw));
        return raw;
    }
    return symbol;
}

int Model::add(std::uint32_t symbol)
{
    const std::uint32_t weights = r32(0x18), slots = r32(0x1C), groups = r32(0x0C),
                        symbols = r32(0x20);
    w16(weights + symbol * 2, 1);
    int count = r16(0x08);
    w32(slots + static_cast<std::uint32_t>(count) * 4, weights + symbol * 2);
    w16(0x08, ++count);
    if (count == 2)
        return 0;
    const int used = r16(0x0A), levels = r16(0x04);
    std::uint16_t group;
    if (used < levels)
    {
        group = static_cast<std::uint16_t>(used - 1);
        w16(0x0A, used + 1);
    }
    else
    {
        group = static_cast<std::uint16_t>(used - 2);
        while (s16(groups + group * 2u) == 0)
            group = static_cast<std::uint16_t>(group - 1);
    }
    w16(groups + group * 2u, s16(groups + group * 2u) - 1);
    w16(groups + 2 + group * 2u, s16(groups + 2 + group * 2u) + 2);
    w32(symbols + (group + 1u) * 4, r32(symbols + (group + 1u) * 4) - 4);
    for (int u = group + 2; u <= levels; ++u)
        w32(symbols + static_cast<std::uint32_t>(u) * 4,
            r32(symbols + static_cast<std::uint32_t>(u) * 4) + 4);
    return group;
}

void Model::rebuild()
{
    const std::uint32_t slot = r32(0x1C), symt = r32(0x20), gc = r32(0x0C);
    const int n_slots = r16(0x08), upd = s16(0x2C), num = r16(0x04);
    w16(0x2C, upd - 1);
    // Weights halved on the update boundary; the largest one noted.
    int maxw = 0;
    for (int i = 0; i < n_slots; ++i)
    {
        const std::uint32_t p = r32(slot + static_cast<std::uint32_t>(i) * 4);
        int wv = r16(p);
        if (((upd - 1) & 0xFFFF) == 0)
        {
            wv >>= 1;
            w16(p, wv);
        }
        maxw = std::max(maxw, wv);
    }
    // The slots partitioned by weight, top bit first.
    if (maxw != 0)
    {
        std::uint16_t mask = 0x8000;
        while ((maxw & mask) == 0)
            mask = static_cast<std::uint16_t>(mask >> 1 | 0x8000);
        int la = 0;
        long guard = 0;
        while (la < n_slots)
        {
            if (++guard > kRebuildGuard)
            {
                broken_ = true;
                return;
            }
            if ((r16(r32(slot + static_cast<std::uint32_t>(la) * 4)) & mask) != 0)
            {
                ++la;
                continue;
            }
            int next = la + 1, ins = la;
            if (n_slots <= next)
                break;
            int u5 = ins;
            for (; next < n_slots; ++next)
            {
                const std::uint32_t pp = slot + static_cast<std::uint32_t>(next) * 4;
                const std::uint32_t p9 = r32(pp);
                u5 = ins;
                if ((r16(p9) & mask) != 0)
                {
                    u5 = ins + 1;
                    const std::uint32_t current = r32(slot + static_cast<std::uint32_t>(ins) * 4);
                    w32(slot + static_cast<std::uint32_t>(ins) * 4, p9);
                    w32(pp, current);
                }
                ins = u5;
            }
            if (u5 != la)
                la = u5 - 1;
            const auto halved = static_cast<std::uint16_t>(mask >> 1);
            mask = static_cast<std::uint16_t>(halved | 0x8000);
            if ((halved & 1) != 0)
                break;
        }
    }
    // The groups rebalanced.
    int la = 0, n_groups = r16(0x0A), moved = 0;
    auto top = static_cast<std::uint16_t>(n_groups - 1);
    long guard = 0;
    if (n_groups != 0)
        for (;;)
        {
            if (++guard > kRebuildGuard)
            {
                broken_ = true;
                return;
            }
            const std::uint32_t pgc = gc + static_cast<std::uint32_t>(la) * 2,
                                p2 = symt + static_cast<std::uint32_t>(la) * 4;
            const std::uint16_t g = r16(pgc);
            if (g == 0)
                ++la;
            else
            {
                const std::uint32_t p1 = r32(p2 + 4);
                const std::uint16_t w_first = r16(r32(r32(p2))), w_last = r16(r32(p1 - 4)),
                                    w_last2 = r16(r32(p1 - 8));
                if (g < 3 || ((num - 1) & 0xFFFF) == la ||
                    w_first < static_cast<std::uint16_t>(w_last + w_last2))
                {
                    std::uint32_t p16 = p2 + 4;
                    bool found = false;
                    auto acc = r16(r32(p1 - 4));
                    int u17 = la + 2;
                    for (; u17 < n_groups; ++u17)
                    {
                        const std::uint32_t p4 = symt + static_cast<std::uint32_t>(u17) * 4;
                        acc = static_cast<std::uint16_t>(acc - s16(r32(r32(p4))));
                        const std::uint16_t next_weight = r16(r32(r32(p4) + 4));
                        if (r16(gc + static_cast<std::uint32_t>(u17) * 2) > 1 &&
                            ((acc & 0x8000) != 0 || acc < next_weight))
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        ++la;
                    else
                    {
                        w16(pgc, s16(pgc) - 1);
                        ++moved;
                        ++la;
                        w32(p16, r32(p16) - 4);
                        w16(gc + static_cast<std::uint32_t>(la) * 2,
                            s16(gc + static_cast<std::uint32_t>(la) * 2) + 2);
                        w32(p2 + 8, r32(p2 + 8) + 4);
                        if (la < ((u17 - 1) & 0xFFFF))
                        {
                            const int span = (u17 - 1) - la;
                            la += span;
                            for (int s = 0; s < span; ++s, p16 += 4)
                                w32(p16 + 8, r32(p16 + 8) + 4);
                        }
                        w16(gc + static_cast<std::uint32_t>(la) * 2,
                            s16(gc + static_cast<std::uint32_t>(la) * 2) + 1);
                        w32(p16 + 4, r32(p16 + 4) + 4);
                        w16(gc + 2 + static_cast<std::uint32_t>(la) * 2,
                            s16(gc + 2 + static_cast<std::uint32_t>(la) * 2) - 2);
                        if (s16(gc + top * 2u) == 0)
                        {
                            w16(0x0A, --n_groups);
                            top = static_cast<std::uint16_t>(top - 1);
                        }
                        la = 0;
                    }
                }
                else
                {
                    ++moved;
                    w16(pgc - 2, s16(pgc - 2) + 1);
                    w16(pgc, s16(pgc) - 3);
                    w16(gc + 2 + static_cast<std::uint32_t>(la) * 2,
                        s16(gc + 2 + static_cast<std::uint32_t>(la) * 2) + 2);
                    w32(p2, r32(p2) + 4);
                    w32(p2 + 4, r32(p2 + 4) - 8);
                    if (top == la)
                    {
                        w16(0x0A, ++n_groups);
                        top = static_cast<std::uint16_t>(top + 1);
                    }
                    la = 0;
                }
            }
            if (la >= n_groups)
                break;
        }
    // The update period tuned to how much moved.
    if (moved < 0x10)
    {
        if (moved < 8 && r16(0x2E) != 1)
        {
            w16(0x02, r16(0x02) << 1);
            w16(0x2C, r16(0x2C) >> 1);
            w16(0x2E, r16(0x2E) >> 1);
        }
    }
    else
    {
        w16(0x02, r16(0x32));
        w16(0x2E, r16(0x30));
    }
    w16(0x00, r16(0x02));
    if (r16(0x2C) == 0)
        w16(0x2C, r16(0x2E));
}

std::uint32_t checksum(const std::uint8_t *data, std::size_t size, int bits)
{
    const std::uint32_t mask = (1u << bits) - 1;
    std::uint32_t w = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        w ^= data[i];
        w = (w << 8 | w >> (bits - 8)) & mask;
    }
    return w;
}

bool decompress(const std::uint8_t *data, std::size_t at, std::size_t end, std::size_t size,
                std::vector<std::uint8_t> &out)
{
    Bits in{data, at, end};
    if (in.bits(16) != 0xB59C)
        return false;
    const bool raw_literals = in.bits(8) != 0;
    in.bits(8);
    const int initial = static_cast<int>(in.bits(12)), update = static_cast<int>(in.bits(12));
    const int low = in.bits(4) == 8 ? 7 : 6;
    if (in.bad)
        return false;
    Model lengths(6, 0x0C, initial, update), distances(6, 0x0C, initial, update),
        literals(8, 0x10, initial, update);
    out.clear();
    out.reserve(size);
    while (out.size() < size)
    {
        if (in.bit() == 0)
        {
            const auto byte = raw_literals ? in.bits(8) : literals.decode(in);
            if (in.bad)
                return false;
            out.push_back(static_cast<std::uint8_t>(byte));
            continue;
        }
        std::uint32_t distance = in.bits(low);
        distance |= distances.decode(in) << low;
        if (in.bad)
            return false;
        if (distance == 0)
            break;
        const std::uint32_t length = lengths.decode(in) & 0x7F;
        if (in.bad)
            return false;
        const std::size_t back = distance + 1u;
        for (std::uint32_t i = 0; i < length && out.size() < size; ++i)
        {
            const auto pos = out.size();
            out.push_back(pos >= back ? out[pos - back] : 0);
        }
    }
    return !in.bad;
}

namespace
{
struct Bytes
{
    const std::uint8_t *data;
    std::size_t size, pos = 0;
    bool bad = false;

    std::uint8_t u8()
    {
        if (pos >= size)
        {
            bad = true;
            return 0;
        }
        return data[pos++];
    }
    std::uint16_t u16()
    {
        const std::uint16_t low = u8();
        return static_cast<std::uint16_t>(low | u8() << 8);
    }
    std::uint32_t u32()
    {
        const std::uint32_t low = u16();
        return low | static_cast<std::uint32_t>(u16()) << 16;
    }
    std::int64_t vli()
    {
        const std::uint8_t lead = u8();
        int count = 0;
        for (std::uint8_t mask = 0x40; mask != 0 && (lead & mask) != 0; mask >>= 1)
            ++count;
        std::uint64_t value = lead & 0x3F;
        if (count != 0)
        {
            value = static_cast<std::uint64_t>(lead & ((0x40u >> count) - 1)) << (8 * count);
            for (int i = 0; i < count; ++i)
                value |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return (lead & 0x80) != 0 ? -static_cast<std::int64_t>(value)
                                  : static_cast<std::int64_t>(value);
    }
    std::string text()
    {
        std::size_t length = u8();
        if (length == 0xFF)
            length = u16();
        if (length == 0 || bad || length > size - pos)
        {
            bad = bad || length > size - pos;
            return {};
        }
        std::string value(reinterpret_cast<const char *>(data + pos), length - 1);
        pos += length;
        return value.substr(0, value.find('\0'));
    }
};
} // namespace

bool run_program(const std::vector<std::uint8_t> &old, const std::vector<std::uint8_t> &ops,
                 std::size_t size, int sources, std::vector<std::uint8_t> &out)
{
    Bytes in{ops.data(), ops.size()};
    out.assign(size, 0);
    std::size_t cursor = 0;
    std::int64_t poke = 0;
    std::vector<std::pair<std::size_t, std::size_t>> gaps, templates;
    bool bad = false;
    // A count or seek within the new file, or a source offset; anything else is damage.
    const auto length = [&]
    {
        const auto v = in.vli();
        if (v < 0 || static_cast<std::uint64_t>(v) > size)
            bad = true;
        return bad ? std::size_t{0} : static_cast<std::size_t>(v);
    };
    const auto offset = [&]
    {
        const auto v = in.vli();
        if (v < 0)
            bad = true;
        return bad ? std::size_t{0} : static_cast<std::size_t>(v);
    };
    const auto gap = [&](std::size_t skip)
    {
        if (skip == 0)
            return;
        if (skip > size - std::min(cursor, size))
            bad = true;
        else
        {
            gaps.emplace_back(cursor, skip);
            cursor += skip;
        }
    };
    const auto copy = [&](std::size_t from, std::size_t count)
    {
        if (bad || count > size - cursor)
        {
            bad = true;
            return;
        }
        if (from < old.size())
            std::copy_n(old.begin() + static_cast<std::ptrdiff_t>(from),
                        std::min(count, old.size() - from),
                        out.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += count;
    };
    const auto fill = [&](const std::uint8_t *pattern, std::size_t width, std::size_t count)
    {
        if (bad || count > size - cursor)
        {
            bad = true;
            return;
        }
        for (std::size_t i = 0; pattern && i < count; ++i)
            out[cursor + i] = pattern[i % width];
        cursor += count;
    };
    const auto add = [&](std::int64_t at, int width, std::int64_t delta)
    {
        if (at < 0 || static_cast<std::uint64_t>(at) + static_cast<std::uint64_t>(width) > size)
        {
            bad = true;
            return;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(
                         out[static_cast<std::size_t>(at) + static_cast<std::size_t>(i)])
                     << (8 * i);
        value += static_cast<std::uint64_t>(delta);
        for (int i = 0; i < width; ++i)
            out[static_cast<std::size_t>(at) + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto signed_bytes = [&](int width)
    {
        std::uint32_t raw = 0;
        for (int i = 0; i < width; ++i)
            raw |= static_cast<std::uint32_t>(in.u8()) << (8 * i);
        const int shift = 32 - 8 * width;
        return static_cast<std::int64_t>(static_cast<std::int32_t>(raw << shift) >> shift);
    };
    const auto seek = [&](std::int64_t step)
    {
        if (step > static_cast<std::int64_t>(size) || step < -static_cast<std::int64_t>(size))
            bad = true;
        else
            poke += step;
    };
    for (;;)
    {
        const std::uint8_t op = in.u8();
        if (in.bad)
            return false;
        if (op == 0x01)
            break;
        switch (op)
        {
        case 0x02:
            in.vli();
            cursor = 0;
            poke = 0;
            break;
        case 0x03:
        case 0x04:
        {
            gap(op == 0x04 ? length() : 0);
            if (sources >= 2)
                in.vli();
            const auto from = offset();
            copy(from, length());
            break;
        }
        case 0x05:
            if (size > cursor)
                gaps.emplace_back(cursor, size - cursor);
            for (const auto &[at, count] : gaps)
            {
                if (count > in.size - in.pos)
                    return false;
                std::copy_n(in.data + in.pos, count, out.begin() + static_cast<std::ptrdiff_t>(at));
                in.pos += count;
            }
            gaps.clear();
            break;
        case 0x06:
            seek(in.vli());
            add(poke, 1, signed_bytes(1));
            break;
        case 0x07:
        case 0x0E:
        case 0x0F:
        case 0x10:
        {
            const int width = op == 0x0F ? 2 : op == 0x10 ? 4 : 1;
            poke = 0;
            const auto delta = signed_bytes(width);
            for (auto n = length(); n > 0 && !bad && !in.bad; --n)
            {
                seek(in.vli());
                add(poke, width, delta);
            }
            break;
        }
        case 0x08:
        {
            if (sources >= 2)
                in.vli();
            const auto from = offset();
            templates.emplace_back(from, length());
            break;
        }
        case 0x09:
        case 0x0A:
        {
            gap(op == 0x0A ? length() : 0);
            const auto index = offset();
            if (index >= templates.size())
                return false;
            copy(templates[index].first, templates[index].second);
            break;
        }
        case 0x0B:
        case 0x0C:
            gap(op == 0x0C ? length() : 0);
            fill(nullptr, 0, length());
            break;
        case 0x0D:
            poke = 0;
            for (auto n = length(); n > 0 && !bad && !in.bad; --n)
            {
                seek(in.vli());
                add(poke, 1, signed_bytes(1));
            }
            break;
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        {
            gap(op >= 0x14 ? length() : 0);
            const std::size_t width = std::size_t{1} << ((op - 0x11) % 3);
            std::uint8_t pattern[4] = {};
            for (std::size_t i = 0; i < width; ++i)
                pattern[i] = in.u8();
            fill(pattern, width, length());
            break;
        }
        default:
            return false;
        }
        if (bad || in.bad)
            return false;
    }
    return true;
}
} // namespace rtpatch

namespace
{
using rtpatch::Bytes;
using rtpatch::checksum;

struct Version
{
    std::uint32_t size = 0, w1 = 0, w2 = 0;
};

struct Record
{
    bool whole = true; // NEW (else MODIFY)
    std::string path;  // with slashes
    std::size_t stream = 0, packed = 0;
    int sources = 1;
    Version from, to;
};

constexpr char kTrailer[] = "DKNJ";

bool read_all(const std::string &path, std::vector<std::uint8_t> &out)
{
    const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat info
    {
    };
    if (file < 0 || fstat(file, &info) != 0 || info.st_size <= 0)
    {
        if (file >= 0)
            close(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < out.size())
    {
        const ssize_t got =
            pread(file, out.data() + done, out.size() - done, static_cast<off_t>(done));
        if (got <= 0)
            break;
        done += static_cast<std::size_t>(got);
    }
    close(file);
    return done == out.size();
}

std::uint32_t u32_at(const std::vector<std::uint8_t> &data, std::size_t at)
{
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 |
           static_cast<std::uint32_t>(data[at + 3]) << 24;
}

// Where the payload starts: 0 for a bare one; in a self-extractor, the end of the
// program's last section, which the payload's trailer has to confirm.
bool payload_start(const std::vector<std::uint8_t> &data, std::size_t &start)
{
    if (data.size() >= 2 && data[0] == 'K' && data[1] == '*')
    {
        start = 0;
        return true;
    }
    if (data.size() < 0x200 || data[0] != 'M' || data[1] != 'Z' ||
        std::memcmp(data.data() + data.size() - 4, kTrailer, 4) != 0)
        return false;
    const std::size_t pe = u32_at(data, 0x3C);
    if (pe > data.size() - 24 || std::memcmp(data.data() + pe, "PE\0\0", 4) != 0)
        return false;
    const std::size_t sections = data[pe + 6] | data[pe + 7] << 8,
                      optional = data[pe + 20] | data[pe + 21] << 8, table = pe + 24 + optional;
    std::size_t end = 0;
    for (std::size_t i = 0; i < sections; ++i)
    {
        const std::size_t at = table + 40 * i;
        if (at + 24 > data.size())
            return false;
        end =
            std::max<std::size_t>(end, std::size_t{u32_at(data, at + 20)} + u32_at(data, at + 16));
    }
    if (end + 2 > data.size() || data[end] != 'K' || data[end + 1] != '*' ||
        u32_at(data, data.size() - 8) != end)
        return false;
    start = end;
    return true;
}

bool parse(const std::vector<std::uint8_t> &data, std::size_t start, std::vector<Record> &records,
           std::string &why)
{
    Bytes in{data.data() + start, data.size() - start, 2};
    const std::uint16_t version = in.u16(), flags = in.u16();
    const std::uint32_t ext = (flags & 0x8000) != 0 ? in.u32() : 0;
    const bool extra = (ext & 0x10000) != 0;
    const std::uint16_t default_options = in.u16();
    in.u32(), in.u32(), in.u16(), in.u16();
    if ((in.u16() & 0x4) != 0)
        in.u32();
    in.u32();
    std::uint32_t expected = 0;
    if (version >= 0x2BC)
    {
        expected = in.u32();
        in.u32();
        for (auto n = in.u16(); n > 0 && !in.bad; --n)
        {
            in.u8(), in.u16();
            in.text(), in.text();
        }
    }
    for (auto n = in.u16(); n > 0 && !in.bad; --n)
        in.text(), in.text();
    for (auto n = in.u16(); n > 0 && !in.bad; --n)
        in.text();
    const auto entry = [&](Version &v)
    {
        if (in.size - in.pos < 34)
        {
            in.bad = true;
            return;
        }
        const auto *e = in.data + in.pos;
        const auto at = [&](std::size_t i)
        {
            return static_cast<std::uint32_t>(e[i]) | static_cast<std::uint32_t>(e[i + 1]) << 8 |
                   static_cast<std::uint32_t>(e[i + 2]) << 16 |
                   static_cast<std::uint32_t>(e[i + 3]) << 24;
        };
        v = {at(16), at(26) & 0x7FFFFFFF, at(30) & 0x3FFFFFFF};
        in.pos += 34;
        if (extra)
        {
            in.pos += 8;
            in.text();
        }
    };
    for (;;)
    {
        if (in.bad)
        {
            why = "it ends early";
            return false;
        }
        const std::uint16_t header = in.u16();
        const int kind = header >> 12;
        if (kind == 1)
            break;
        if (kind != 2 && kind != 4)
        {
            why = "it holds a kind of record this doesn't handle (" + std::to_string(kind) + ")";
            return false;
        }
        const std::uint16_t options = (header & 0x2) != 0 ? in.u16() : default_options;
        Record record;
        record.whole = kind == 2;
        record.path = (header & 0x4) != 0 ? in.text() : std::string();
        std::replace(record.path.begin(), record.path.end(), '\\', '/');
        if ((options & 0xC0) != 0)
        {
            in.vli();
            if (extra)
                in.vli();
        }
        if ((header & 0x80) != 0 || (header & 0x100) != 0 || (header & 0x200) != 0)
        {
            why = record.path + " goes outside the game's folder";
            return false;
        }
        in.pos += 10;
        if (record.whole)
        {
            const auto count = in.vli();
            in.u32();
            record.packed = in.u32();
            if (count < 1 || count > 16)
            {
                why = "a damaged record";
                return false;
            }
            entry(record.to);
            for (auto n = count - 1; n > 0; --n)
            {
                Version unused;
                entry(unused);
            }
        }
        else
        {
            in.u16();
            const auto sources = in.vli(), targets = in.vli();
            in.u32();
            record.packed = in.u32();
            if (sources < 1 || sources > 16 || targets < 1 || targets > 16)
            {
                why = "a damaged record";
                return false;
            }
            record.sources = static_cast<int>(sources);
            entry(record.from);
            for (auto n = sources - 1; n > 0; --n)
            {
                Version unused;
                entry(unused);
            }
            entry(record.to);
            for (auto n = targets - 1; n > 0; --n)
            {
                Version unused;
                entry(unused);
            }
        }
        if (in.bad || record.packed > in.size - in.pos || !safe_relative(record.path))
        {
            why = in.bad || record.packed > in.size - in.pos
                      ? "it ends early"
                      : "it names " + record.path + ", which isn't a plain path";
            return false;
        }
        record.stream = start + in.pos;
        in.pos += record.packed;
        records.push_back(std::move(record));
    }
    const std::size_t rest = in.size - in.pos;
    if (in.bad ||
        (rest != 0 && !(rest == 8 && std::memcmp(in.data + in.pos + 4, kTrailer, 4) == 0)))
    {
        why = "there's more after its end";
        return false;
    }
    if (version >= 0x2BC && expected != records.size())
    {
        why = "it lists " + std::to_string(expected) + " files and holds " +
              std::to_string(records.size());
        return false;
    }
    return true;
}

bool matches(const std::vector<std::uint8_t> &bytes, const Version &v)
{
    return bytes.size() == v.size && checksum(bytes.data(), bytes.size(), 31) == v.w1 &&
           checksum(bytes.data(), bytes.size(), 30) == v.w2;
}

// Where path goes in folder: the existing file as the copy spells it, or a new one in
// its folders (made as needed).
bool place(const std::string &folder, const std::string &path, std::string &target,
           std::string &error)
{
    target = find_entry(folder, path);
    if (!target.empty())
        return true;
    target = folder;
    std::size_t start = 0;
    for (auto slash = path.find('/'); slash != std::string::npos;
         start = slash + 1, slash = path.find('/', start))
    {
        const auto part = path.substr(start, slash - start);
        const auto found = find_entry(target, part);
        target = found.empty() ? target + "/" + part : found;
        if (found.empty() && mkdir(target.c_str(), 0777) != 0 && errno != EEXIST)
        {
            error = "Can't make the folders for " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    target += "/" + path.substr(start);
    return true;
}

bool write_beside(const std::string &target, const std::vector<std::uint8_t> &bytes,
                  std::string &why)
{
    const auto building = target + ".upgrading";
    unlink(building.c_str());
    const int out = open(building.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    bool ok = out >= 0;
    std::size_t done = 0;
    while (ok && done < bytes.size())
    {
        const ssize_t step = write(out, bytes.data() + done, bytes.size() - done);
        ok = step > 0;
        done += ok ? static_cast<std::size_t>(step) : 0;
    }
    ok = ok && fsync(out) == 0;
    if (out >= 0 && close(out) != 0)
        ok = false;
    if (!ok || rename(building.c_str(), target.c_str()) != 0)
    {
        why = std::strerror(errno);
        unlink(building.c_str());
        return false;
    }
    return true;
}
} // namespace

bool apply_rtpatch(const std::string &patch, const std::string &folder,
                   const std::atomic<bool> &cancel, std::string &error)
{
    std::vector<std::uint8_t> data;
    std::size_t start = 0;
    std::vector<Record> records;
    std::string why;
    if (!read_all(patch, data))
    {
        error = "Can't read the patch";
        return false;
    }
    if (!payload_start(data, start) || !parse(data, start, records, why))
    {
        error =
            why.empty() ? "The patch isn't an RTPatch update" : "The patch can't be used: " + why;
        return false;
    }
    for (const auto &record : records)
    {
        if (cancel.load())
        {
            error = "Cancelled";
            return false;
        }
        std::string target;
        if (!place(folder, record.path, target, error))
            return false;
        std::vector<std::uint8_t> made;
        if (record.whole)
        {
            if (!rtpatch::decompress(data.data(), record.stream, record.stream + record.packed,
                                     record.to.size, made))
            {
                error = "The patch is damaged at " + record.path;
                return false;
            }
        }
        else
        {
            std::vector<std::uint8_t> old, ops;
            if (!read_all(target, old) || !matches(old, record.from))
            {
                error = record.path + " isn't the file the patch is for";
                return false;
            }
            if (!rtpatch::decompress(data.data(), record.stream, record.stream + record.packed,
                                     std::size_t{record.to.size} * 2 + 4096, ops) ||
                !rtpatch::run_program(old, ops, record.to.size, record.sources, made))
            {
                error = "The patch is damaged at " + record.path;
                return false;
            }
        }
        if (!matches(made, record.to))
        {
            error = record.path + " didn't come out as the patch says";
            return false;
        }
        if (!write_beside(target, made, why))
        {
            error = "Can't put " + record.path + " in place: " + why;
            return false;
        }
    }
    return true;
}
} // namespace store::library

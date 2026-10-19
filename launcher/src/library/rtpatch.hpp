// Prospero BlackBox - A publisher's RTPatch update applied without running it.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace store::library
{
// A publisher can ship a game update as an RTPatch self-extractor (Patch.exe): a Windows
// program with the patch appended. The console can't run it, and we wouldn't: the patch
// is found after the program, parsed and decoded here (docs/rtpatch-format.md).
//
// Applies the patch (the .exe, or the bare payload) to the game's folder (a copy, never
// the base copy itself). Each file is built beside the one it replaces and checked
// against the size and checksums the patch records for it before it's renamed into
// place; a file the patch changes (rather than carries whole) has the old one checked
// first. False with error set, the folder then half-changed (the caller throws the copy
// away).
bool apply_rtpatch(const std::string &patch, const std::string &folder,
                   const std::atomic<bool> &cancel, std::string &error);

// The pieces, for the tests.
namespace rtpatch
{
// MSB-first bit reader over [data + at, data + end).
struct Bits
{
    const std::uint8_t *data = nullptr;
    std::size_t pos = 0, end = 0;
    int left = 8; // bits left in data[pos]
    bool bad = false;

    int bit();
    std::uint32_t bits(int count);
};

// One adaptive Huffman alphabet of the 0xB59C codec, in the original decoder's flat
// table layout (the layout is what keeps it byte-exact).
class Model
{
  public:
    Model(int escape_bits, int levels, int initial_period, int update_period);
    // The next symbol; an escape is followed by the symbol's raw bits, which are read and
    // the symbol learnt. Sets bits.bad on running out (or on a model a damaged stream
    // broke).
    std::uint32_t decode(Bits &bits);
    // Whether symbol has been seen (decodes without the escape).
    bool known(std::uint32_t symbol) const;

  private:
    std::vector<std::uint8_t> m_;
    bool broken_ = false;

    std::uint16_t r16(std::uint32_t at) const;
    std::int16_t s16(std::uint32_t at) const;
    void w16(std::uint32_t at, int value);
    std::uint32_t r32(std::uint32_t at) const;
    void w32(std::uint32_t at, std::uint32_t value);
    void limits(int start);
    int add(std::uint32_t symbol);
    void rebuild();
};

// The rolling checksum the patch records: for each byte, w = rotl8(w ^ byte) within bits.
std::uint32_t checksum(const std::uint8_t *data, std::size_t size, int bits);
// A 0xB59C stream at [at, end) decoded to at most size bytes; false if it's damaged.
bool decompress(const std::uint8_t *data, std::size_t at, std::size_t end, std::size_t size,
                std::vector<std::uint8_t> &out);
// A MODIFY record's program run against the old file into size bytes; false if damaged.
bool run_program(const std::vector<std::uint8_t> &old, const std::vector<std::uint8_t> &ops,
                 std::size_t size, int sources, std::vector<std::uint8_t> &out);
} // namespace rtpatch
} // namespace store::library

// Prospero BlackBox - RTPatch updates read and applied, on patches this test makes itself.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No publisher's bytes: every stream here comes from the small encoder below, which finds
// each symbol's code by asking the decoder's own model (it tries codes, shortest first,
// on a copy of the model). Run with a folder argument, it also leaves the patch, the old
// files and the expected new ones there for tools/rtpatch.py to be checked against.
#include "library/rtpatch.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace store::library;
using Bytes = std::vector<std::uint8_t>;

namespace
{
// ---- the codec, backwards -------------------------------------------------------

struct BitWriter
{
    Bytes bytes;
    std::size_t count = 0; // bits written

    void put(std::uint32_t value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i)
        {
            if (count % 8 == 0)
                bytes.push_back(0);
            if ((value >> i) & 1)
                bytes.back() |= static_cast<std::uint8_t>(0x80 >> (count % 8));
            ++count;
        }
    }
};

// Writes symbol with the code model gives it now (after an escape, its raw bits), and
// moves model on as the decoder will. The codes are walked in order: decoding a prefix
// followed by zeros gives the first code starting with it, and one more than that code
// is the prefix of the next.
void encode(rtpatch::Model &model, BitWriter &out, std::uint32_t symbol, int escape_bits)
{
    const bool known = model.known(symbol);
    const int offset = static_cast<int>(out.count % 8);
    const auto attempt = [&](std::uint64_t code, int length, bool raw, rtpatch::Model &copy,
                             std::uint32_t &got, int &used)
    {
        BitWriter trial;
        if (offset != 0)
            trial.put(out.bytes.back() >> (8 - offset), offset);
        trial.put(static_cast<std::uint32_t>(code), length);
        if (raw)
            trial.put(symbol, escape_bits);
        trial.put(0, 32);
        rtpatch::Bits bits{trial.bytes.data(), 0, trial.bytes.size(), 8 - offset};
        got = copy.decode(bits);
        used = static_cast<int>(bits.pos * 8 + static_cast<std::size_t>(8 - bits.left)) - offset;
        return !bits.bad;
    };
    std::uint64_t code = 0;
    int length = 1;
    for (int step = 0; step < 600; ++step)
    {
        auto copy = model;
        std::uint32_t got = 0;
        int used = 0;
        assert(attempt(code, length, false, copy, got, used));
        const bool escape = !model.known(got); // the escape: its raw bits came from the zeros
        const int code_length = escape ? used - escape_bits : used;
        // Longer codes have the smaller values, so the next code can be shorter: then the
        // candidate starts with it.
        assert(code_length > 0);
        code =
            code_length >= length ? code << (code_length - length) : code >> (length - code_length);
        length = code_length;
        if (escape == !known && (escape || got == symbol))
        {
            auto taken = model;
            assert(attempt(code, length, escape, taken, got, used) && got == symbol);
            model = taken;
            out.put(static_cast<std::uint32_t>(code), length);
            if (escape)
                out.put(symbol, escape_bits);
            return;
        }
        ++code;
        assert(code < (std::uint64_t{1} << length)); // past the last code
    }
    assert(!"no code found");
}

struct Token
{
    std::uint8_t literal = 0;
    std::uint32_t distance = 0,
                  length = 0; // a copy when length != 0; distance 0 + length 1: the end
};

Bytes compress(const std::vector<Token> &tokens, bool raw_literals)
{
    BitWriter out;
    out.put(0xB59C, 16);
    out.put(raw_literals ? 1 : 0, 8);
    out.put(0xFF, 8);
    out.put(64, 12);
    out.put(16, 12);
    out.put(8, 4);
    rtpatch::Model lengths(6, 0x0C, 64, 16), distances(6, 0x0C, 64, 16), literals(8, 0x10, 64, 16);
    for (const auto &token : tokens)
    {
        if (token.length == 0)
        {
            out.put(0, 1);
            if (raw_literals)
                out.put(token.literal, 8);
            else
                encode(literals, out, token.literal, 8);
            continue;
        }
        out.put(1, 1);
        out.put(token.distance & 0x7F, 7);
        encode(distances, out, token.distance >> 7, 6);
        if (token.distance != 0)
            encode(lengths, out, token.length, 6);
    }
    out.put(0, 32); // the real streams run on a little too
    return out.bytes;
}

// Tokens for data: back-references where the last 600 bytes repeat, else literals.
std::vector<Token> tokenize(const Bytes &data, bool end)
{
    std::vector<Token> tokens;
    for (std::size_t at = 0; at < data.size();)
    {
        std::size_t best = 0, from = 0;
        for (std::size_t back = 1; back <= std::min<std::size_t>(at, 600); ++back)
        {
            std::size_t n = 0;
            while (at + n < data.size() && n < 63 && data[at + n] == data[at - back + n])
                ++n;
            if (n > best)
            {
                best = n;
                from = back;
            }
        }
        if (best >= 3)
        {
            tokens.push_back(
                {0, static_cast<std::uint32_t>(from - 1), static_cast<std::uint32_t>(best)});
            at += best;
        }
        else
            tokens.push_back({data[at++], 0, 0});
    }
    if (end)
        tokens.push_back({0, 0, 1});
    return tokens;
}

Bytes sample(std::size_t size, std::uint32_t seed)
{
    Bytes data(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        seed = seed * 1103515245 + 12345;
        // Mostly a few repeating phrases, some noise: escapes, copies and rebuilds all happen.
        data[i] = (i / 37) % 8 == 0 ? static_cast<std::uint8_t>(seed >> 16)
                                    : static_cast<std::uint8_t>("phrase-"[i % 7] + (i / 200) % 4);
    }
    return data;
}

// ---- the container --------------------------------------------------------------

struct Out
{
    Bytes b;
    void u8(int v)
    {
        b.push_back(static_cast<std::uint8_t>(v));
    }
    void u16(int v)
    {
        u8(v & 0xFF);
        u8(v >> 8 & 0xFF);
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<int>(v >> (8 * i) & 0xFF));
    }
    void text(const std::string &s)
    {
        u8(static_cast<int>(s.size() + 1));
        b.insert(b.end(), s.begin(), s.end());
        u8(0);
    }
    void vli(std::int64_t value)
    {
        const int sign = value < 0 ? 0x80 : 0;
        auto v = static_cast<std::uint64_t>(value < 0 ? -value : value);
        if (v < 64)
            return u8(sign | static_cast<int>(v));
        for (int count = 1; count <= 3; ++count)
            if (v >> (8 * count) < (0x40u >> count))
            {
                u8(sign | ((0x80 - (0x80 >> count)) & 0x7F) | static_cast<int>(v >> (8 * count)));
                for (int i = 0; i < count; ++i)
                    u8(static_cast<int>(v >> (8 * i) & 0xFF));
                return;
            }
        assert(!"too big");
    }
    void add(const Bytes &more)
    {
        b.insert(b.end(), more.begin(), more.end());
    }
};

void entry(Out &out, const std::string &name, const Bytes &file)
{
    Bytes e(34, 0);
    std::memcpy(e.data(), name.data(), std::min<std::size_t>(name.size(), 13));
    const auto put32 = [&](std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            e[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(16, static_cast<std::uint32_t>(file.size()));
    put32(26, rtpatch::checksum(file.data(), file.size(), 31));
    put32(30, rtpatch::checksum(file.data(), file.size(), 30));
    out.add(e);
    out.add(Bytes(8, 0)); // times
    out.text(name);
}

struct File
{
    std::string path;
    Bytes old, now;      // old empty: carried whole
    Bytes program;       // for a changed file: the program, uncompressed
    bool options = true; // the option-override form (with its two VLIs)
};

Bytes payload(const std::vector<File> &files, std::size_t exe_offset, bool corrupt_check = false)
{
    Out out;
    out.u8('K');
    out.u8('*');
    out.u16(0x02BC);
    out.u16(0x8000);
    out.u32(0x10000);
    out.u16(0x0030);
    out.u32(0), out.u32(0), out.u16(0), out.u16(0), out.u16(0), out.u32(0);
    out.u32(static_cast<std::uint32_t>(files.size()));
    out.u32(0x12345678);
    out.u16(1);
    out.u8(0), out.u16(4);
    out.text("SOFTWARE\\Test\\Game");
    out.text("Install Dir");
    out.u16(0);
    out.u16(1);
    out.text("DATA");
    for (const auto &file : files)
    {
        auto backslashed = file.path;
        std::replace(backslashed.begin(), backslashed.end(), '/', '\\');
        const auto name = file.path.substr(file.path.rfind('/') + 1);
        const bool whole = file.old.empty();
        out.u16((whole ? 0x2000 : 0x4000) | 0x0444 | (file.options ? 0x0002 : 0));
        if (file.options)
            out.u16(0x01B0);
        out.text(backslashed);
        if (file.options)
            out.vli(0), out.vli(0);
        out.add(Bytes(10, 0));
        Bytes target = file.now;
        if (corrupt_check)
            target.back() ^= 1; // the recorded checksum then isn't the result's
        if (whole)
        {
            const auto stream = compress(tokenize(file.now, false), false);
            out.vli(1);
            out.u32(static_cast<std::uint32_t>(file.now.size()));
            out.u32(static_cast<std::uint32_t>(stream.size()));
            entry(out, name, target);
            out.add(stream);
        }
        else
        {
            const auto stream = compress(tokenize(file.program, true), false);
            out.u16(0);
            out.vli(1), out.vli(1);
            out.u32(0);
            out.u32(static_cast<std::uint32_t>(stream.size()));
            entry(out, name, file.old);
            entry(out, name, target);
            out.add(stream);
        }
    }
    out.u16(0x1000);
    if (exe_offset != 0)
    {
        out.u32(static_cast<std::uint32_t>(exe_offset));
        out.b.insert(out.b.end(), {'D', 'K', 'N', 'J'});
    }
    return out.b;
}

// A minimal PE32 with one section, the payload after it.
Bytes exe(const Bytes &body)
{
    Bytes pe(0x400, 0);
    pe[0] = 'M', pe[1] = 'Z', pe[0x3C] = 0x40;
    std::memcpy(pe.data() + 0x40, "PE\0\0", 4);
    pe[0x40 + 6] = 1; // one section, no optional header
    const std::size_t section = 0x40 + 24;
    pe[section + 16 + 1] = 0x02; // raw size 0x200
    pe[section + 20 + 1] = 0x02; // at 0x200
    pe.insert(pe.end(), body.begin(), body.end());
    return pe;
}

// ---- a MODIFY program and what it makes ------------------------------------------

Bytes old_file()
{
    Bytes old(256);
    for (std::size_t i = 0; i < old.size(); ++i)
        old[i] = static_cast<std::uint8_t>(i * 7 + 3);
    return old;
}

void program(Out &ops)
{
    ops.u8(0x02), ops.vli(0);                           // the one source
    ops.u8(0x08), ops.vli(10), ops.vli(5);              // template 0: old[10..15)
    ops.u8(0x03), ops.vli(0), ops.vli(16);              // [0,16) = old[0..16)
    ops.u8(0x04), ops.vli(4), ops.vli(100), ops.vli(8); // gap [16,20); [20,28) = old[100..108)
    ops.u8(0x0B), ops.vli(4);                           // [28,32) zero
    ops.u8(0x11), ops.u8(0xAA), ops.vli(3);             // [32,35) AA
    ops.u8(0x12), ops.u8('x'), ops.u8('y'), ops.vli(5); // [35,40) xyxyx
    ops.u8(0x0A), ops.vli(2), ops.vli(0);               // gap [40,42); [42,47) template 0
    ops.u8(0x0F), ops.u16(0x0101), ops.vli(2), ops.vli(0), ops.vli(4);    // +0x0101 at 0 and 4
    ops.u8(0x06), ops.vli(20), ops.u8(0xFF);                              // -1 at 4 + 20
    ops.u8(0x13), ops.u8(1), ops.u8(2), ops.u8(3), ops.u8(4), ops.vli(6); // [47,53) 010203040102
    ops.u8(0x0C), ops.vli(1), ops.vli(300); // gap [53,54); [54,354) zero
    ops.u8(0x05);                           // the gaps, in order, then the tail
    for (const char c : std::string("GAP!hiT0123456789"))
        ops.u8(c);
    ops.u8(0x01);
}

Bytes expected_program_result()
{
    const auto old = old_file();
    Bytes now(364, 0);
    std::copy_n(old.begin(), 16, now.begin());
    std::copy_n("GAP!", 4, now.begin() + 16);
    std::copy_n(old.begin() + 100, 8, now.begin() + 20);
    std::fill_n(now.begin() + 32, 3, 0xAA);
    std::copy_n("xyxyx", 5, now.begin() + 35);
    std::copy_n("hi", 2, now.begin() + 40);
    std::copy_n(old.begin() + 10, 5, now.begin() + 42);
    const auto add16 = [&](std::size_t at, int delta)
    {
        const int v = (now[at] | now[at + 1] << 8) + delta;
        now[at] = static_cast<std::uint8_t>(v), now[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    add16(0, 0x0101);
    add16(4, 0x0101);
    now[24] = static_cast<std::uint8_t>(now[24] - 1);
    const std::uint8_t four[] = {1, 2, 3, 4, 1, 2};
    std::copy_n(four, 6, now.begin() + 47);
    now[53] = 'T';
    std::copy_n("0123456789", 10, now.begin() + 354);
    return now;
}

void put(const fs::path &path, const Bytes &body)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(body.data()),
               static_cast<std::streamsize>(body.size()));
}

Bytes read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

bool apply(const fs::path &patch, const fs::path &folder, std::string &error)
{
    const std::atomic<bool> cancel{false};
    return apply_rtpatch(patch.string(), folder.string(), cancel, error);
}
} // namespace

int main(int argc, char **argv)
{
    // The checksum, by hand: one byte 1 becomes 0x100; four FF bytes within 30 bits go
    // FF00, FFFF00, 3FFFFF03 (the top wrapping round), 3FFFFCFF.
    const Bytes one{1}, tall{0xFF, 0xFF, 0xFF, 0xFF};
    assert(rtpatch::checksum(one.data(), 1, 31) == 0x100);
    assert(rtpatch::checksum(tall.data(), 4, 30) == 0x3FFFFCFF);

    // The codec both ways: Huffman literals (escapes, rebuilds, copies) and raw ones.
    for (const bool raw : {false, true})
    {
        const auto data = sample(3000, raw ? 7 : 1);
        const auto stream = compress(tokenize(data, true), raw);
        Bytes out;
        assert(rtpatch::decompress(stream.data(), 0, stream.size(), data.size() + 100, out));
        assert(out == data); // stopped by the end token, not the size
        Bytes cut;
        assert(!rtpatch::decompress(stream.data(), 0, stream.size() / 2, data.size(), cut));
    }
    Bytes nothing;
    const Bytes not_a_stream{0x12, 0x34, 0, 0, 0, 0};
    assert(!rtpatch::decompress(not_a_stream.data(), 0, not_a_stream.size(), 10, nothing));

    // A program on its own, then damaged ones.
    {
        Out ops;
        program(ops);
        Bytes made;
        assert(rtpatch::run_program(old_file(), ops.b, 364, 1, made) &&
               made == expected_program_result());
        auto unknown = ops.b;
        unknown[0] = 0x40;
        assert(!rtpatch::run_program(old_file(), unknown, 364, 1, made));
        const Bytes past_end{0x03, 0x00, 0x7F, 0x01}; // 63 bytes into a 10-byte file
        assert(!rtpatch::run_program(old_file(), past_end, 10, 1, made));
        const Bytes no_template{0x09, 0x00, 0x01};
        assert(!rtpatch::run_program(old_file(), no_template, 10, 1, made));
        const Bytes short_flush{0x05, 'a', 0x01};
        assert(!rtpatch::run_program(old_file(), short_flush, 10, 1, made));
    }

    // Whole patches: a new file, a replaced one (the short record form), a changed one.
    char made_root[] = "/tmp/prospero-rtpatch-XXXXXX";
    const fs::path root = mkdtemp(made_root);
    Out ops;
    program(ops);
    const std::vector<File> files{
        {"DATA/new.bin", {}, sample(5000, 3), {}, true},
        {"replaced.dll", {}, sample(700, 4), {}, false},
        {"DATA/changed.bin", old_file(), expected_program_result(), ops.b, true},
    };
    const auto game = root / "game";
    const auto fresh = [&]
    {
        fs::remove_all(game);
        put(game / "data" / "changed.bin", old_file()); // spelled differently from the patch
        put(game / "REPLACED.DLL", Bytes(10, 1));
        put(game / "other.txt", Bytes{'k'});
    };
    const auto patch_exe = root / "patch.exe", patch_rtp = root / "patch.rtp";
    put(patch_rtp, payload(files, 0));
    put(patch_exe, exe(payload(files, 0x400)));
    for (const auto &patch : {patch_exe, patch_rtp})
    {
        fresh();
        std::string error;
        assert(apply(patch, game, error) && error.empty());
        assert(read(game / "data" / "new.bin") == files[0].now);
        assert(read(game / "REPLACED.DLL") == files[1].now);
        assert(read(game / "data" / "changed.bin") == expected_program_result());
        assert(read(game / "other.txt") == Bytes{'k'});
        assert(!fs::exists(game / "DATA") || fs::equivalent(game / "DATA", game / "data"));
        for (const auto &entry : fs::recursive_directory_iterator(game))
            assert(entry.path().extension() != ".upgrading");
    }

    // Refused: the wrong old file, a result that fails its checksum, damage, paths out.
    {
        std::string error;
        fresh();
        put(game / "data" / "changed.bin", Bytes(256, 9));
        assert(!apply(patch_rtp, game, error) &&
               error.find("isn't the file the patch is for") != std::string::npos);

        fresh();
        const auto bad_check = root / "bad-check.rtp";
        put(bad_check, payload(files, 0, true));
        assert(!apply(bad_check, game, error) &&
               error.find("didn't come out") != std::string::npos);

        auto cut = read(patch_rtp);
        cut.resize(cut.size() - 40);
        put(root / "cut.rtp", cut);
        assert(!apply(root / "cut.rtp", game, error));

        auto broken = read(patch_rtp);
        broken[broken.size() / 2] ^= 0x5A;
        put(root / "broken.rtp", broken);
        fresh();
        assert(!apply(root / "broken.rtp", game, error));

        auto outside = files;
        outside[0].path = "../escape.bin";
        put(root / "outside.rtp", payload(outside, 0));
        fresh();
        assert(!apply(root / "outside.rtp", game, error) && !fs::exists(root / "escape.bin"));

        auto exe_moved = exe(payload(files, 0x300)); // the trailer disagrees with the PE
        put(root / "moved.exe", exe_moved);
        assert(!apply(root / "moved.exe", game, error) &&
               error == "The patch isn't an RTPatch update");

        put(root / "text.txt", Bytes{'h', 'i'});
        assert(!apply(root / "text.txt", game, error));
        assert(!apply(root / "missing.exe", game, error) && error == "Can't read the patch");
    }

    // For tools/rtpatch.py: the same patch, the old files and the expected results.
    if (argc > 1)
    {
        const fs::path out = argv[1];
        fs::remove_all(out);
        fs::create_directories(out);
        fs::copy_file(patch_exe, out / "patch.exe");
        fresh();
        fs::copy(game, out / "old", fs::copy_options::recursive);
        put(out / "expected" / "DATA" / "new.bin", files[0].now);
        put(out / "expected" / "replaced.dll", files[1].now);
        put(out / "expected" / "DATA" / "changed.bin", expected_program_result());
    }
    fs::remove_all(root);
    std::cout << "RTPatch checks passed\n";
}

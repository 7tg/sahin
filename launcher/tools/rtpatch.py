#!/usr/bin/env python3
# Prospero BlackBox - Apply an RTPatch self-extractor without running it.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The 0xB59C codec below (bit reader, adaptive Huffman model, LZSS loop) follows
# rtptool (https://github.com/bwrsandman/rtptool, MIT, (c) 2026 Sandy Carter) by way
# of fighters-codex's port (https://github.com/jomkz/fighters-codex, lib/src/rtpatch.cpp,
# MIT, (c) 2026 John McKenzie). Their MIT notices apply to that part:
#
#   Permission is hereby granted, free of charge, to any person obtaining a copy of
#   this software and associated documentation files (the "Software"), to deal in the
#   Software without restriction, including without limitation the rights to use, copy,
#   modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
#   and to permit persons to whom the Software is furnished to do so, subject to the
#   following conditions: The above copyright notice and this permission notice shall
#   be included in all copies or substantial portions of the Software.
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
"""Read an RTPatch update (the .exe self-extractor, or its bare .rtp) and write the files it carries.

    rtpatch.py PATCH OLD_TREE OUT_TREE      apply: OUT_TREE = OLD_TREE's changed files, patched
    rtpatch.py PATCH --list                 show the container

No code from the patch is run: the payload is found after the exe's PE image, parsed,
and decoded here. docs/rtpatch-format.md describes the format. NEW records (a whole
file) and MODIFY records (copy/poke/fill program against the old file) are handled;
each result is checked against the size and the two rolling checksums the patch
records for it. OUT_TREE receives only the files the patch writes.
"""
import argparse, os, struct, sys

MAGIC = b"K*"
STREAM_MAGIC = 0xB59C


# ---- where the payload is -------------------------------------------------------

TRAILER = b"DKNJ"


def payload_offset(data):
    """0 for a bare .rtp; for a PE self-extractor, the end of its last section.

    The payload ends with u32 offset + "DKNJ", the offset being where it starts in the
    exe; both ways of finding it have to agree."""
    if data[:2] == MAGIC:
        return 0
    if data[:2] != b"MZ":
        raise ValueError("neither an RTPatch file nor a Windows executable")
    if data[-4:] != TRAILER:
        raise ValueError("no RTPatch trailer at the end of the executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE header")
    sections, optional = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    table, end = pe + 24 + optional, 0
    for i in range(sections):
        raw_size, raw_at = struct.unpack_from("<II", data, table + 40 * i + 16)
        end = max(end, raw_at + raw_size)
    if data[end:end + 2] != MAGIC or struct.unpack_from("<I", data, len(data) - 8)[0] != end:
        raise ValueError("no RTPatch payload after the executable (at 0x%x)" % end)
    return end


# ---- container ------------------------------------------------------------------

class Reader:
    def __init__(self, data, at):
        self.d, self.p = data, at

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def lp(self):
        """Length-prefixed string; the length counts the closing NUL (0xFF escapes to a u16)."""
        n = self.u8()
        if n == 0xFF:
            n = self.u16()
        raw = self.d[self.p:self.p + n]
        self.p += n
        return raw[:-1].decode("latin-1") if n else ""

    def vli(self):
        b = self.u8()
        count, mask = 0, 0x40
        while mask and b & mask:
            count += 1
            mask >>= 1
        if count == 0:
            value = b & 0x3F
        else:
            value = (b & ((0x40 >> count) - 1)) << (8 * count)
            for i in range(count):
                value |= self.u8() << (8 * i)
        return -value if b & 0x80 else value


def read_entry(r, extra):
    """A file version: 24-byte descriptor, 10-byte checksum block, (extra mode) times and name."""
    at = r.p
    short = r.d[at:at + 14].split(b"\0")[0].decode("latin-1")
    size = struct.unpack_from("<I", r.d, at + 16)[0]
    w1 = struct.unpack_from("<I", r.d, at + 26)[0] & 0x7FFFFFFF
    w2 = struct.unpack_from("<I", r.d, at + 30)[0] & 0x3FFFFFFF
    r.p = at + 34
    name = short
    if extra:
        r.p += 8
        name = r.lp() or short
    return {"name": name, "size": size, "w1": w1, "w2": w2}


def parse(data, base):
    r = Reader(data, base + 2)
    head = {"version": r.u16(), "flags": r.u16()}
    ext = r.u32() if head["flags"] & 0x8000 else 0
    extra = bool(ext & 0x10000)
    option_default = r.u16()
    r.u32(), r.u32(), r.u16(), r.u16()  # total size, reserved, default attributes, reserved
    if r.u16() & 0x4:  # command flags: bit 2 adds a combine id
        r.u32()
    r.u32()
    if head["version"] >= 0x2BC:
        # 7.00: record count, an unexplained word, then where to find the install
        # (registry key and value) before the system-file and folder tables.
        head["records"], head["stamp"] = r.u32(), r.u32()
        head["locations"] = []
        for _ in range(r.u16()):
            r.u8(), r.u16()
            head["locations"].append((r.lp(), r.lp()))
    head["specials"] = [(r.lp(), r.lp()) for _ in range(r.u16())]
    head["dirs"] = [r.lp() for _ in range(r.u16())]
    records = []
    while True:
        at = r.p
        hdr = r.u16()
        kind = hdr >> 12
        if kind == 1:
            break
        if kind not in (2, 4):
            raise ValueError("record type %d at 0x%x isn't handled" % (kind, at - base))
        option = r.u16() if hdr & 0x2 else option_default
        path = r.lp() if hdr & 0x4 else ""
        if option & 0xC0:
            r.vli()
            if extra:
                r.vli()
        if hdr & 0x80:
            r.vli()
        if hdr & 0x100:
            r.u16()
        if hdr & 0x200:
            r.lp(), r.lp()
        r.p += 10
        rec = {"at": at - base, "path": path}
        if kind == 2:  # NEW: the whole file, compressed
            count = r.vli()
            rec.update(kind="new", size=r.u32(), csize=r.u32())
            rec["dst"] = [read_entry(r, extra) for _ in range(count)][0]
            rec["src"] = None
        else:  # MODIFY: a program against the old file
            r.u16()
            src_count, dst_count = r.vli(), r.vli()
            r.u32()
            rec.update(kind="modify", csize=r.u32())
            srcs = [read_entry(r, extra) for _ in range(src_count)]
            rec["dst"] = [read_entry(r, extra) for _ in range(dst_count)][0]
            rec.update(src=srcs[0], src_count=src_count, size=rec["dst"]["size"])
        rec["stream"] = r.p
        r.p += rec["csize"]
        records.append(rec)
    # After the end record: nothing, or the 8-byte trailer (u32 offset in the exe, "DKNJ").
    rest = data[r.p:]
    if rest and not (len(rest) == 8 and rest[4:] == TRAILER):
        raise ValueError("%d unexplained bytes after the end record" % len(rest))
    if head.get("records", len(records)) != len(records):
        raise ValueError("the header says %d records, found %d" % (head["records"], len(records)))
    return head, records


# ---- the 0xB59C codec -----------------------------------------------------------

class Bits:
    def __init__(self, data, at, end):
        self.d, self.pos, self.end, self.left = data, at, end, 8

    def bit(self):
        if self.pos >= self.end:
            raise ValueError("compressed stream ends early")
        v = (self.d[self.pos] >> (self.left - 1)) & 1
        self.left -= 1
        if self.left == 0:
            self.pos += 1
            self.left = 8
        return v

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v


class Model:
    """Adaptive Huffman model in the decoder's own flat table layout (see the notice above)."""

    def __init__(self, esc_bits, levels, init_period, upd_period):
        alpha = 1 << esc_bits
        self.gc = 0x34
        self.st = levels * 2 + 0x34
        self.slot = levels * 6 + 0x38
        self.wt = levels * 6 + 0x40 + alpha * 4
        self.lim = self.wt + 4 + alpha * 2
        self.m = bytearray(self.lim + 0xC0 + 0x100)
        w16, w32 = self.w16, self.w32
        for o in (0x32, 0x02, 0x00):
            w16(o, init_period)
        for o in (0x30, 0x2E, 0x2C):
            w16(o, upd_period)
        w16(0x06, esc_bits)
        w16(0x04, levels)
        w32(0x0C, self.gc)
        w32(0x20, self.st)
        w32(0x1C, self.slot)
        w32(0x18, self.wt)
        w32(0x14, self.lim)
        w32(0x10, self.lim)
        w16(0x0A, 1)
        w16(0x08, 1)
        w16(self.gc, 2)
        w32(self.st, self.slot)
        for i in range(1, levels + 1):
            w32(self.st + i * 4, self.slot + 8)
        base = self.wt + alpha * 2
        w32(self.slot, base)
        for i in range(1, alpha + 2):
            w32(self.slot + i * 4, base + 2)
        for j in range(alpha):
            w16(self.wt + j * 2, 0x8000)
        w16(0x24, alpha)
        self.limits(0)

    def r16(self, o):
        m = self.m
        return m[o] | (m[o + 1] << 8) if o + 1 < len(m) else 0

    def s16(self, o):
        v = self.r16(o)
        return v - 0x10000 if v & 0x8000 else v

    def w16(self, o, v):
        if o + 1 < len(self.m):
            v &= 0xFFFF
            self.m[o] = v & 0xFF
            self.m[o + 1] = v >> 8

    def r32(self, o):
        return struct.unpack_from("<I", self.m, o)[0] if o + 3 < len(self.m) else 0

    def w32(self, o, v):
        if o + 3 < len(self.m):
            struct.pack_into("<I", self.m, o, v & 0xFFFFFFFF)

    def limits(self, start):
        gc, lim, num = self.r32(0x0C), self.r32(0x10), self.r16(0x04)
        sx = lambda v: ((v & 0xFFFF) ^ 0x8000) - 0x8000
        s3 = 2 if start == 0 else sx(self.s16(lim + (start - 1) * 8) * 2)
        for level in range(start, num):
            s4 = sx(s3 - self.s16(gc + level * 2))
            self.w16(lim + level * 8, s4)
            s3 = sx(s4 * 2)
            self.w16(lim + level * 8 + 2, s3)
            self.w16(lim + level * 8 + 4, s4 * 4)
            self.w16(lim + level * 8 + 6, s4 * 16)

    def decode(self, bi):
        if bi.pos >= bi.end:
            raise ValueError("compressed stream ends early")
        left = bi.left
        val = ((1 << left) - 1) & bi.d[bi.pos]
        idx, tot = left - 1, left
        lim = self.r32(0x10)
        if val < self.r16(lim + idx * 8):
            while True:
                bi.pos += 1
                if bi.pos >= bi.end:
                    raise ValueError("compressed stream ends early")
                idx += 8
                tot += 8
                val = (((val & 0xFF) << 8) | bi.d[bi.pos]) & 0xFFFF
                if val >= self.r16(lim + idx * 8):
                    break
        idx -= 1
        cnt = (tot - 1) & 0xFF
        shifted = 0
        while cnt:
            at = lim + 2 + idx * 8
            if val < (self.r16(at) if at < len(self.m) - 1 else 0):
                break
            idx -= 1
            cnt -= 1
            val >>= 1
            shifted += 1
        level = (idx + 1) & 0xFFFF
        slots = self.r32(self.r32(0x20) + level * 4)
        k = (val - (self.s16(lim + level * 8) & 0xFFFF)) & 0xFFFF
        sym = ((self.r32(slots + k * 4) - self.r32(0x18)) & 0xFFFFFFFF) >> 1
        if shifted == 0:
            shifted = 8
            bi.pos += 1
        bi.left = shifted
        w = self.r32(0x18)
        self.w16(w + sym * 2, self.r16(w + sym * 2) + 1)
        countdown = self.s16(0x00) - 1
        self.w16(0x00, countdown)
        if countdown & 0xFFFF == 0:
            self.rebuild()
            self.limits(0)
        if sym == self.r16(0x24):
            raw = bi.bits(self.r16(0x06))
            self.limits(self.add(raw))
            return raw
        return sym

    def add(self, sym):
        w, slot, gc, st = self.r32(0x18), self.r32(0x1C), self.r32(0x0C), self.r32(0x20)
        self.w16(w + sym * 2, 1)
        n = self.r16(0x08)
        self.w32(slot + n * 4, w + sym * 2)
        n += 1
        self.w16(0x08, n)
        if n == 2:
            return 0
        groups, num = self.r16(0x0A), self.r16(0x04)
        if groups < num:
            g = (groups - 1) & 0xFFFF
            self.w16(0x0A, groups + 1)
        else:
            g = (groups - 2) & 0xFFFF
            while self.s16(gc + g * 2) == 0:
                g = (g - 1) & 0xFFFF
        self.w16(gc + g * 2, self.s16(gc + g * 2) - 1)
        self.w16(gc + 2 + g * 2, self.s16(gc + 2 + g * 2) + 2)
        self.w32(st + (g + 1) * 4, self.r32(st + (g + 1) * 4) - 4)
        for u in range(g + 2, num + 1):
            self.w32(st + u * 4, self.r32(st + u * 4) + 4)
        return g

    def rebuild(self):
        r16, s16, r32, w16, w32 = self.r16, self.s16, self.r32, self.w16, self.w32
        slot, symt, gc = r32(0x1C), r32(0x20), r32(0x0C)
        n_slots, upd, num = r16(0x08), s16(0x2C), r16(0x04)
        w16(0x2C, upd - 1)
        halve = ((upd - 1) & 0xFFFF) == 0
        maxw = 0
        for i in range(n_slots):
            p = r32(slot + i * 4)
            wv = r16(p)
            if halve:
                wv >>= 1
                w16(p, wv)
            maxw = max(maxw, wv)
        # Partition the slots by weight, top bit first.
        if maxw:
            mask = 0x8000
            while not maxw & mask:
                mask = (mask >> 1) | 0x8000
            la = 0
            while la < n_slots:
                if r16(r32(slot + la * 4)) & mask == 0:
                    nxt, ins = la + 1, la
                    if n_slots <= nxt:
                        break
                    u5 = ins
                    while nxt < n_slots:
                        pp = slot + nxt * 4
                        p9 = r32(pp)
                        u5 = ins
                        if r16(p9) & mask:
                            u5 = ins + 1
                            cur = r32(slot + ins * 4)
                            w32(slot + ins * 4, p9)
                            w32(pp, cur)
                        nxt += 1
                        ins = u5
                    if u5 != la:
                        la = u5 - 1
                    nm = mask >> 1
                    mask = nm | 0x8000
                    if nm & 1:
                        break
                else:
                    la += 1
        # Rebalance the groups.
        la, n_groups = 0, r16(0x0A)
        top = (n_groups - 1) & 0xFFFF
        moved = 0
        if n_groups:
            while True:
                pgc, p2 = gc + la * 2, symt + la * 4
                g = r16(pgc)
                if g == 0:
                    la += 1
                else:
                    p1 = r32(p2 + 4)
                    w_first = r16(r32(r32(p2)))
                    w_last = r16(r32((p1 - 4) & 0xFFFFFFFF))
                    w_last2 = r16(r32((p1 - 8) & 0xFFFFFFFF))
                    if g < 3 or ((num - 1) & 0xFFFF) == la or w_first < ((w_last + w_last2) & 0xFFFF):
                        p16 = p2 + 4
                        found = False
                        acc = r16(r32((p1 - 4) & 0xFFFFFFFF))
                        u17 = la + 2
                        while u17 < n_groups:
                            p4 = symt + u17 * 4
                            acc = (acc - s16(r32(r32(p4)))) & 0xFFFF
                            wn = r16(r32((r32(p4) + 4) & 0xFFFFFFFF))
                            if r16(gc + u17 * 2) > 1 and (acc & 0x8000 or acc < wn):
                                found = True
                                break
                            u17 += 1
                        if not found:
                            la += 1
                        else:
                            w16(pgc, s16(pgc) - 1)
                            moved += 1
                            la += 1
                            w32(p16, r32(p16) - 4)
                            w16(gc + la * 2, s16(gc + la * 2) + 2)
                            w32(p2 + 8, r32(p2 + 8) + 4)
                            if la < ((u17 - 1) & 0xFFFF):
                                span = (u17 - 1) - la
                                la += span
                                for _ in range(span):
                                    w32(p16 + 8, r32(p16 + 8) + 4)
                                    p16 += 4
                            w16(gc + la * 2, s16(gc + la * 2) + 1)
                            w32(p16 + 4, r32(p16 + 4) + 4)
                            w16(gc + 2 + la * 2, s16(gc + 2 + la * 2) - 2)
                            if s16(gc + top * 2) == 0:
                                n_groups -= 1
                                w16(0x0A, n_groups)
                                top = (top - 1) & 0xFFFF
                            la = 0
                    else:
                        moved += 1
                        w16(pgc - 2, s16(pgc - 2) + 1)
                        w16(pgc, s16(pgc) - 3)
                        w16(gc + 2 + la * 2, s16(gc + 2 + la * 2) + 2)
                        w32(p2, r32(p2) + 4)
                        w32(p2 + 4, r32(p2 + 4) - 8)
                        if top == la:
                            n_groups += 1
                            w16(0x0A, n_groups)
                            top = (top + 1) & 0xFFFF
                        la = 0
                if la >= n_groups:
                    break
        # Tune the update period to how much moved.
        if moved < 0x10:
            if moved < 8 and r16(0x2E) != 1:
                w16(0x02, r16(0x02) << 1)
                w16(0x2C, r16(0x2C) >> 1)
                w16(0x2E, r16(0x2E) >> 1)
        else:
            w16(0x02, r16(0x32))
            w16(0x2E, r16(0x30))
        w16(0x00, r16(0x02))
        if r16(0x2C) == 0:
            w16(0x2C, r16(0x2E))


def decompress(data, at, end, size):
    bi = Bits(data, at, end)
    if bi.bits(16) != STREAM_MAGIC:
        raise ValueError("not a 0xB59C stream at 0x%x" % at)
    raw_literals = bi.bits(8)
    bi.bits(8)
    init_period, upd_period, window = bi.bits(12), bi.bits(12), bi.bits(4)
    low = 7 if window == 8 else 6
    lengths = Model(6, 0x0C, init_period, upd_period)
    distances = Model(6, 0x0C, init_period, upd_period)
    literals = None if raw_literals else Model(8, 0x10, init_period, upd_period)
    out = bytearray()
    while len(out) < size:
        if bi.bit() == 0:
            out.append((literals.decode(bi) if literals else bi.bits(8)) & 0xFF)
            continue
        dist = bi.bits(low)
        dist |= distances.decode(bi) << low
        if dist == 0:
            break
        length = lengths.decode(bi) & 0x7F
        back = dist + 1
        for _ in range(min(length, size - len(out))):
            pos = len(out)
            out.append(out[pos - back] if pos >= back else 0)
    return bytes(out)


# ---- MODIFY programs ------------------------------------------------------------

def run_program(src, ops, size, src_count):
    r = Reader(ops, 0)
    out = bytearray(size)
    pos = poke = 0
    gaps, templates = [], []

    def copy(advance, at, count):
        nonlocal pos
        if advance:
            gaps.append((pos, advance))
            pos += advance
        out[pos:pos + count] = src[at:at + count].ljust(count, b"\0")
        pos += count

    def fill(seek, pattern, count):
        nonlocal pos
        if seek:
            gaps.append((pos, seek))
            pos += seek
        out[pos:pos + count] = (pattern * count)[:count] if pattern else bytes(count)
        pos += count

    def add(at, width, delta):
        value = int.from_bytes(out[at:at + width], "little") + delta
        out[at:at + width] = (value & ((1 << (8 * width)) - 1)).to_bytes(width, "little")

    def signed(raw):
        return int.from_bytes(raw, "little", signed=True)

    while True:
        op = r.u8()
        if op == 0x01:
            break
        if op == 0x02:
            r.vli()
            pos = poke = 0
        elif op in (0x03, 0x04):
            advance = r.vli() if op == 0x04 else 0
            if src_count >= 2:
                r.vli()
            copy(advance, r.vli(), r.vli())
        elif op == 0x05:
            if size > pos:
                gaps.append((pos, size - pos))
            for at, n in gaps:
                out[at:at + n] = ops[r.p:r.p + n]
                r.p += n
            gaps.clear()
        elif op == 0x06:
            poke += r.vli()
            add(poke, 1, signed(bytes([r.u8()])))
        elif op in (0x07, 0x0E, 0x0F, 0x10):
            width = {0x07: 1, 0x0E: 1, 0x0F: 2, 0x10: 4}[op]
            poke = 0
            delta = signed(ops[r.p:r.p + width])
            r.p += width
            for _ in range(r.vli()):
                poke += r.vli()
                add(poke, width, delta)
        elif op == 0x08:
            if src_count >= 2:
                r.vli()
            templates.append((r.vli(), r.vli()))
        elif op in (0x09, 0x0A):
            advance = r.vli() if op == 0x0A else 0
            copy(advance, *templates[r.vli()])
        elif op in (0x0B, 0x0C):
            seek = r.vli() if op == 0x0C else 0
            fill(seek, b"", r.vli())
        elif op == 0x0D:
            poke = 0
            for _ in range(r.vli()):
                poke += r.vli()
                add(poke, 1, signed(bytes([r.u8()])))
        elif 0x11 <= op <= 0x16:
            seek = r.vli() if op >= 0x14 else 0
            width = (1, 2, 4)[(op - 0x11) % 3]
            pattern = ops[r.p:r.p + width]
            r.p += width
            fill(seek, pattern, r.vli())
        else:
            raise ValueError("unknown opcode 0x%02x" % op)
    return bytes(out[:size])


# ---- checks and the tool --------------------------------------------------------

def rolling(data, bits):
    """The patch's own checksum: fold each byte in, then rotate left by 8 within N bits."""
    mask, w = (1 << bits) - 1, 0
    for c in data:
        w ^= c
        w = ((w << 8) | (w >> (bits - 8))) & mask
    return w


def rolling_both(data):
    return rolling(data, 31), rolling(data, 30)


def find(root, rel):
    """rel (backslashes or slashes) under root, matched without regard to case."""
    path = root
    for part in rel.replace("\\", "/").split("/"):
        try:
            hit = [n for n in os.listdir(path) if n.lower() == part.lower()]
        except OSError:
            return None
        if not hit:
            return None
        path = os.path.join(path, hit[0])
    return path


def build(data, rec, old_root):
    end = rec["stream"] + rec["csize"]
    if rec["kind"] == "new":
        out = decompress(data, rec["stream"], end, rec["size"])
    else:
        old_path = find(old_root, rec["path"])
        if not old_path:
            raise ValueError("%s: the old file is missing" % rec["path"])
        src = open(old_path, "rb").read()
        if (len(src), *rolling_both(src)) != (rec["src"]["size"], rec["src"]["w1"], rec["src"]["w2"]):
            raise ValueError("%s: the old file isn't the one this patch is for" % rec["path"])
        ops = decompress(data, rec["stream"], end, rec["size"] * 2 + 4096)
        out = run_program(src, ops, rec["size"], rec["src_count"])
    if (len(out), *rolling_both(out)) != (rec["size"], rec["dst"]["w1"], rec["dst"]["w2"]):
        raise ValueError("%s: the result fails the patch's own size/checksum" % rec["path"])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("patch")
    ap.add_argument("old", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    whole = open(args.patch, "rb").read()
    data = whole[payload_offset(whole):]
    head, records = parse(data, 0)
    if args.list or not args.out:
        print("RTPatch version 0x%04x, payload at 0x%x, %d bytes" % (head["version"], len(whole) - len(data), len(data)))
        for key, value in head.get("locations", []):
            print("finds the install at HKLM\\%s \\ %s" % (key, value))
        for rec in records:
            print("  %-7s %-26s %10d bytes  (%d compressed)" % (rec["kind"], rec["path"], rec["size"], rec["csize"]))
        if not args.out:
            return 0
    for rec in records:
        out = build(data, rec, args.old)
        path = os.path.join(args.out, rec["path"].replace("\\", "/"))
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "wb") as f:
            f.write(out)
        print("  %-26s %10d bytes  ok" % (rec["path"], len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

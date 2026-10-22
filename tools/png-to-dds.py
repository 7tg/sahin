#!/usr/bin/env python3
"""A PNG as an uncompressed DDS (A8R8G8B8, one level), with the standard library only.

  tools/png-to-dds.py IN.png OUT.dds

For the texture plugin (third_party/ds2texinject), which loads DDS files itself: Wine's PNG
decoding needs a Unix library the console's Wine doesn't have. Reads 8-bit RGBA PNGs without
interlacing, as art/make-prompts.py writes them.
"""
import struct, sys, zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG")
    at, idat, header = 8, b"", None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour != 6 or interlace:
        sys.exit(f"{path}: not an 8-bit RGBA PNG without interlacing")
    raw, stride, rows, prev = zlib.decompress(idat), width * 4, [], bytearray(width * 4)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            if kind == 1: line[i] = (line[i] + a) & 255
            elif kind == 2: line[i] = (line[i] + b) & 255
            elif kind == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif kind:
                sys.exit(f"{path}: filter {kind}")
        rows.append(bytes(line))
        prev = line
    return width, height, rows


def main():
    src, out = sys.argv[1], sys.argv[2]
    width, height, rows = read_png(src)
    # DDS_HEADER: caps, size, pixel format A8R8G8B8 (BGRA in memory), one level.
    header = struct.pack("<4s7I44x8I4I4x", b"DDS ", 124, 0x100F, height, width, width * 4, 0, 1,
                         32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
                         0x1000, 0, 0, 0)
    pixels = bytearray()
    for row in rows:
        for i in range(0, len(row), 4):
            r, g, b, a = row[i:i + 4]
            pixels += bytes((b, g, r, a))
    open(out, "wb").write(header + pixels)
    print(f"{out}: {width}x{height}, {len(header) + len(pixels)} bytes")


if __name__ == "__main__":
    main()

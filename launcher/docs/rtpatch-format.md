# RTPatch updates (Most Wanted 1.3)

EA shipped Most Wanted 1.3 as `Patch.v1.3.exe`: Pocket Soft's RTPatch 7.00 self-extractor
with the patch appended. Prospero BlackBox reads that file and writes the 1.3 files itself;
no EA or Pocket Soft code is run. `tools/rtpatch.py` is the reference decoder,
`src/library/rtpatch.cpp` the one in the manager.

This describes what was confirmed on that one patch (sha256
`4c858e9b63ae2f856af7f1c090d5ce3d8c75246e9c0f7018ff57efb87addd0ef`, 12,777,356 bytes).
The codec and the MODIFY program were worked out earlier by
[rtptool](https://github.com/bwrsandman/rtptool) (MIT, RTPatch 2.09) and
[fighters-codex](https://github.com/jomkz/fighters-codex) (MIT, an EA patcher built with
RTPatch Pro 4.11); version 7.00 keeps both and adds a few header fields.

How things were confirmed: a 1.2 tree from the NA discs (`GLOBAL/GAMEPLAY.BIN` 2,103,680
bytes, CRC `91a3ddf1`) and the 1.3 tree that EA's patcher made from it gave the answer key,
seven files; a separate 1.3 install agrees on the six that aren't `speed.exe`. Every field
below is either checked by the decoders on every run (marked *checked*) or only observed
(marked *seen*).

## Where the payload is

| What | Value | How |
|---|---|---|
| Container | an ordinary PE32 (i386, GUI) | `file`; PE header |
| Payload start | the end of the last PE section's raw data (`0xc2400` here) | *checked*: the payload's magic is there |
| Payload end | the end of the file | *checked* |
| Trailer | the payload's last 8 bytes: u32 payload start (in the exe), then `DKNJ` | *checked*: equals the PE-derived start |

A bare payload (starting with `K*`) is read the same way.

## Conventions

Little-endian. **lp_string**: u8 length counting the closing NUL (`0xFF` escapes to a u16),
then the bytes. **VLI**: lead byte; bit 7 is the sign; the run of 1-bits from bit 6 down
is the number of extra bytes, which hold the low part little-endian, the lead byte's
remaining bits the high part; no run means a value 0..63 in the low 6 bits.

## Header

| Field | Type | This patch | How |
|---|---|---|---|
| magic | 2 bytes | `K*` | *checked* |
| version | u16 | `0x02bc` (7.00) | *seen*; matches "Version 7.00" in the exe's strings |
| flags | u16 | `0x92e4` | bit 15 *checked* (an ext word follows) |
| ext flags | u32, if flags bit 15 | `0x00010000` | bit 16 = extra mode, *checked* by the record walk |
| option flags | u16 | `0x0030` | the default for records with no override; *checked* (records without one parse) |
| total size, reserved | u32, u32 | 0, 0 | *seen* |
| attributes, reserved | u16, u16 | 0, 0 | *seen* |
| command flags | u16 | 0 | bit 2 would add a u32 combine id |
| reserved | u32 | `0x00140000` | *seen*, meaning unknown |
| record count | u32 (7.00) | 7 | *checked* against the walk |
| unknown | u32 (7.00) | | *seen*; a stamp or id, not used |
| install locations | u16 count (7.00), then per entry: u8, u16, lp key, lp value | 1: `SOFTWARE\EA GAMES\Need for Speed Most Wanted`, `Install Dir` | *seen*; EA's patcher read exactly this HKLM value under Wine. The u8/u16 are unexplained (0, 4) |
| system files | u16 count, then (lp name, lp prompt) pairs | 0 | as in 4.11 |
| folders | u16 count, then lp strings | `FRONTEND`, `GLOBAL` | *checked* by the walk |

## Records

A u16 record header: the top nibble is the type (1 end, 2 NEW, 4 MODIFY; 3, 5, 6 are
RENAME/MKDIR/DELETE elsewhere and refused here). Then, in order:

| Field | When | How |
|---|---|---|
| option flags override (u16) | header bit 1 | *checked* (`0x01b0` on five records) |
| path, lp_string, backslashes | header bit 2 | *checked*: names match the answer key |
| seek VLI, then (extra mode) checksum VLI | option flags & `0xC0` | *checked*: the five records with `0x01b0` carry two bytes more than the two without |
| disk-set VLI | header bit 7 | not in this patch |
| attributes (u16) | header bit 8 | not in this patch |
| two lp_strings | header bit 9 | not in this patch |
| metadata | 10 bytes, always | *seen*, not interpreted |

**NEW (type 2)**: VLI entry count, u32 file size, u32 compressed size, the entries, then the
compressed file (that many bytes). *checked*: the walk lands exactly on each next record,
the end record and the trailer, and every decoded size matches.

**MODIFY (type 4)**: u16 flags, VLI source count, VLI destination count, u32 reserved,
u32 compressed size, the source entries, the destination entries, then the compressed
program. Not in this patch: the layout is from rtptool and fighters-codex.

**Entry** (one version of a file): 24 bytes (name, NUL-padded, in the first 14; size u32 at
+16; four bytes at +20, *seen*), 10 bytes of checksums (u8, u8, then w1 u32 at +2 masked to
31 bits and w2 u32 at +6 masked to 30 bits), and in extra mode 8 bytes of times (*seen*)
and an lp_string with the long name.

**Checksums** *checked* on all seven new files: start at 0, for each byte
`w = rotl8(w ^ byte)` within 31 bits (w1) and within 30 bits (w2).

**End**: a u16 with type 1, then the trailer (above) or the end of the file.

## The compressed stream

MSB-first bits. Header: 16 bits magic `0xB59C`, 8 bits literal mode (0: literals are
Huffman-coded; else 8 raw bits), 8 bits ignored, 12 bits initial period, 12 bits update
period, 4 bits window (8: 7 low distance bits, else 6). All seven streams here:
literal mode 0, periods 64 and 16, window 8.

Three adaptive Huffman models (literals: 8-bit escape, 16 levels; lengths and distances:
6-bit escape, 12 levels) share the bit stream. A symbol not seen yet comes as the escape
symbol and then its raw bits. Weights are counted per symbol; when the countdown reaches
zero the model is rebuilt (weights halved on the update period, symbols re-sorted by
weight, groups rebalanced, the period retuned by how much moved). The exact model is in
the decoders; it was taken from rtptool's reverse engineering and is what makes the output
byte-exact.

Tokens: one flag bit. 0: a literal byte. 1: distance = (Huffman distance symbol << low
bits) | low bits read raw; distance 0 ends the stream; else a Huffman length symbol (low
7 bits) and that many bytes copied from distance + 1 back (zero before the start).

*checked*: decoding each record's stream gives exactly its file size, the patch's own
checksums, and the 1.3 file's sha256.

## MODIFY programs

The decompressed bytes of a MODIFY record are a program writing the new file over a
zeroed buffer of its size: copies from the old file at absolute offsets (optionally after
a gap that later receives literal bytes), stored copy templates, zero and pattern fills,
signed 1/2/4-byte additions at running positions, one late FLUSH that pours the
remaining program bytes into the gaps in order, END. Opcodes 0x01-0x16, as in rtptool's
SPEC.md section 9. Implemented in both decoders and checked on a made-up patch
(`tests/store_rtpatch_test.cpp` builds it; `tools/store-test.sh` runs both decoders on
it); **not exercised by this patch**, which only has NEW records.

## What the 1.3 patch carries

Seven NEW records, whole files: `FRONTEND\FrontB.lzc`, `GLOBAL\gameplay.bin`,
`GLOBAL\gameplay.lzc`, `GLOBAL\InGameB.bun`, `GLOBAL\InGameB.lzc`, `server.dll` (added:
the 1.2 copy the console imports has none) and `speed.exe` (the installer replaces it
anyway). It doesn't need the 1.2 bytes at all, only a 1.2 copy to put them in.

## Open

- The 7.00 header's unknown u32 after the record count, the location entry's u8/u16, the
  reserved word before it, the 10-byte record metadata and the entry's times are read
  past, not understood.
- MODIFY, RENAME, MKDIR and DELETE are untested on real data (this patch has none).

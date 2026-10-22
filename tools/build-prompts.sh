#!/usr/bin/env bash
# Blur's PlayStation button prompts, as the zip the install plan's "playstation-prompts" source
# pins (profile/blur/make-blur-plan.py): release/playstation-prompts.zip, holding
#   scripts/DS2TexInject.asi       the texture plugin, built from third_party/ds2texinject
#   DS2TexInject.ini               its settings (profile/blur/DS2TexInject.ini)
#   texmod/_cache/0xF6FA7B91.dds   our atlas (art/prompts.png, drawn by art/make-prompts.py)
# The same sources give the same bytes: no link timestamp, fixed zip dates, sorted members.
# It runs inside the pinned build image (tools/build.sh prompts).
set -euo pipefail
W=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$W/.deps/prompts out=$W/release/playstation-prompts.zip
rm -rf "$work" && mkdir -p "$work/scripts" "$work/texmod/_cache" "$W/release"
i686-w64-mingw32-g++ -O2 -std=c++17 -shared -s -Wall -Wno-unused-function \
    -static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp \
    -ffile-prefix-map="$W"=. -o "$work/scripts/DS2TexInject.asi" \
    "$W/third_party/ds2texinject/ds2texinject.cpp" -luuid
python3 "$W/tools/png-to-dds.py" "$W/art/prompts.png" "$work/texmod/_cache/0xF6FA7B91.dds"
cp "$W/profile/blur/DS2TexInject.ini" "$work/"
python3 - "$work" "$out" <<'PY'
import os, sys, zipfile
work, out = sys.argv[1:]
names = sorted(os.path.relpath(os.path.join(d, f), work).replace(os.sep, "/")
               for d, _, files in os.walk(work) for f in files)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in names:
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        info.compress_type, info.external_attr = zipfile.ZIP_DEFLATED, 0o644 << 16
        z.writestr(info, open(os.path.join(work, name), "rb").read())
PY
sha256sum "$out" | cut -c1-64 | tr -d '\n'; echo "  $(stat -c %s "$out")  ${out#$W/}"

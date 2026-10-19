#!/usr/bin/env python3
"""Zip a game's own app folder the same way every time: the release asset the homebrew catalog
installs (its <TITLEID>/ folder at the top, under 2 GiB).

Entries sorted, fixed times (2026-01-01), deflated, no links or .DS_Store; the same folder
gives the same bytes on any machine (the zips tools/make-bundle.py makes are made this way).

  tools/make-app-zip.py release/game-apps/PPSA99404 release/game-apps/PPSA99404.zip
"""
import hashlib, json, os, shutil, sys, zipfile

app, out = sys.argv[1], sys.argv[2]
tid = os.path.basename(os.path.normpath(app))
param = json.load(open(os.path.join(app, 'sce_sys', 'param.json')))
if param['titleId'] != tid:
    sys.exit(f'{app} holds {param["titleId"]}, not {tid}')
names = []
for folder, dirs, files in os.walk(app):
    dirs.sort()
    for name in files:
        path = os.path.join(folder, name)
        if os.path.islink(path) or name == '.DS_Store':
            continue
        names.append(os.path.relpath(path, app))
with zipfile.ZipFile(out + '.part', 'w', allowZip64=True) as z:
    for name in sorted(names):
        info = zipfile.ZipInfo(tid + '/' + name.replace(os.sep, '/'), (2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        with open(os.path.join(app, name), 'rb') as f, z.open(info, 'w', force_zip64=True) as w:
            shutil.copyfileobj(f, w, 1 << 20)
os.replace(out + '.part', out)
size = os.path.getsize(out)
if size >= 2 << 30:
    sys.exit(f'{out} is {size} bytes: the catalog takes at most 2 GiB')
digest = hashlib.sha256()
with open(out, 'rb') as f:
    for piece in iter(lambda: f.read(1 << 20), b''):
        digest.update(piece)
print(f'{digest.hexdigest()}  {size}  {os.path.basename(out)} ({len(names)} files, {tid} {param["contentVersion"]})')

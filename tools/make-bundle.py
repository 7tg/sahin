#!/usr/bin/env python3
"""Fill the manager's assets/bundle: what a dev build carries so it installs a game on its own.

For every plan in <manager>/assets/plans, each source with no download address (the files
this repo makes: speed.exe, the 1.2 -> 1.3 diff, DSOAL, the D3DX DLLs and our ASI) is written to
assets/bundle/<sha256>, checked against the plan's hash and size. A source that does have an
address is carried the same way when <release>/assets or mods/companions holds that
file (the download's name, or in mods/companions the one file of the plan's size and
SHA-256). The Xbox 360 zips are carried until their release exists. A missing file is
left for the download; a file that is there but does not match is an error.
The bare prefix (tools/make-bare-prefix.py's zip) goes beside them as
bare-prefix-nfs-blackbox.zip.
Each game's Home app (tools/build-prospero-win.sh apps) is zipped as <TITLEID>.zip, once
its eboot is seen to start the profile its plan installs, and listed in apps.json (file,
sha256, size, version) for the manager's Add to Home.
Anything a plan pins that isn't found here is an error; nothing is guessed.

--for-app fills it for a game's own app (tools/build.sh game-apps): no Home-app zips and
no downloads from <release>/assets (the Xbox 360 zips come from their public release; the
app stays under the catalog's 2 GiB).

  tools/make-bundle.py [--manager ../prospero-blackbox] [--release release] [--for-app]
"""
import argparse, glob, hashlib, json, os, shutil, sys, zipfile
from urllib.parse import unquote, urlparse

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--manager', default=os.path.join(os.path.dirname(R), 'prospero-blackbox'))
ap.add_argument('--release', default=os.path.join(R, 'release'))
ap.add_argument('--for-app', action='store_true')
args = ap.parse_args()
args.prefix = os.path.join(args.release, 'bare-prefix-nfs-blackbox.zip')
# The manager's game apps and the plan each one plays (catalog/blackbox.cpp's title IDs).
APPS = {'PPSA99403': 'NFSMW'}


def sha(b): return hashlib.sha256(b).hexdigest()


# What this repo makes, by hash: the built files, the diffs, and DSOAL's inner zip
# (the plan pins DSOAL_r648.zip from inside the release's DSOAL.zip).
made = {}
for path in sorted(glob.glob(os.path.join(R, 'profiles', '*', 'built', '*')) +
                   glob.glob(os.path.join(R, 'profiles', '*', '*.zip')) +
                   glob.glob(os.path.join(args.release, 'x360', '*')) +  # tools/build-x360-bits.sh
                   glob.glob(os.path.join(args.release, 'menu', '*')) +  # tools/build-blackbox-menu.sh
                   glob.glob(os.path.join(args.release, 'settings-mod', '*'))):  # tools/build-settings-mod.sh
    if os.path.isfile(path):
        b = open(path, 'rb').read()
        made[sha(b)] = (b, os.path.relpath(path, R))
dsoal = os.path.join(R, 'mods', 'dsoal', 'DSOAL.zip')
if os.path.isfile(dsoal):
    b = zipfile.ZipFile(dsoal).read('DSOAL_r648.zip')
    made[sha(b)] = (b, 'mods/dsoal/DSOAL.zip!DSOAL_r648.zip')

bundle = os.path.join(args.manager, 'assets', 'bundle')
os.makedirs(bundle, exist_ok=True)
wanted, missing = set(), []
for plan_path in sorted(glob.glob(os.path.join(args.manager, 'assets', 'plans', '*.json'))):
    plan = json.load(open(plan_path))
    for sid, s in sorted(plan.get('sources', {}).items()):
        if s.get('url'):
            continue
        if s['sha256'] not in made or len(made[s['sha256']][0]) != s['size']:
            missing.append(f"{os.path.basename(plan_path)}: {sid} ({s['sha256']})")
            continue
        b, where = made[s['sha256']]
        out = os.path.join(bundle, s['sha256'])
        if not (os.path.isfile(out) and sha(open(out, 'rb').read()) == s['sha256']):
            with open(out + '.part', 'wb') as f:
                f.write(b)
            os.replace(out + '.part', out)
        wanted.add(s['sha256'])
        print(f"{s['sha256'][:12]}  {s['size']:>10}  {sid}  <- {where}")

# A download whose file is already here (the Xbox 360 zips, the companion mods) is carried
# too, so a dev build installs it without the release that publishes it. The address stays
# in the plan. The bytes are linked, not read in: the movies zip is 1.8 GB.
def stream_sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for piece in iter(lambda: f.read(1 << 20), b''):
            digest.update(piece)
    return digest.hexdigest()


def place(src, digest, size):
    out = os.path.join(bundle, digest)
    if os.path.isfile(out) and not os.path.islink(out) and os.path.getsize(out) == size:
        if os.path.samestat(os.stat(src), os.stat(out)) or stream_sha(out) == digest:
            return
    tmp = out + '.part'
    if os.path.lexists(tmp):
        os.remove(tmp)
    try:
        os.link(src, tmp)
    except OSError:
        shutil.copyfile(src, tmp)
    if os.path.getsize(tmp) != size or stream_sha(tmp) != digest:
        os.remove(tmp)
        sys.exit(f'{src} changed while it was carried')
    os.replace(tmp, out)


assets = os.path.join(args.release, 'assets')
companions = os.path.join(R, 'mods', 'companions')
bad = []


def local_source(name, size, digest):
    """A file we already have for this download, or None. A same-named file that does
    not match the plan is an error (it would be the one a person expects to ship)."""
    for folder in ((companions,) if args.for_app else (assets, companions)):
        src = os.path.join(folder, name)
        if not os.path.isfile(src) or os.path.islink(src):
            continue
        got = os.path.getsize(src)
        if got != size or stream_sha(src) != digest:
            bad.append(f"{name} in {os.path.relpath(folder, R)} does not match the plan")
            return None
        return src
    if os.path.isdir(companions):
        hits = []
        for fn in sorted(os.listdir(companions)):
            src = os.path.join(companions, fn)
            if os.path.isfile(src) and not os.path.islink(src) and os.path.getsize(src) == size and stream_sha(src) == digest:
                hits.append(src)
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            bad.append(f"{name}: more than one companion matches")
    return None


for plan_path in sorted(glob.glob(os.path.join(args.manager, 'assets', 'plans', '*.json'))):
    plan = json.load(open(plan_path))
    for sid, s in sorted(plan.get('sources', {}).items()):
        url = s.get('url') or ''
        if not url or s['sha256'] in wanted:
            continue
        name = unquote(os.path.basename(urlparse(url).path))
        src = local_source(name, s['size'], s['sha256'])
        if src is None:
            continue
        place(src, s['sha256'], s['size'])
        wanted.add(s['sha256'])
        print(f"{s['sha256'][:12]}  {s['size']:>10}  {sid}  <- {os.path.relpath(src, R)}")
if bad:
    sys.exit('a local file does not match the plan:\n  ' + '\n  '.join(bad))
if missing:
    sys.exit('not made by this repo:\n  ' + '\n  '.join(missing))

if not os.path.isfile(args.prefix):
    sys.exit(f'no bare prefix at {args.prefix}: run tools/build-prospero-win.sh prefix')
z = zipfile.ZipFile(args.prefix)
if z.testzip() is not None or 'system.reg' not in z.namelist():
    sys.exit(f'{args.prefix} is not a bare prefix')
shutil.copyfile(args.prefix, os.path.join(bundle, 'bare-prefix-nfs-blackbox.zip'))
wanted.add('bare-prefix-nfs-blackbox.zip')
print(f"{os.path.getsize(args.prefix):>24}  bare-prefix-nfs-blackbox.zip <- {os.path.relpath(args.prefix, R)}")

# The game apps: a zip made the same way every time (sorted, fixed times), the app in a
# folder named after its title, as the manager's archive check wants.
listing = {}
for tid, game in ([] if args.for_app else sorted(APPS.items())):
    app = os.path.join(args.release, 'apps', tid)
    param = json.load(open(os.path.join(app, 'sce_sys', 'param.json')))
    profile = json.load(open(os.path.join(args.manager, 'assets', 'plans', game + '.json')))['profile']['id']
    if param['titleId'] != tid or profile.encode() not in open(os.path.join(app, 'eboot.bin'), 'rb').read():
        sys.exit(f'{app} is not {tid} starting the profile {profile}: run tools/build-prospero-win.sh apps')
    names = []
    for folder, dirs, files in os.walk(app):
        dirs.sort()
        for name in files:
            path = os.path.join(folder, name)
            if os.path.islink(path) or name == '.DS_Store':
                continue
            names.append(os.path.relpath(path, app))
    out = os.path.join(bundle, tid + '.zip')
    with zipfile.ZipFile(out + '.part', 'w') as z:
        for name in sorted(names):
            info = zipfile.ZipInfo(tid + '/' + name.replace(os.sep, '/'), (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            with open(os.path.join(app, name), 'rb') as f, z.open(info, 'w') as w:
                shutil.copyfileobj(f, w, 1 << 20)
    os.replace(out + '.part', out)
    digest = hashlib.sha256()
    with open(out, 'rb') as f:
        for piece in iter(lambda: f.read(1 << 20), b''):
            digest.update(piece)
    listing[tid] = {'file': tid + '.zip', 'sha256': digest.hexdigest(), 'size': os.path.getsize(out),
                    'version': param['contentVersion']}
    wanted.add(tid + '.zip')
    print(f"{digest.hexdigest()[:12]}  {os.path.getsize(out):>10}  {tid}.zip ({len(names)} files, starts {profile}) <- {os.path.relpath(app, R)}")
if not args.for_app:
    with open(os.path.join(bundle, 'apps.json'), 'w') as f:
        json.dump(listing, f, indent=1, sort_keys=True)
        f.write('\n')
    wanted.add('apps.json')

# Files a plan no longer pins are moved aside (out of assets/, which the build ships whole).
aside = os.path.join(args.manager, '.local', 'bundle.old')
for name in sorted(os.listdir(bundle)):
    if name not in wanted:
        os.makedirs(aside, exist_ok=True)
        os.replace(os.path.join(bundle, name), os.path.join(aside, name))
        print(f'moved aside: {name}')

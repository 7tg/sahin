#!/usr/bin/env python3
"""Fill the launcher's assets/bundle: what the app carries so it installs Blur on its own.

The bare prefix (tools/make-bare-prefix.py's zip) goes in as bare-prefix-sahin.zip. Every
other source a plan in <launcher>/assets/plans pins is downloaded by the console, so a
source without a download address would have to be made by this repo: there is none, and
one is an error (nothing is guessed). DXVK's d3d9.dll is not carried for the Amax client:
its d3d9.dll proxy hands off to system32's d3d9.dll, which in the prefix is DXVK already.
Anything else in assets/bundle is moved aside to <launcher>/.local/bundle.old.

  tools/make-bundle.py [--launcher launcher] [--release release]
"""
import argparse, glob, json, os, shutil, sys, zipfile

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--launcher', default=os.path.join(R, 'launcher'))
ap.add_argument('--release', default=os.path.join(R, 'release'))
args = ap.parse_args()
prefix = os.path.join(args.release, 'bare-prefix-sahin.zip')

missing = []
for plan_path in sorted(glob.glob(os.path.join(args.launcher, 'assets', 'plans', '*.json'))):
    for sid, s in sorted(json.load(open(plan_path)).get('sources', {}).items()):
        if not s.get('url'):
            missing.append(f"{os.path.basename(plan_path)}: {sid} ({s['sha256']})")
if missing:
    sys.exit('sources without a download address, not made by this repo:\n  ' + '\n  '.join(missing))

if not os.path.isfile(prefix):
    sys.exit(f'no bare prefix at {prefix}: run tools/build.sh prefix')
z = zipfile.ZipFile(prefix)
if z.testzip() is not None or 'system.reg' not in z.namelist():
    sys.exit(f'{prefix} is not a bare prefix')
bundle = os.path.join(args.launcher, 'assets', 'bundle')
os.makedirs(bundle, exist_ok=True)
shutil.copyfile(prefix, os.path.join(bundle, 'bare-prefix-sahin.zip'))
print(f"{os.path.getsize(prefix):>12}  bare-prefix-sahin.zip <- {os.path.relpath(prefix, R)}")

aside = os.path.join(args.launcher, '.local', 'bundle.old')
for name in sorted(os.listdir(bundle)):
    if name != 'bare-prefix-sahin.zip':
        os.makedirs(aside, exist_ok=True)
        os.replace(os.path.join(bundle, name), os.path.join(aside, name))
        print(f'moved aside: {name}')

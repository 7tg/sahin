#!/usr/bin/env python3
"""Fill the launcher's assets/bundle: what the app carries so it installs Blur on its own.

The bare prefix (tools/make-bare-prefix.py's zip) goes in as bare-prefix-sahin.zip. Every
other source a plan in <launcher>/assets/plans pins is downloaded by the console, but those
made by this repo (MADE below: tools/build-prompts.sh's zip), which have no download address:
each goes in as <sha256>.zip, which is how the launcher finds a carried source, and must be
the plan's file by hash and size, or the plan is stale. A source neither downloaded nor made
here is an error (nothing is guessed). DXVK's d3d9.dll is not carried for the Amax client:
its d3d9.dll proxy hands off to system32's d3d9.dll, which in the prefix is DXVK already.
Anything else in assets/bundle is moved aside to <launcher>/.local/bundle.old.

  tools/make-bundle.py [--launcher launcher] [--release release]
"""
import argparse, glob, hashlib, json, os, shutil, sys, zipfile

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--launcher', default=os.path.join(R, 'launcher'))
ap.add_argument('--release', default=os.path.join(R, 'release'))
args = ap.parse_args()
prefix = os.path.join(args.release, 'bare-prefix-sahin.zip')
# The sources this repo makes, by the plan's id: their file in release/.
MADE = {'playstation-prompts': 'playstation-prompts.zip'}

missing, carried = [], {}
for plan_path in sorted(glob.glob(os.path.join(args.launcher, 'assets', 'plans', '*.json'))):
    for sid, s in sorted(json.load(open(plan_path)).get('sources', {}).items()):
        if s.get('url'):
            continue
        if sid not in MADE:
            missing.append(f"{os.path.basename(plan_path)}: {sid} ({s['sha256']})")
            continue
        made = os.path.join(args.release, MADE[sid])
        if not os.path.isfile(made):
            sys.exit(f'no {made}: run tools/build.sh prompts')
        digest, size = hashlib.sha256(open(made, 'rb').read()).hexdigest(), os.path.getsize(made)
        if (digest, size) != (s['sha256'], s['size']):
            sys.exit(f"{made} is {digest} ({size} bytes) but {os.path.basename(plan_path)} pins "
                     f"{s['sha256']} ({s['size']}): update profile/blur/make-blur-plan.py and the plan")
        carried[f'{digest}.zip'] = made
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
for name, made in sorted(carried.items()):
    shutil.copyfile(made, os.path.join(bundle, name))
    print(f"{os.path.getsize(made):>12}  {name} <- {os.path.relpath(made, R)}")

aside = os.path.join(args.launcher, '.local', 'bundle.old')
for name in sorted(os.listdir(bundle)):
    if name != 'bare-prefix-sahin.zip' and name not in carried:
        os.makedirs(aside, exist_ok=True)
        os.replace(os.path.join(bundle, name), os.path.join(aside, name))
        print(f'moved aside: {name}')

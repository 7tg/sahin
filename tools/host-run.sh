#!/usr/bin/env bash
# Sahin - host run: the game tools/host-install.sh installed, started under the host's Wine
# with the DXVK the build made.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/host-run.sh [seconds]     (seconds: stop it after that long; default: run until it exits)
#
# The installed prefix (HOST_DIR/win/prefixes/sahin) is prospero-win's: its 32-bit side is set
# up for the wowprospero CPU, and the host's Wine can't open a window from a 32-bit program in
# it. So the run uses a host prefix of its own (HOST_DIR/run/prefix, made once with wineboot):
# the installed game folder is copied into it on every run, with the installed prefix's DXVK
# DLLs (the build's, x32 into syswow64 and x64 into system32).
#
# HOST_DIR  as for tools/host-install.sh (default: ./host-check)
# WINE      the host's wine (default: wine)
# HEADLESS  WxH: run inside a headless gamescope of that size instead of on the desktop (no
#           window, no focus taken); screenshots with
#           GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot FILE.png, keys through
#           XTEST on its DISPLAY (printed, e.g. python-xlib's xtest.fake_input). Blur picks the
#           desktop size as its resolution unless its settings.xml says otherwise.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
HOST_DIR=${HOST_DIR:-$here/host-check}
WINE=${WINE:-wine}
seconds=${1:-0}
if [[ -n ${HEADLESS:-} ]]; then
    w=${HEADLESS%x*} h=${HEADLESS#*x}
    exec env -u HEADLESS -u DISPLAY -u WAYLAND_DISPLAY HOST_DIR="$HOST_DIR" \
        gamescope --backend headless -w "$w" -h "$h" -W "$w" -H "$h" -- \
        bash -c 'echo "== gamescope DISPLAY=$DISPLAY"; exec "$0" "$@"' "$0" "$@"
fi
installed=$HOST_DIR/win/prefixes/sahin
[[ -f $installed/drive_c/Games/BLUR/Blur.exe ]] || { echo "no install in $installed: run tools/host-install.sh" >&2; exit 1; }
[[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]] || { echo "no display (DISPLAY / WAYLAND_DISPLAY)" >&2; exit 1; }

export WINEPREFIX=$HOST_DIR/run/prefix WINEARCH=win64
if [[ ! -f $WINEPREFIX/system.reg ]]; then
    mkdir -p "$HOST_DIR/run"
    WINEDEBUG=-all wineboot -i
    wineserver -w
fi
for dll in d3d8 d3d9 d3d10core d3d11 dxgi; do
    cp "$installed/drive_c/windows/syswow64/$dll.dll" "$WINEPREFIX/drive_c/windows/syswow64/"
    cp "$installed/drive_c/windows/system32/$dll.dll" "$WINEPREFIX/drive_c/windows/system32/"
done
game=$WINEPREFIX/drive_c/Games/BLUR
rm -rf "$game"
mkdir -p "${game%/*}"
cp -a --reflink=auto "$installed/drive_c/Games/BLUR" "$game"

# The profile's overrides (BLUR.json: dll_overrides): DXVK's d3d9/dxgi, the ASI loader as dsound.
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d9,dxgi=n;dsound=n,b"}
export DXVK_HUD=${DXVK_HUD:-fps}
export WINEDEBUG=${WINEDEBUG:-+loaddll,err}
log=$HOST_DIR/run.log
touch "$HOST_DIR/run.log.start"
echo "== $WINEPREFIX: Blur.exe, log in $log"
cd "$game"
if (( seconds > 0 )); then
    timeout "$seconds" "$WINE" Blur.exe > "$log" 2>&1 || true
    wineserver -k || true
else
    "$WINE" Blur.exe > "$log" 2>&1
fi

#!/usr/bin/env bash
# Sahin - host install check: the launcher's installer run against a copy of your Blur folder.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/host-install.sh [option...]      (default option: online)
#
# BLUR      your Blur folder (read-only: it is copied, never changed or linked to)
# HOST_DIR  where the copy, the prefix and the install go (default: ./host-check, untracked);
#           on the same btrfs/xfs as BLUR the copy is a reflink and costs no space
# BARE      the bare prefix zip (default: release/bare-prefix-sahin.zip)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
L=$here/launcher
BLUR=${BLUR:-"$HOME/Games/Blur EN"}
HOST_DIR=${HOST_DIR:-$here/host-check}
BARE=${BARE:-$here/release/bare-prefix-sahin.zip}
(( $# )) || set -- online
[[ -f $BLUR/Blur.exe ]] || { echo "no Blur.exe in $BLUR" >&2; exit 1; }
[[ -f $BARE ]] || { echo "no bare prefix at $BARE (tools/build.sh prefix)" >&2; exit 1; }

# The installer, compiled as store-test.sh compiles store_game_install_test.
build=$L/build/host-install
mkdir -p "$build"
objects=()
for name in yyjson/yyjson miniz/miniz miniz/miniz_tinfl miniz/miniz_tdef miniz/miniz_zip; do
    object=$build/${name//\//_}.o
    [[ $object -nt $L/src/third_party/$name.c ]] || clang -std=c11 -O1 -w -c "$L/src/third_party/$name.c" -o "$object"
    objects+=("$object")
done
clang++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror -I"$L/src" \
    "$L/host/install_main.cpp" "$L/src/library/game_sources.cpp" "$L/src/library/prefix.cpp" \
    "$L/src/library/game_install.cpp" "$L/src/library/upgrade.cpp" "$L/src/library/rtpatch.cpp" \
    "$L/src/library/install_plan.cpp" "$L/src/library/game_profile.cpp" \
    "$L/src/library/wine_registry.cpp" "$L/src/library/base_game.cpp" "$L/src/library/tree.cpp" \
    "$L/src/core/save_file.cpp" "$L/src/core/i18n.cpp" "$L/src/core/i18n_strings.cpp" "${objects[@]}" -pthread -o "$build/host-install"

# base-games/BLUR: a real copy of the user's folder (reflinked where the file system can).
data=$HOST_DIR/data
win=$HOST_DIR/win
base=$data/base-games/BLUR
if [[ ! -f $base/Blur.exe ]]; then
    mkdir -p "$data/base-games"
    rm -rf "$base.part"
    cp -a --reflink=auto "$BLUR" "$base.part"
    if [[ -n $(find "$base.part" -type l -print -quit) ]]; then
        echo "the copy holds symlinks; refusing" >&2
        exit 1
    fi
    mv "$base.part" "$base"
fi
mkdir -p "$win/prefixes" "$win/profiles"  # both are there on the console

"$build/host-install" "$L/assets/plans/BLUR.json" "$data" "$win" --bare-prefix "$BARE" "$@"

game=$win/prefixes/sahin/drive_c/Games/BLUR
echo "== $game"
ls -la "$game" "$game/scripts" 2>/dev/null || true

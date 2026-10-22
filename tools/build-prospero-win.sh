#!/usr/bin/env bash
# Build the prospero-win stack (the Wine runtime package and the games' own Home apps)
# from this repo. It runs inside the pinned build image: use tools/build.sh, which runs it
# there (any machine with Docker), rather than calling it directly.
#
#   tools/build-prospero-win.sh                     # every step
#   tools/build-prospero-win.sh setup wineps5 pkg   # some steps
#
# Steps, in order:
#   setup     the source trees (each patched branch made anew when its patches change):
#             prospero-win sahin-v020 = 644e32e + patches/prospero-win,
#             PS5_Mesa = 01d7057 + patches/PS5_Mesa, PS5_Vulkan = 8aee3af + patches/PS5_Vulkan/v020
#             (all applied with fixed committer dates, so the same hashes anywhere), Wine at
#             the pinned commit, Mesa's Python tools, llvm-mingw, the PRX link tool
#   title     prospero-win's eboot (tools/build_native.sh)
#   radv      RADV for the console (PS5_Vulkan)
#   zink      Mesa WGL/Zink (PE32/PE64)
#   hostwine  Wine for the build machine (tools only, no optional libraries: tools/linux/)
#   wineps5   Wine's PS5 modules (PRX) and libvulkan.prx from RADV
#   cpu       wowprospero and wow64native
#   pkg       the package -> release/PPSA99995 (git-ignored)
#   apps      the games' own Home apps -> <release>/apps (art from art/)
#   dxvk      our DXVK: 3.1.1 + patches/dxvk/, every DLL for 32/64-bit (tools/build-dxvk.sh)
#   prefix    the bare Wine prefix the manager unpacks, with that DXVK -> <release>/bare-prefix-sahin.zip
#             (tools/make-bare-prefix.py: created by host Wine, packed as pw_prefix pushes it)
set -euo pipefail
# Every link globs the SDK's stubs ("$sdk"/target/lib/*.so), and each import goes to the first
# stub that exports it. macOS's locale sorts libkernel_web.so ahead of libkernel.so, which the
# console doesn't load for a game: its imports stay null and the eboot dies in main. C order,
# the same as Linux, on every host.
export LC_ALL=C
W=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $(uname -sm) == "Linux x86_64" ]] || { echo "build-prospero-win.sh runs in the build image: use tools/build.sh" >&2; exit 2; }
PW=$W/prospero-win
SRC=$PW/.deps/wine/source
WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8
PW_BASE=644e32e400c555e340d507383398c5d21b39b0b0
MESA_BASE=01d70579b8d410d6a482e36c6f184798ae4c83ad MESA_REV=ac61f9a095984aec8f36ee1174996668c6b08210
VULKAN_BASE=8aee3afb37bcfad06f0c7d263b901040f08ebbd0
out=$W/release
mingw=llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz
mingw_sha=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
jobs=$(nproc)
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
venv=$PW/.deps/mesa-python
steps=${*:-setup title radv zink hostwine wineps5 cpu pkg apps dxvk prefix}
want() { [[ " $steps " == *" $1 "* ]]; }
say() { printf '\n==> %s\n' "$*"; }
am() { # repo-dir base patches... : the branch = base + patches, committed reproducibly
    git -C "$1" -c user.name=lavavex -c user.email=27239435+lavavex@users.noreply.github.com \
        am -q --committer-date-is-author-date "${@:3}"
}
series() { # repo-dir base branch patches... : the branch made anew whenever the patches change
    local repo=$1 base=$2 branch=$3 sum
    sum=$(cat "${@:4}" | sha256sum | cut -c1-64)
    git -C "$repo" rev-parse -q --verify "$branch" >/dev/null &&
        [[ $(git -C "$repo" config --get "sahin.$branch-series" || true) == "$sum" ]] && return
    git -C "$repo" cat-file -e "$base^{commit}" 2>/dev/null || git -C "$repo" fetch -q origin "$base" ||
        git -C "$repo" fetch -q origin
    git -C "$repo" switch -q -f -C "$branch" "$base"
    am "$repo" "$base" "${@:4}"
    git -C "$repo" config "sahin.$branch-series" "$sum"
}

if want setup; then
    say "setup: source trees"
    git -C "$W" submodule update --init PS5_Mesa PS5_Vulkan PS5_PayloadSDK ps5-native-app-boilerplate
    [[ -d $PW/.git ]] || git clone -q https://github.com/mpereiraesaa/prospero-win.git "$PW"
    series "$PW" "$PW_BASE" sahin-v020 "$W"/patches/prospero-win/*.patch
    series "$W/PS5_Mesa" "$MESA_BASE" ps5-app0-param-v020 "$W"/patches/PS5_Mesa/*.patch
    [[ $(git -C "$W/PS5_Mesa" rev-parse ps5-app0-param-v020) == "$MESA_REV" ]] ||
        { echo "PS5_Mesa ps5-app0-param-v020 is not $MESA_REV" >&2; exit 2; }
    series "$W/PS5_Vulkan" "$VULKAN_BASE" sahin-v020 "$W"/patches/PS5_Vulkan/v020/*.patch
    git -C "$W/PS5_Vulkan" switch -q sahin-v020
    git -C "$W/PS5_Mesa" switch -q ps5-app0-param-v020
    git -C "$PW" switch -q sahin-v020
    if [[ $(git -C "$SRC" rev-parse HEAD 2>/dev/null) != "$WINE_COMMIT" ]]; then
        say "setup: Wine $WINE_COMMIT"
        mkdir -p "$SRC"
        [[ -d $SRC/.git ]] || git -C "$SRC" init -q
        git -C "$SRC" fetch -q --depth 1 https://github.com/wine-mirror/wine.git "$WINE_COMMIT"
        git -C "$SRC" checkout -q FETCH_HEAD
    fi
    if [[ ! -x $venv/bin/meson ]]; then
        python3 -m venv "$venv"
        "$venv/bin/python" -m pip install -q -r "$PW/tools/mesa_zink_requirements.txt"
    fi
    mkdir -p "$W/downloads"
    if ! printf '%s  %s\n' "$mingw_sha" "$W/downloads/$mingw" | sha256sum --check --status 2>/dev/null; then
        curl -sfL -o "$W/downloads/$mingw.part" "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/$mingw"
        printf '%s  %s\n' "$mingw_sha" "$W/downloads/$mingw.part" | sha256sum --check --status
        mv "$W/downloads/$mingw.part" "$W/downloads/$mingw"
    fi
    # The PRX link tool, built for this machine.
    tool=$W/ps5-native-app-boilerplate/build/host/ps5-native-tool
    if ! "$tool" >/dev/null 2>&1 && [[ $("$tool" 2>&1 | head -1) != usage:* ]]; then
        mkdir -p "$(dirname "$tool")"
        n=$W/ps5-native-app-boilerplate/tooling/native
        c++ -std=c++20 -O2 "$n/native_app_builder.cpp" "$n/self_container.cpp" "$n/elf_object.cpp" \
            "$n/sce_module_writer.cpp" -lz -o "$tool"
    fi
fi
export PATH=$venv/bin:$PATH
cd "$PW"
[[ $(git -C "$SRC" rev-parse HEAD) == "$WINE_COMMIT" ]] || { echo "run the setup step first" >&2; exit 2; }

if want title; then
    say "title (eboot.bin)"
    tools/build_native.sh
fi
if want radv; then
    say "RADV"
    (cd "$W/PS5_Vulkan" && make deps && bash tools/build-radv.sh release)
fi
if want zink; then
    say "Mesa WGL/Zink"
    MESON=$venv/bin/meson bash tools/build_mesa_zink.sh --work "$PW/.deps/mesa-zink" --mesa "$W/PS5_Mesa" \
        --llvm-mingw "$W/downloads/$mingw" --jobs 4
fi
if want hostwine; then
    say "host Wine"
    CONFIG_SITE=$W/tools/linux/wine-host.config.site PKG_CONFIG_LIBDIR=/var/empty \
        tools/build_host_wine.sh --source "$SRC" --work "$PW/.deps/wine-host" --jobs "$jobs"
fi
if want wineps5; then
    say "Wine PS5 modules"
    # FreeType's tarball, fetched here: Savannah's download address redirects to a random
    # mirror, and some of them don't answer. The same file from Savannah's own server, or
    # from SourceForge, checked against the hash build_wine_ps5.sh pins.
    ft=freetype-2.13.3.tar.xz
    ft_sha=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    mkdir -p "$W/downloads"
    for url in https://download-mirror.savannah.gnu.org/releases/freetype/$ft \
        https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/$ft \
        https://download.savannah.gnu.org/releases/freetype/$ft; do
        printf '%s  %s\n' "$ft_sha" "$W/downloads/$ft" | sha256sum --check --status 2>/dev/null && break
        curl -sfL --retry 3 -o "$W/downloads/$ft.part" "$url" &&
            printf '%s  %s\n' "$ft_sha" "$W/downloads/$ft.part" | sha256sum --check --status &&
            mv "$W/downloads/$ft.part" "$W/downloads/$ft" || rm -f "$W/downloads/$ft.part"
    done
    PROSPERO_FREETYPE_TARBALL=$W/downloads/$ft tools/build_wine_ps5.sh --source "$SRC" --host-tools "$PW/.deps/wine-host/build" \
        --prx-foundation "$W/ps5-native-app-boilerplate" --radv "$W/PS5_Vulkan" --jobs "$jobs"
fi
if want cpu; then
    say "CPU modules"
    tools/build_wowprospero.sh --build "$PW/.deps/wine-host/build" --source "$PW/.deps/wine-host/source"
    SDK=$PW/.deps/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk
    stubs=$PW/.deps/title-stubs
    mkdir -p "$stubs"
    for so in "$SDK"/target/lib/*.so; do
        case ${so##*/} in libkernel_web.so|libScePosixForWebKit.so) continue ;; esac
        cp "$so" "$stubs/"
    done
    tools/build_wow64native.sh --source "$PW/.deps/wine-host/source" --build "$PW/.deps/wine-host/build" \
        --output "$PW/.deps/wow64native" --ps5-sdk "$SDK" --ps5-wine "$PW/.deps/wine-ps5" \
        --ps5-ntdll "$PW/.deps/wine-ps5/prx/ntdll.shared.elf" --ps5-foundation "$W/ps5-native-app-boilerplate" \
        --ps5-stubs "$stubs"
fi
if want pkg; then
    say "package -> $out/PPSA99995"
    tools/package_release.sh --title dist/PPSA99995 --wine-ps5 .deps/wine-ps5 \
        --mesa-zink "$PW/.deps/mesa-zink/artifacts" --host-wine "$PW/.deps/wine-host/install/usr" \
        --cpu-dll "$PW/.deps/wine-host/build/dlls/wowprospero/x86_64-windows/wowprospero.dll" \
        --native-cpu "$PW/.deps/wow64native" --lapy-release build/native/lapy-helper-release.json --out "$out"
fi
if want apps; then
    say "games' own apps -> $out/apps"
    if [[ ! -x $W/PS5_Vulkan/.deps/native/bc7enc_rdo/build/bc7enc ]]; then
        make -C "$W/PS5_Vulkan" assets-deps
    fi
    # title id | name | the profile it starts | art (art/<art>.png icon, -bg.png background,
    # -logo.png, .at9 music) | the contentId's label (16 A-Z/0-9) | its region and publisher (UP0000:
    # a placeholder, not a real publisher's) | the app's contentVersion. The manager's apps
    # (PPSA994xx) start the id their game's plan gives its profile (launcher/assets/plans/<game>.json).
    while IFS='|' read -r tid name profile art label publisher version; do
        [[ -z $tid || $tid == \#* ]] && continue
        args=(--title-id "$tid" --name "$name" --profile "$profile" --package "$out/PPSA99995" --out "$out/apps")
        [[ -n $label ]] && args+=(--content-label "$label")
        [[ -n $publisher ]] && args+=(--content-publisher "$publisher")
        [[ -n $version ]] && args+=(--content-version "$version")
        [[ -f $W/art/$art.png ]] && args+=(--icon "$W/art/$art.png")
        [[ -f $W/art/$art-bg.png ]] && args+=(--background "$W/art/$art-bg.png")
        [[ -f $W/art/$art-logo.png ]] && args+=(--logo "$W/art/$art-logo.png")
        [[ -f $W/art/$art.at9 ]] && args+=(--music "$W/art/$art.at9")
        tools/build_game_app.sh "${args[@]}"
    done <<'APPS'
PPSA99404|Sahin|pb-blur|sahin|SAHINBLUR0000000|UP0000|01.006.000
APPS
fi
if want dxvk; then
    say "DXVK 3.1.1 + patches/dxvk"
    "$W/tools/build-dxvk.sh"
fi
if want prefix; then
    say "bare prefix -> $out/bare-prefix-sahin.zip"
    python3 "$W/tools/make-bare-prefix.py" "$out/bare-prefix-sahin.zip"
    sha256sum "$out/bare-prefix-sahin.zip"
fi
say "done: $steps"

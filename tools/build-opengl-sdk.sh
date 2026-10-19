#!/usr/bin/env bash
# Build the launcher's ps5-opengl SDK from source and package it the way its releases were
# packaged, for launcher/tools/fetch-opengl-sdk.sh:
#
#   tools/build-opengl-sdk.sh PAYLOAD_SDK OUT_DIR
#
# -> OUT_DIR/ps5-opengl-sdk-<version>.tar.gz holding ps5-opengl-sdk-<version>/{sdk/, LICENSE,
#    LICENSES/, THIRD_PARTY_NOTICES.md, BUILD-INFO.txt}.
#
# The SDK's own releases (github.com/blackbearreloaded/ps5-opengl, v1.0.0 pinned by the
# launcher) are gone with that account; the source is the ps5-opengl submodule, a fork with
# the full history (mpereiraesaa/ps5-opengl), at the commit launcher/tools/fetch-opengl-sdk.sh
# pins with the version, archive and manifest hashes. Built as the project's release workflow
# (.github/workflows/release.yml) builds a tag: `make sdk-gl46`, the "dynamic" display profile
# (1080p60 at startup, 4K120 capacity, mode chosen at runtime), the batching flags on. Its
# toolchain is ours: the image's clang 18 behind the public payload SDK v0.42 (the launcher's
# own copy, tools/setup-native-dependencies.sh), Meson and Mako from the hash-pinned
# tools/opengl-sdk-requirements.txt. Mesa, the shader compiler and their sources are fetched
# by the project's tools/fetch-sources.py, at the commits and archive hash of its
# dependencies.json. Runs inside the build image (tools/build.sh's opengl-sdk step).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
payload=$(realpath -e -- "${1:?usage: $0 PAYLOAD_SDK OUT_DIR}")
out=${2:?usage: $0 PAYLOAD_SDK OUT_DIR}
src=$here/ps5-opengl
pins=$here/launcher/tools/fetch-opengl-sdk.sh
version=$(sed -n 's/^version=//p' "$pins")
commit=$(sed -n 's/^commit=//p' "$pins")
[[ -n $version && -n $commit ]] || { echo "no version/commit pin in $pins" >&2; exit 2; }
[[ $(git -C "$src" rev-parse HEAD) == "$commit" ]] ||
    { echo "ps5-opengl is at $(git -C "$src" rev-parse HEAD), $pins pins $commit" >&2; exit 2; }
[[ -x $payload/bin/prospero-clang ]] || { echo "no payload SDK at $payload" >&2; exit 2; }

venv=$here/.deps/opengl-python
if [[ ! -x $venv/bin/meson ]]; then
    rm -rf "$venv" && python3 -m venv "$venv"
    "$venv/bin/pip" install -q --no-deps --only-binary :all: --require-hashes \
        -r "$here/tools/opengl-sdk-requirements.txt"
fi
export PATH=$venv/bin:$PATH

# A fresh build every time: the project's scripts reuse what they find (the PS5 compiler
# archive, Meson's build tree), and a release is built from nothing. The verified downloads
# in third_party/ stay.
git -C "$src" diff --quiet HEAD -- . ':!third_party' || { echo "$src has local changes" >&2; exit 2; }
rm -rf "$src/build"
git -C "$src" clean -q -xdf -e third_party
[[ ! -d $src/third_party/opengnm-psbc ]] || git -C "$src/third_party/opengnm-psbc" clean -q -xdf
rm -rf "$src/third_party/mesa-26.2.0"   # patched in place by the build; extracted again
python3 "$src/tools/fetch-sources.py"

export PS5_PAYLOAD_SDK=$payload PS5_NATIVE_APP_TEMPLATE=$here/ps5-native-app-boilerplate
export PS5_SCANOUT_HEIGHT=2160 PS5_SCANOUT_FPS=120 PS5_DYNAMIC_SCANOUT=1
export PS5_GPU_PRESENT_BATCH=1 PS5_DRAW_PROFILE=1 PS5_MULTIDRAW_BATCH=1 PS5_DEFERRED_DRAW_BATCH=1
export PS5_CCACHE=0
make -C "$src" sdk-gl46

name=ps5-opengl-sdk-$version
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
mkdir "$stage/$name"
cp -a "$src/build/sdk/ps5-opengl-gl46" "$stage/$name/sdk"
(cd "$stage/$name/sdk" && sha256sum --check --strict --quiet manifest.sha256)
cp -a "$src/LICENSE" "$src/LICENSES" "$src/THIRD_PARTY_NOTICES.md" "$stage/$name/"
cp "$src/third_party/SPIRV-Headers/LICENSE" "$stage/$name/LICENSES/SPIRV-Headers.txt"
cp "$src/third_party/Vulkan-Headers/LICENSE.md" "$stage/$name/LICENSES/Vulkan-Headers.txt"
{
    printf 'ps5-opengl SDK %s, built from source by Sahin (tools/build-opengl-sdk.sh)\n\n' "$version"
    printf 'source: https://github.com/mpereiraesaa/ps5-opengl commit %s\n' "$commit"
    printf 'build: make sdk-gl46, display profile dynamic (PS5_SCANOUT_HEIGHT=2160 PS5_SCANOUT_FPS=120 PS5_DYNAMIC_SCANOUT=1)\n'
    printf 'flags: PS5_GPU_PRESENT_BATCH=1 PS5_DRAW_PROFILE=1 PS5_MULTIDRAW_BATCH=1 PS5_DEFERRED_DRAW_BATCH=1\n'
    printf 'compiler: %s\n' "$("$payload/bin/prospero-clang" --version | head -1)"
    printf 'payload SDK: v0.42 (ps5-payload-dev/sdk), not included\n'
    printf 'sdk/manifest.sha256: %s\n' "$(sha256sum "$stage/$name/sdk/manifest.sha256" | cut -d' ' -f1)"
} > "$stage/$name/BUILD-INFO.txt"

# The same bytes for the same commit: names sorted, one owner, the commit's time, no gzip
# name or time.
epoch=$(git -C "$src" show -s --format=%ct HEAD)
mkdir -p "$out"
tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
    --mode='u+rwX,go+rX,go-w' -C "$stage" -cf - "$name" | gzip -9n > "$out/$name.tar.gz.part"
mv "$out/$name.tar.gz.part" "$out/$name.tar.gz"
printf '==> %s.tar.gz %s\n==> sdk/manifest.sha256 %s\n' "$name" \
    "$(sha256sum "$out/$name.tar.gz" | cut -d' ' -f1)" \
    "$(sha256sum "$stage/$name/sdk/manifest.sha256" | cut -d' ' -f1)"

#!/usr/bin/env bash
# Build our DXVK: 3.1.1 with patches/dxvk/ (32-bit reference/use counters, threads woken
# after the lock is released), every DLL (d3d8, d3d9, d3d10core, d3d11, dxgi) for 32- and
# 64-bit, packed as a DXVK release archive.
#
#   tools/build-dxvk.sh            -> .deps/dxvk-3.1.1-sahin.tar.gz (path and sha256 printed)
#
# Runs in the build image (tools/build.sh): meson, ninja, glslang and the MinGW-w64 GCC. The
# archive's layout is DXVK's own (dxvk-<version>/x32|x64/<dll>.dll), so prospero-win's
# pw_install.py takes it; it is packed with sorted names and fixed times.
set -euo pipefail
W=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=3.1.1-sahin
commit=b1a1c99ab52b687cf950d62c88bc2fa316b41663
src=$W/.deps/dxvk-src
work=$W/.deps/dxvk-build
archive=$W/.deps/dxvk-$version.tar.gz

if [[ ! -d $src/.git ]]; then
    git clone -q --recurse-submodules --shallow-submodules --depth 1 --branch v3.1.1 \
        https://github.com/doitsujin/dxvk.git "$src"
fi
[[ $(git -C "$src" rev-parse HEAD~0 2>/dev/null) == "$commit" ]] ||
    git -C "$src" log --format=%H | grep -qx "$commit" ||
    { echo "$src is not DXVK v3.1.1 ($commit)" >&2; exit 2; }
if [[ $(git -C "$src" rev-parse HEAD) == "$commit" ]]; then
    git -C "$src" -c user.name=lavavex -c user.email=27239435+lavavex@users.noreply.github.com \
        am -q --committer-date-is-author-date "$W"/patches/dxvk/*.patch
fi
git -C "$src" log --oneline -3

# No link-time stamp in the PE headers, so the same source gives the same DLLs.
for cross in "$src/build-win32.txt" "$src/build-win64.txt"; do
    grep -q -- '--no-insert-timestamp' "$cross" ||
        printf "\n[built-in options]\nc_link_args = ['-Wl,--no-insert-timestamp']\ncpp_link_args = ['-Wl,--no-insert-timestamp']\n" >> "$cross"
done
# The strip step rewrites each DLL and would stamp the time again; binutils takes this one.
export SOURCE_DATE_EPOCH=1767225600
rm -rf "$work"
mkdir -p "$work"
(cd "$src" && ./package-release.sh "$version" "$work" --no-package)
dist=$work/dxvk-$version
for bits in x32 x64; do
    for dll in d3d8 d3d9 d3d10core d3d11 dxgi; do
        [[ -f $dist/$bits/$dll.dll ]] || { echo "missing $bits/$dll.dll" >&2; exit 2; }
    done
done
# Only the DLLs, sorted, fixed times and owners: the same build gives the same archive.
tar -C "$work" --sort=name --mtime=2026-01-01T00:00:00Z --owner=0 --group=0 --numeric-owner \
    -cf - "dxvk-$version/x32" "dxvk-$version/x64" | gzip -9n > "$archive.part"
mv "$archive.part" "$archive"
echo "$archive"
sha256sum "$archive"

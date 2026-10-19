#!/usr/bin/env bash
# Prospero BlackBox - The compiler builtins archive the PS5 link needs.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints the path of libclang_rt.builtins-x86_64.a. On an x86-64 Linux host the
# compiler ships it; elsewhere (Homebrew's LLVM on macOS has none for this target) it is
# built once from compiler-rt's own source, pinned by hash, with the same file list
# compiler-rt's CMake picks for x86-64 Linux, into .deps/native/compiler-rt.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=$(command -v "${PS5_CLANG:-clang-18}")
archiver=$(command -v llvm-ar-18 || command -v llvm-ar)
shipped="$("$compiler" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
if [[ -s $shipped ]]; then
    printf '%s\n' "$shipped"
    exit 0
fi

version=18.1.8
url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/compiler-rt-$version.src.tar.xz"
hash=e054e99a9c9240720616e927cb52363abbc8b4f1ef0286bad3df79ec8fdf892f
cache="$root/.deps/native/compiler-rt"
library="$cache/libclang_rt.builtins-x86_64.a"
stamp="$cache/.built"
sdk="$root/.deps/native/ps5-payload-sdk"
if [[ -s $library && -f $stamp && $(<"$stamp") == "$version $compiler" ]]; then
    printf '%s\n' "$library"
    exit 0
fi
[[ -d $sdk/target/include ]] || { echo "the PS5 payload SDK is needed first" >&2; exit 2; }

echo "==> [deps] Building compiler-rt $version builtins for x86-64" >&2
mkdir -p "$cache"
archive="$cache/compiler-rt-$version.src.tar.xz"
if [[ ! -f $archive ]] || ! printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --status; then
    wget -q "$url" -O "$archive.download"
    printf '%s  %s\n' "$hash" "$archive.download" | sha256sum --check --strict >/dev/null
    mv "$archive.download" "$archive"
fi
source="$cache/compiler-rt-$version.src"
rm -rf -- "$source" "$cache/objects"
tar -xJf "$archive" -C "$cache"
builtins="$source/lib/builtins"

# compiler-rt's list for x86_64 on Linux: the generic sources and their Linux extras,
# the 128-bit float and 80-bit x87 ones, the x86 CPU model, and x86_64's own, which
# replace the generic file of the same name.
mapfile -t files < <(python3 - "$builtins/CMakeLists.txt" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
def listed(name):
    found = re.search(r"set\(\s*" + name + r"\s*\n(.*?)\)", text, re.S)
    return re.findall(r"[\w/.]+\.(?:c|S)", found.group(1))
files = listed("GENERIC_SOURCES") + ["emutls.c", "enable_execute_stack.c", "eprintf.c",
         "gcc_personality_v0.c", "clear_cache.c"] + listed("GENERIC_TF_SOURCES") + \
        listed("x86_ARCH_SOURCES") + listed("x86_80_BIT_SOURCES") + \
        ["x86_64/floatdidf.c", "x86_64/floatdisf.c", "x86_64/floatundidf.S",
         "x86_64/floatundisf.S", "x86_64/floatdixf.c", "x86_64/floatundixf.S"]
own = {f.split("/")[-1].rsplit(".", 1)[0] for f in files if f.startswith("x86_64/")}
seen = []
for f in files:
    if ("/" not in f and f.rsplit(".", 1)[0] in own) or f in seen:
        continue
    seen.append(f)
print("\n".join(seen))
PY
)
mkdir -p "$cache/objects"
objects=()
for file in "${files[@]}"; do
    object="$cache/objects/${file//\//_}.o"
    "$compiler" -target x86_64-unknown-linux-gnu -O2 -fPIC -fno-builtin -fvisibility=hidden \
        -fomit-frame-pointer -std=c11 -nostdinc -isystem "$("$compiler" --print-resource-dir)/include" \
        -isystem "$sdk/target/include" -c "$builtins/$file" -o "$object"
    objects+=("$object")
done
rm -f -- "$library"
"$archiver" rcs "$library" "${objects[@]}"
printf '%s %s\n' "$version" "$compiler" >"$stamp"
echo "==> [deps] compiler-rt builtins: ${#objects[@]} objects" >&2
printf '%s\n' "$library"

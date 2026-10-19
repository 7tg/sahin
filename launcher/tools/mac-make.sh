#!/usr/bin/env bash
# Prospero BlackBox - make on macOS.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The build scripts are written for Linux: GNU coreutils/find/sed/tar, bash 4+, an LLVM
# with the PS5 target. This puts Homebrew's on the PATH first and runs make with
# Homebrew's clang, for example:
#
#     tools/mac-make.sh DEVELOPMENT=1 USE_CCACHE=0 app -j8
#
# (tools/store-test.sh needs none of this.) Missing packages are named, not installed.

set -euo pipefail
command -v brew >/dev/null || { echo "Homebrew is needed: https://brew.sh" >&2; exit 2; }
packages=(llvm bash coreutils findutils gnu-sed gnu-tar wget ninja)
missing=()
for package in "${packages[@]}"; do
    brew --prefix --installed "$package" >/dev/null 2>&1 || missing+=("$package")
done
if (( ${#missing[@]} > 0 )); then
    echo "missing Homebrew packages; install them with: brew install ${missing[*]}" >&2
    exit 2
fi
prefix=$(brew --prefix)
llvm=$(brew --prefix llvm)
gnu=""
for package in coreutils findutils gnu-sed gnu-tar; do
    gnu+="$(brew --prefix "$package")/libexec/gnubin:"
done
export PATH="$gnu$prefix/bin:$llvm/bin:$PATH"
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec make -C "$root" PS5_CLANG="$llvm/bin/clang" "$@"

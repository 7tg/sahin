#!/usr/bin/env bash
# ps5-native-app-boilerplate - Build the missing link-only CommonDialog facade.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ -z ${PS5_PAYLOAD_SDK:-} ]]; then
    bash "$root/tools/setup-native-dependencies.sh" >/dev/null
    export PS5_PAYLOAD_SDK="$root/.deps/native/ps5-payload-sdk"
fi
build="$root/build/system-keyboard"
mkdir -p "$build"
sh "$root/tooling/prospero-clang18" -std=c++20 -O2 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -fPIC -c "$root/examples/system-keyboard/common_dialog.cpp" \
    -o "$build/common_dialog.o"
"$PS5_PAYLOAD_SDK/bin/prospero-lld" --shared -soname libSceCommonDialog.sprx \
    -o "$build/libSceCommonDialog.so" "$build/common_dialog.o"
printf '%s\n' "$build/libSceCommonDialog.so"

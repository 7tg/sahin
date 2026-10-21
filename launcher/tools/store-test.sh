#!/usr/bin/env bash
# Prospero BlackBox - Sanitized store-specific host checks.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/build/store-tests"
mkdir -p "$build"
python3 "$root/tools/store-smoke.py" --self-test
python3 "$root/tools/check-i18n.py"
flags=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
# On macOS, Apple's ASan has no leak checker and Apple's clang no libFuzzer; Homebrew's
# LLVM (brew install llvm) has libFuzzer, so the fuzzing uses it when it's there.
leaks=1
fuzz_cc=clang
fuzz_cxx=clang++
if [[ $(uname -s) == Darwin ]]; then
    leaks=0
    llvm=$(brew --prefix llvm 2>/dev/null || true)
    if [[ -n $llvm && -x $llvm/bin/clang ]]; then
        fuzz_cc=$llvm/bin/clang
        fuzz_cxx=$llvm/bin/clang++
    fi
fi
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_i18n_test.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" \
    -o "$build/i18n-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/i18n-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_locations_test.cpp" "$root/src/system/locations.cpp" -o "$build/locations-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/locations-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_storage_test.cpp" "$root/src/system/storage_probe.cpp" \
    "$root/src/system/locations.cpp" -o "$build/storage-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/storage-test"
clang++ -std=c++20 "${flags[@]}" -DSTORE_NATIVE_PATH_WALK=1 -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_storage_test.cpp" "$root/src/system/storage_probe.cpp" \
    "$root/src/system/locations.cpp" -o "$build/storage-native-path-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/storage-native-path-test"
objects=()
for name in monocypher/monocypher monocypher/monocypher-ed25519 yyjson/yyjson qrcodegen/qrcodegen; do
    object="$build/${name//\//_}.o"
    clang -std=c11 "${flags[@]}" -c "$root/src/third_party/$name.c" -o "$object"
    objects+=("$object")
done
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_catalog_test.cpp" "$root/src/catalog/catalog.cpp" "${objects[@]}" \
    -o "$build/catalog-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/catalog-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_inventory_test.cpp" "$root/src/system/inventory.cpp" \
    "$root/src/system/locations.cpp" "$root/src/catalog/catalog.cpp" "${objects[@]}" \
    -o "$build/inventory-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/inventory-test"
miniz=()
for name in miniz miniz_tinfl miniz_tdef miniz_zip; do
    object="$build/miniz_$name.o"
    clang -std=c11 "${flags[@]}" -w -c "$root/src/third_party/miniz/$name.c" -o "$object"
    miniz+=("$object")
done
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_install_test.cpp" "$root/src/install/archive.cpp" \
    "$root/src/install/files.cpp" "$root/src/install/transaction.cpp" \
    "$root/src/system/inventory.cpp" "$root/src/system/locations.cpp" \
    "$root/src/system/storage_probe.cpp" "$root/src/system/running.cpp" \
    "$root/src/catalog/catalog.cpp" \
    "$root/src/install/worker.cpp" \
    "$root/src/core/save_file.cpp" "${objects[@]}" "${miniz[@]}" -o "$build/install-test"
# The file worker as a host program: the same sources the console payload is built from.
clang++ -std=c++20 "${flags[@]}" -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror \
    -I"$root/src" "$root/helper/main.cpp" "$root/src/install/archive.cpp" \
    "$root/src/install/files.cpp" "$root/src/install/worker.cpp" "${miniz[@]}" -pthread \
    -o "$build/store-worker"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/install-test" "$build/store-worker"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_menu_sfx_test.cpp" "$root/src/audio/cues.cpp" "$root/src/audio/wav.cpp" \
    "$root/src/audio/mixer.cpp" "$root/src/core/save_file.cpp" -o "$build/menu-sfx-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 \
    "$build/menu-sfx-test" "$root/assets/audio/sfx"
# Fuzz the ZIP and JSON readers from valid seeds. STORE_FUZZ_SECONDS=0 skips it.
fuzz_seconds=${STORE_FUZZ_SECONDS:-20}
if (( fuzz_seconds > 0 )); then
    fuzzed=()
    for name in monocypher/monocypher monocypher/monocypher-ed25519 yyjson/yyjson \
        miniz/miniz miniz/miniz_tinfl miniz/miniz_tdef miniz/miniz_zip; do
        object="$build/fuzz_${name//\//_}.o"
        "$fuzz_cc" -std=c11 "${flags[@]}" -fsanitize=fuzzer-no-link -w -c \
            "$root/src/third_party/$name.c" -o "$object"
        fuzzed+=("$object")
    done
    "$fuzz_cxx" -std=c++20 "${flags[@]}" -fsanitize=fuzzer -Wall -Wextra -Wpedantic -Werror \
        -I"$root/src" "$root/tests/store_fuzz.cpp" "$root/src/install/archive.cpp" \
        "$root/src/install/files.cpp" "$root/src/catalog/catalog.cpp" "${fuzzed[@]}" \
        -o "$build/store-fuzz"
    rm -rf "$build/fuzz-corpus"
    mkdir -p "$build/fuzz-corpus"
    python3 - "$build/fuzz-corpus" <<'PY'
import io, json, sys, zipfile
from pathlib import Path
corpus = Path(sys.argv[1])
param = json.dumps({"titleId": "PPSA99500", "contentVersion": "01.000.001"})
for level, method in enumerate((zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", method) as archive:
        archive.writestr("PPSA99500/", "")
        archive.writestr("PPSA99500/eboot.bin", b"program" * 64)
        archive.writestr("PPSA99500/sce_sys/param.json", param)
    (corpus / f"zip-{level}").write_bytes(b"\x01" + data.getvalue())
records = [
    param,
    json.dumps({"schema": 1, "titleId": "PPSA99500", "location": "/data/homebrew",
                "contentVersion": "01.000.001", "releaseTag": "v1", "sha256": "a" * 64,
                "installedAt": "2026-10-02T00:00:00Z"}),
    json.dumps({"schema": 1, "operation": "update", "state": "swap", "titleId": "PPSA99500",
                "location": "/data/homebrew", "contentVersion": "01.000.002",
                "sha256": "b" * 64}),
    json.dumps({"schema": 3, "apps": [{"titleid": "PPSA99500", "name": "App", "kind": "app",
                                       "status": "available", "format": "zip", "size": 1}]}),
    json.dumps({"schema": 3, "apps": {"PPSA99500": {"content_version": "01.000.001"}}}),
]
for index, record in enumerate(records):
    (corpus / f"json-{index}").write_bytes(b"\x00" + record.encode())
PY
    ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/store-fuzz" \
        -max_total_time="$fuzz_seconds" -max_len=65536 -rss_limit_mb=2048 -print_final_stats=0 \
        "$build/fuzz-corpus" > "$build/fuzz.log" 2>&1 || { tail -40 "$build/fuzz.log"; exit 1; }
    printf 'Fuzzing passed: %s\n' "$(grep -c '' "$build/fuzz.log") log lines, ${fuzz_seconds}s"
fi
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_base_game_test.cpp" "$root/src/library/base_game.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" "$root/src/library/tree.cpp" "${miniz[@]}" \
    -o "$build/base-game-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/base-game-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_early_exit_test.cpp" "$root/src/app/early_exit.cpp" -o "$build/early-exit-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/early-exit-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_wine_registry_test.cpp" "$root/src/library/wine_registry.cpp" \
    "$root/src/core/save_file.cpp" -o "$build/registry-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/registry-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_game_profile_test.cpp" "$root/src/library/game_profile.cpp" \
    "$root/src/core/save_file.cpp" -o "$build/profile-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/profile-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    -DSTORE_ASSETS="\"$root/assets\"" "$root/tests/store_game_install_test.cpp" \
    "$root/src/library/game_install.cpp" "$root/src/library/upgrade.cpp" "$root/src/library/rtpatch.cpp" "$root/src/library/install_plan.cpp" \
    "$root/src/library/game_profile.cpp" "$root/src/library/wine_registry.cpp" \
    "$root/src/library/base_game.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" "$root/src/library/tree.cpp" "$root/src/core/save_file.cpp" \
    "${objects[@]}" "${miniz[@]}" -o "$build/game-install-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/game-install-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_rtpatch_test.cpp" "$root/src/library/rtpatch.cpp" "$root/src/library/base_game.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" \
    "$root/src/library/install_plan.cpp" "$root/src/library/game_profile.cpp" "$root/src/library/tree.cpp" \
    "$root/src/core/save_file.cpp" "${objects[@]}" "${miniz[@]}" -o "$build/rtpatch-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/rtpatch-test" "$build/rtpatch-synthetic"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_game_sources_test.cpp" "$root/src/library/game_sources.cpp" \
    "$root/src/library/game_install.cpp" "$root/src/library/upgrade.cpp" "$root/src/library/rtpatch.cpp" \
    "$root/src/library/install_plan.cpp" "$root/src/library/game_profile.cpp" "$root/src/library/wine_registry.cpp" \
    "$root/src/library/base_game.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" "$root/src/library/tree.cpp" "$root/src/core/save_file.cpp" \
    "${objects[@]}" "${miniz[@]}" -o "$build/game-sources-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/game-sources-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_prefix_test.cpp" "$root/src/library/prefix.cpp" "$root/src/library/install_plan.cpp" \
    "$root/src/library/game_profile.cpp" "$root/src/library/tree.cpp" "$root/src/core/save_file.cpp" \
    "${objects[@]}" "${miniz[@]}" -o "$build/prefix-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/prefix-test"
# The reference decoder on the same made-up patch: the files it writes must be the expected ones.
rm -rf "$build/rtpatch-synthetic/out"
python3 "$root/tools/rtpatch.py" "$build/rtpatch-synthetic/patch.exe" "$build/rtpatch-synthetic/old" \
    "$build/rtpatch-synthetic/out" > /dev/null
diff -r "$build/rtpatch-synthetic/expected" "$build/rtpatch-synthetic/out"
printf 'tools/rtpatch.py checks passed\n'
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_http_test.cpp" "$root/src/catalog/catalog.cpp" \
    "$root/src/net/http.cpp" "${objects[@]}" -o "$build/http-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/http-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_icons_test.cpp" "$root/src/catalog/catalog.cpp" \
    "$root/src/catalog/icons.cpp" "$root/src/core/image.cpp" "$root/src/core/save_file.cpp" \
    "${objects[@]}" -o "$build/icons-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/icons-test"
clang -std=c11 "${flags[@]}" -Wall -Wextra -Werror -c "$root/src/system/update_check.c" \
    -o "$build/update_check.o"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_service_test.cpp" "$root/src/app/service.cpp" \
    "$root/src/system/inventory.cpp" "$root/src/system/locations.cpp" \
    "$root/src/core/qr.cpp" "$root/src/catalog/blackbox.cpp" "$root/src/library/base_game.cpp" "$root/src/core/i18n.cpp" "$root/src/core/i18n_strings.cpp" "$root/src/library/tree.cpp" "$root/src/library/upgrade.cpp" "$root/src/library/rtpatch.cpp" "$root/src/library/game_sources.cpp" "$root/src/library/prefix.cpp" "$root/src/library/game_install.cpp" "$root/src/library/install_plan.cpp" "$root/src/library/game_profile.cpp" "$root/src/library/wine_registry.cpp" "$root/src/system/app_folder.cpp" \
    "$root/src/catalog/icons.cpp" "$root/src/catalog/catalog.cpp" \
    "$root/src/core/image.cpp" "$root/src/core/save_file.cpp" "$root/host/platform_host.cpp" \
    "$root/src/install/archive.cpp" "$root/src/install/files.cpp" \
    "$root/src/install/transaction.cpp" "$root/src/system/storage_probe.cpp" \
    "$root/src/system/running.cpp" "$root/src/install/worker.cpp" \
    "${objects[@]}" "${miniz[@]}" "$build/update_check.o" -pthread -o "$build/service-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 "$build/service-test"
clang++ -std=c++20 "${flags[@]}" -Wall -Wextra -Wpedantic -Werror -I"$root/src" \
    "$root/tests/store_curl_test.cpp" "$root/host/http.cpp" "$root/src/net/curl_request.cpp" \
    "$root/src/net/http.cpp" "$root/src/catalog/catalog.cpp" "${objects[@]}" \
    -lcurl -o "$build/curl-test"
ASAN_OPTIONS=detect_leaks=$leaks UBSAN_OPTIONS=halt_on_error=1 \
    python3 "$root/tests/run_curl_tls.py" "$build/curl-test"

#!/usr/bin/env bash
# ps5-homebrew-ui - Pinned ps5-opengl SDK.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verifies the pinned ps5-opengl SDK archive, unpacks it into .deps/ps5-opengl and prints the
# SDK prefix (the directory holding manifest.sha256).
#
# Sahin: the SDK's release downloads (github.com/blackbearreloaded/ps5-opengl, v1.0.0) are
# gone, so the SDK is built from source: the repository's ps5-opengl submodule (the fork
# github.com/mpereiraesaa/ps5-opengl) at `commit`, by tools/build-opengl-sdk.sh (the
# opengl-sdk step of tools/build.sh), which reads the version and commit from here. The build
# is reproducible, so the hashes below are its output's. The archive is taken from
# .deps/ps5-opengl/ or PS5_OPENGL_ARCHIVE; nothing is downloaded.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=0.6.0-31-gb696cd3
commit=b696cd39d63a4e346da3c680a31238b41ae359a8
archive_sha256=659776cf824da758d048de6339101fb7002f3abc70efddc5ad2f85ea09ce5c40
manifest_sha256=22acd4607ec796381fe7e1bf93b858c5f276caab53bcc9146d1091f2da5256e3

cache="$root/.deps/ps5-opengl"
archive="$cache/ps5-opengl-sdk-$version.tar.gz"
extracted="$cache/ps5-opengl-sdk-$version"
prefix="$extracted/sdk"
mkdir -p "$cache"

if [[ ! -f $prefix/manifest.sha256 ]]; then
    if [[ -n ${PS5_OPENGL_ARCHIVE:-} ]]; then
        archive=$PS5_OPENGL_ARCHIVE
    fi
    [[ -f $archive ]] || {
        echo "no ps5-opengl SDK archive at $archive: build it with Sahin's" \
            "tools/build.sh opengl-sdk (release/ps5-opengl-sdk-$version.tar.gz)" >&2
        exit 2
    }
    sha256sum --check --status <<<"$archive_sha256  $archive" || {
        echo "ps5-opengl SDK archive checksum mismatch: $archive" >&2
        exit 2
    }
    rm -rf -- "$extracted"
    # Only the compiled SDK and the license texts are needed to build.
    tar -xzf "$archive" -C "$cache" "ps5-opengl-sdk-$version/sdk" \
        "ps5-opengl-sdk-$version/LICENSE" "ps5-opengl-sdk-$version/LICENSES" \
        "ps5-opengl-sdk-$version/THIRD_PARTY_NOTICES.md"
fi

(cd "$prefix" && sha256sum --check --strict --quiet manifest.sha256) || {
    echo "ps5-opengl SDK manifest verification failed: $prefix" >&2
    exit 2
}
actual=$(sha256sum "$prefix/manifest.sha256" | cut -d' ' -f1)
[[ $actual == "$manifest_sha256" ]] || {
    echo "unexpected ps5-opengl SDK manifest: $actual" >&2
    exit 2
}
printf '%s\n' "$prefix"

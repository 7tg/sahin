#!/usr/bin/env bash
# Build Sahin, the same way on any machine: prospero-win (the Wine runtime and Blur's runner),
# our DXVK, the bare prefix, the launcher's bundle and the app, PPSA99404.
#
#   tools/build.sh [--clean] [STEP...]
#
# It builds the committed HEAD of this repo, never the working tree: the repo and its five
# submodules (each at its gitlink commit) are cloned from the working copy, mounted read-only,
# into a Docker volume and built there, at fixed paths, inside the pinned image
# (tools/container/Dockerfile: x86-64 Ubuntu 24.04, packages from one snapshot day). Outputs,
# with release/SHA256SUMS, are copied to release/. The same commit gives the same SHA256SUMS.
# x86-64 Linux only: the prefix step runs 32-bit x86 Wine.
#
# The launcher needs ps5-opengl-sdk-1.0.0.tar.gz (sha256 f93643c0...) in downloads/: its
# release download is gone.
#
# STEPs, in order:
#   setup title radv zink hostwine wineps5 cpu pkg apps dxvk prefix
#             tools/build-prospero-win.sh's steps
#   plan      profile/blur/make-blur-plan.py; fails if the committed
#             launcher/assets/plans/BLUR.json differs from what it writes
#   bundle    tools/make-bundle.py: the bare prefix into the launcher's assets/bundle
#   game-app  the app: the launcher as eboot.bin, the runner (apps step) as game.bin
#             -> release/PPSA99404/ and release/PPSA99404.zip
#   audit     tools/audit.py over the committed tree and release/PPSA99404 (the PE files
#             built from source are listed in tools/app.audit-allow)
# Default: all. --clean starts from an empty volume (SAHIN_VOLUME names it; default sahin-work).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
image=sahin-build volume=${SAHIN_VOLUME:-sahin-work}
pw_steps="setup title radv zink hostwine wineps5 cpu pkg apps dxvk prefix"
all="$pw_steps plan bundle game-app audit"
clean=0
[[ ${1:-} == --clean ]] && { clean=1; shift; }
steps=" ${*:-$all} "
for s in $steps; do
    [[ " $all " == *" $s "* ]] || { echo "unknown step: $s (steps: $all)" >&2; exit 2; }
done
case $(uname -sm) in
"Linux x86_64") ;;
*) echo "tools/build.sh builds on x86-64 Linux only (the prefix step runs 32-bit x86 Wine);" \
        "this machine is $(uname -sm)" >&2
   exit 2 ;;
esac

git -C "$here" diff --quiet HEAD -- . ':!PS5_Mesa' ':!PS5_Vulkan' ':!prospero-win' \
    ':!PS5_PayloadSDK' ':!ps5-native-app-boilerplate' ||
    echo "==> note: $here has uncommitted changes; they are not built" >&2
# The image is built once per Dockerfile: it carries the Dockerfile's git hash as a label, so an
# image built here before, or copied in (docker save | docker load), is used as it is.
recipe=$(git -C "$here" hash-object tools/container/Dockerfile)
[[ $(docker image inspect -f '{{index .Config.Labels "sahin.dockerfile"}}' "$image" 2>/dev/null) == "$recipe" ]] ||
    docker build -q --platform linux/amd64 --label "sahin.dockerfile=$recipe" -t "$image" "$here/tools/container" >/dev/null
(( clean )) && docker volume rm -f "$volume" >/dev/null
docker volume create "$volume" >/dev/null
mkdir -p "$here/release"

pw=; for s in $steps; do [[ " $pw_steps " == *" $s "* ]] && pw+=" $s"; done
docker run --rm --platform linux/amd64 -v "$volume:/w" -v "$here:/src/sahin:ro" -v "$here/release:/out" \
    -e REV="$(git -C "$here" rev-parse HEAD)" -e OWNER="$(id -u):$(id -g)" \
    "$image" bash -euo pipefail -c "
want() { [[ '$steps' == *\" \$1 \"* ]]; }
checkout() { # source dir commit
    [[ -d \$2/.git ]] || git clone -q --no-checkout \"\$1\" \"\$2\"
    git -C \"\$2\" fetch -q \"\$1\" \"\$3\"
    git -C \"\$2\" checkout -q --force --detach \"\$3\"
}
checkout /src/sahin /w/sahin \"\$REV\"
cd /w/sahin
# The submodules, from the working copy's own clones: each cloned once at its gitlink commit
# (again only when that commit is new to it); the setup step makes its patched branch.
for sub in prospero-win PS5_Mesa PS5_Vulkan PS5_PayloadSDK ps5-native-app-boilerplate; do
    sha=\$(git ls-tree HEAD \$sub | awk '{print \$3}')
    [[ -d \$sub/.git ]] && git -C \$sub cat-file -e \"\$sha^{commit}\" 2>/dev/null && continue
    checkout /src/sahin/\$sub /w/sahin/\$sub \$sha
done

if [[ -n '$pw' ]]; then tools/build-prospero-win.sh $pw; fi

L=/w/sahin/launcher A=release/PPSA99404
if want plan; then
    echo '==> plan'
    python3 profile/blur/make-blur-plan.py
    git diff --exit-code -- launcher/assets/plans/BLUR.json ||
        { echo 'make-blur-plan.py writes another plan than the committed launcher/assets/plans/BLUR.json' >&2; exit 1; }
fi
if want bundle; then
    echo '==> bundle'
    python3 tools/make-bundle.py --launcher \$L
fi
if want game-app; then
    # The launcher (GNUmakefile GAME=PPSA99404) as eboot.bin, prospero-win's runner (the apps
    # step) as game.bin, with the bundle.
    echo '==> game-app'
    # The launcher's pinned ps5-opengl SDK: its release address is gone (404), so a copy put
    # in the working copy's downloads/ is used; launcher/tools/fetch-opengl-sdk.sh checks
    # its hash before unpacking it.
    sdk=ps5-opengl-sdk-1.0.0.tar.gz
    if [[ -f /src/sahin/downloads/\$sdk && ! -d \$L/.deps/ps5-opengl/ps5-opengl-sdk-1.0.0/sdk ]]; then
        mkdir -p \$L/.deps/ps5-opengl && cp /src/sahin/downloads/\$sdk \$L/.deps/ps5-opengl/
    fi
    make -C \$L USE_CCACHE=0 GAME=PPSA99404 app
    D=\$L/dist/PPSA99404
    rm -rf \$D/assets/bundle && cp -a \$L/assets/bundle \$D/assets/bundle
    rm -rf \$A release/PPSA99404.zip && cp -a release/apps/PPSA99404 \$A
    mv \$A/eboot.bin \$A/game.bin
    cp \$D/eboot.bin \$D/store-worker.elf \$D/lapy.elf \$D/lapy-manifest.json \$A/
    # The launcher's licences go in with the runner's: a separate licenses/ beside LICENSES/
    # is one folder on a case-insensitive disk (a player unzipping on Windows or a Mac).
    cp -a \$D/assets \$A/assets && cp -a \$D/licenses/. \$A/LICENSES/
    python3 tools/make-app-zip.py \$A release/PPSA99404.zip
fi
if want audit; then
    echo '==> audit'
    rm -rf /tmp/tree && mkdir /tmp/tree && git archive HEAD | tar -x -C /tmp/tree
    python3 tools/audit.py /tmp/tree
    # The app folder, with the PE files built from source allowed; the list is not shipped.
    cp tools/app.audit-allow \$A/.audit-allow
    rc=0; python3 tools/audit.py \$A || rc=\$?
    rm -f \$A/.audit-allow
    (( rc == 0 )) || exit \$rc
fi

# The outputs, and their hashes.
out='PPSA99995 apps PPSA99404 PPSA99404.zip bare-prefix-sahin.zip'
for o in \$out SHA256SUMS; do rm -rf /out/\$o; [[ ! -e release/\$o ]] || cp -a release/\$o /out/; done
cd /out
for o in \$out; do if [[ -e \$o ]]; then find ./\$o -type f -print0; fi; done | sort -z | xargs -0 -r sha256sum > SHA256SUMS
chown -R \"\$OWNER\" \$out SHA256SUMS 2>/dev/null || true
echo \"==> \$(wc -l < SHA256SUMS) files; release/SHA256SUMS: \$(sha256sum SHA256SUMS | cut -c1-16)\""

#!/usr/bin/env bash
# Build everything, the same way on any machine: the prospero-win package and the games'
# Home apps, our DXVK, the bare prefix, the manager's bundle and the manager app.
#
#   tools/build.sh [--clean] [STEP...]
#
# It builds the committed HEAD of this repo and of the manager beside it
# (../prospero-blackbox, or $MANAGER), never the working trees: both are cloned at those
# commits into a Docker volume and built there, at fixed paths, inside the pinned image
# (tools/container/Dockerfile: x86-64 Ubuntu 24.04, packages from one snapshot day).
# Outputs, with release/SHA256SUMS, are copied to release/. The same commits give the same
# SHA256SUMS.
#
# On x86-64 Linux everything runs in one container. On an Apple-silicon Mac the build runs
# in colima's default VM through Rosetta, which runs no 32-bit x86 code, so the prefix step
# (Wine creating a WoW64 prefix) runs in a second, fully emulated x86-64 colima VM,
# sahin-x86 (colima start sahin-x86 --arch x86_64 --vm-type qemu; needs brew's qemu), with
# the same image copied over.
#
# STEPs: the steps of tools/build-prospero-win.sh, then x360 (tools/build-x360-bits.sh),
# settings-mod (tools/build-settings-mod.sh: the PS5 app's settings in MW's own Options menu),
# game-apps (MW's own app and its zip, release/game-apps/: the catalog's release asset),
# bundle
# (tools/make-bundle.py) and
# manager (its app with the bundle); default all. --clean starts from an empty volume
# (SAHIN_VOLUME names it; default sahin-work).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
manager=$(cd -- "${MANAGER:-$here/../prospero-blackbox}" && pwd)
image=sahin-build volume=${SAHIN_VOLUME:-sahin-work}
clean=0
[[ ${1:-} == --clean ]] && { clean=1; shift; }
steps=" ${*:-setup title radv zink hostwine wineps5 cpu pkg apps dxvk prefix x360 settings-mod bundle manager game-apps} "
main_sock=${DOCKER_HOST:-}
if [[ -z $main_sock && -S $HOME/.colima/default/docker.sock ]]; then
    main_sock=unix://$HOME/.colima/default/docker.sock
fi
x86_sock=unix://$HOME/.colima/sahin-x86/docker.sock
split=false
[[ $(uname -m) == arm64 && $steps == *" prefix "* ]] && split=true
d() { DOCKER_HOST=$main_sock docker "$@"; }
x() { DOCKER_HOST=$x86_sock docker "$@"; }

for repo in "$here" "$manager"; do
    git -C "$repo" diff --quiet HEAD -- . ':!PS5_Mesa' ':!PS5_Vulkan' ':!prospero-win' ||
        echo "==> note: $repo has uncommitted changes; they are not built" >&2
done
# The image is built once per Dockerfile: it carries the Dockerfile's git hash as a label, so an
# image built here before, or copied in (docker save | docker load), is used as it is.
recipe=$(git -C "$here" hash-object tools/container/Dockerfile)
[[ $(d image inspect -f '{{index .Config.Labels "sahin.dockerfile"}}' "$image" 2>/dev/null) == "$recipe" ]] ||
    d build -q --platform linux/amd64 --label "sahin.dockerfile=$recipe" -t "$image" "$here/tools/container" >/dev/null
(( clean )) && d volume rm -f "$volume" >/dev/null
d volume create "$volume" >/dev/null
mkdir -p "$here/release"
run() { # docker-function script: runs a script in the image with the work volume at /w
    "$1" run --rm --platform linux/amd64 -v "$volume:/w" -v "$here:/src/nfs-blackbox-ps5:ro" \
        -v "$manager:/src/prospero-blackbox:ro" -v "$here/release:/out" \
        -e REV="$(git -C "$here" rev-parse HEAD)" -e MREV="$(git -C "$manager" rev-parse HEAD)" \
        "$image" bash -euo pipefail -c "$2"
}

pw=; for s in $steps; do case $s in bundle|manager|x360|menu|settings-mod|game-apps) ;; prefix) $split || pw+=" $s" ;; *) pw+=" $s" ;; esac; done
run d "
checkout() { # source dir commit
    [[ -d \$2/.git ]] || git clone -q --no-checkout \"\$1\" \"\$2\"
    git -C \"\$2\" fetch -q \"\$1\" \"\$3\"
    git -C \"\$2\" checkout -q --force --detach \"\$3\"
}
checkout /src/nfs-blackbox-ps5 /w/nfs-blackbox-ps5 \"\$REV\"
checkout /src/prospero-blackbox /w/prospero-blackbox \"\$MREV\"
cd /w/nfs-blackbox-ps5
if [[ -n '$pw' ]]; then tools/build-prospero-win.sh $pw; fi
if [[ '$steps' == *' x360 '* ]]; then tools/build-x360-bits.sh; fi
if [[ '$steps' == *' menu '* ]]; then tools/build-blackbox-menu.sh; fi
if [[ '$steps' == *' settings-mod '* ]]; then tools/build-settings-mod.sh; fi"

if $split; then
    echo "==> bare prefix in the x86-64 VM (sahin-x86)"
    x info >/dev/null 2>&1 ||
        colima start sahin-x86 --arch x86_64 --vm-type qemu --cpu 8 --memory 8 --disk 60 ||
        { echo "can't start the x86-64 VM (brew install colima qemu lima-additional-guestagents)" >&2; exit 2; }
    [[ $(x image inspect -f '{{.Id}}' "$image" 2>/dev/null) == "$(d image inspect -f '{{.Id}}' "$image")" ]] ||
        d save "$image" | x load -q
    (( clean )) && x volume rm -f "$volume" >/dev/null
    x volume create "$volume" >/dev/null
    # What make-bare-prefix.py reads, from the main volume to the x86-64 one.
    d run --rm --platform linux/amd64 -v "$volume:/w" "$image" tar -C /w -c \
        nfs-blackbox-ps5/tools nfs-blackbox-ps5/profiles nfs-blackbox-ps5/.deps/dxvk-3.1.1-sahin.tar.gz \
        nfs-blackbox-ps5/prospero-win/tools nfs-blackbox-ps5/prospero-win/.deps/mesa-python \
        nfs-blackbox-ps5/prospero-win/.deps/wine-host/install \
        nfs-blackbox-ps5/prospero-win/.deps/wine-host/build/dlls/wowprospero/x86_64-windows/wowprospero.dll \
        nfs-blackbox-ps5/prospero-win/.deps/wow64native/x86_64-windows/wow64native.dll |
        x run --rm -i --platform linux/amd64 -v "$volume:/w" "$image" \
            bash -c 'rm -rf /w/nfs-blackbox-ps5 && tar -C /w -x'
    x run --rm --platform linux/amd64 -v "$volume:/w" "$image" bash -euo pipefail -c '
        cd /w/nfs-blackbox-ps5 && mkdir -p release
        python3 tools/make-bare-prefix.py release/bare-prefix-sahin.zip'
    x run --rm --platform linux/amd64 -v "$volume:/w" "$image" cat /w/nfs-blackbox-ps5/release/bare-prefix-sahin.zip |
        d run --rm -i --platform linux/amd64 -v "$volume:/w" "$image" \
            bash -c 'mkdir -p /w/nfs-blackbox-ps5/release && cat > /w/nfs-blackbox-ps5/release/bare-prefix-sahin.zip'
fi

run d "
cd /w/nfs-blackbox-ps5
if [[ '$steps' == *' bundle '* ]]; then python3 tools/make-bundle.py; fi
if [[ '$steps' == *' manager '* ]]; then make -C /w/prospero-blackbox DEVELOPMENT=1 USE_CCACHE=0 app; fi
if [[ '$steps' == *' game-apps '* ]]; then
    # MW's own app, the catalog's release asset: the launcher (the manager built for the game,
    # from its own checkout of the same commit) as eboot.bin, prospero-win's runner (the apps
    # step) as game.bin, a bundle without Home apps or the 360 zips.
    L=/w/game-PPSA99403
    [[ -d \$L/.git ]] || git clone -q --no-checkout /src/prospero-blackbox \$L
    git -C \$L fetch -q /src/prospero-blackbox \"\$MREV\" && git -C \$L checkout -q --force --detach \"\$MREV\"
    [[ -d \$L/.deps || ! -d /w/prospero-blackbox/.deps ]] || cp -a /w/prospero-blackbox/.deps \$L/.deps
    make -C \$L NFSBB=/w/nfs-blackbox-ps5 USE_CCACHE=0 GAME=PPSA99403 app
    python3 tools/make-bundle.py --manager \$L --for-app
    D=\$L/dist/PPSA99403 A=release/game-apps/PPSA99403
    rm -rf \$D/assets/bundle && cp -a \$L/assets/bundle \$D/assets/bundle
    rm -rf release/game-apps && mkdir -p release/game-apps && cp -a release/apps/PPSA99403 \$A
    mv \$A/eboot.bin \$A/game.bin
    cp \$D/eboot.bin \$D/store-worker.elf \$D/lapy.elf \$D/lapy-manifest.json \$A/
    # The launcher's licences go in with the runner's: a separate licenses/ beside LICENSES/
    # is one folder on a case-insensitive disk (a player unzipping on Windows or a Mac).
    cp -a \$D/assets \$A/assets && cp -a \$D/licenses/. \$A/LICENSES/
    python3 tools/make-app-zip.py \$A release/game-apps/PPSA99403.zip
fi
# The outputs, and their hashes.
rm -rf /out/PPSA99995 /out/apps /out/x360 /out/menu /out/settings-mod /out/game-apps /out/PPSA99400 /out/bare-prefix-sahin.zip /out/SHA256SUMS
cp -a release/. /out/
if [[ -d /w/prospero-blackbox/dist/PPSA99400 ]]; then cp -a /w/prospero-blackbox/dist/PPSA99400 /out/; fi
# Only what this build made: release/ may hold other files (tools/make-x360-assets.py's zips).
cd /out && find ./PPSA99995 ./apps ./x360 ./menu ./settings-mod ./game-apps ./PPSA99400 ./bare-prefix-sahin.zip -type f -print0 2>/dev/null |
    sort -z | xargs -0 sha256sum > SHA256SUMS
echo \"==> \$(wc -l < SHA256SUMS) files; release/SHA256SUMS: \$(sha256sum SHA256SUMS | cut -c1-16)\""

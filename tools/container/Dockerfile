# The one build machine: x86-64 Ubuntu 24.04, pinned by digest, its packages pinned to one
# day of Ubuntu's snapshot archive. tools/build.sh builds everything in it, on Linux or on a
# Mac (colima with Rosetta), so the same commit gives the same bytes anywhere.
FROM --platform=linux/amd64 ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ARG SNAPSHOT=20261012T000000Z
# The snapshot archive is HTTPS only, so the CA bundle comes first from the live archive;
# then every source is the snapshot, and ca-certificates is reinstalled from it.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates && \
    sed -i -E "s#^URIs: .*#URIs: https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT/#" /etc/apt/sources.list.d/ubuntu.sources && \
    rm -rf /var/lib/apt/lists/* && apt-get update && \
    ca=$(apt-cache policy ca-certificates | sed -n 's/^ *Candidate: //p') && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall --allow-downgrades "ca-certificates=$ca" && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends --allow-downgrades \
        bash git curl wget unzip zip xz-utils file rsync patch \
        build-essential pkg-config bison flex gettext cmake ninja-build \
        python3 python3-venv python3-pip \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev clang lld llvm \
        gcc-mingw-w64-i686 gcc-mingw-w64-x86-64 g++-mingw-w64-i686 g++-mingw-w64-x86-64 \
        glslang-tools spirv-tools llvm-spirv-18 libclc-18-dev \
        llvm-18-dev libclang-18-dev libclang-cpp18-dev libpolly-18-dev libllvmspirvlib-18-dev \
        cabextract zlib1g-dev ffmpeg && \
    rm -rf /var/lib/apt/lists/*

# C order everywhere (the PS5 links glob the SDK's stubs; see HANDOFF "C locale"), UTC, and
# one fixed time for every tool that stamps one (PE headers, __DATE__, archives).
ENV LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1767225600
# Git refuses repositories owned by another user (the bind-mounted checkouts).
RUN git config --system --add safe.directory '*' && \
    git config --system user.name lavavex && \
    git config --system user.email 27239435+lavavex@users.noreply.github.com
WORKDIR /w

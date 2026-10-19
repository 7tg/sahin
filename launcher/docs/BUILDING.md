# Building Prospero BlackBox

Built on Linux (developed on Arch/CachyOS with clang 23). The build fetches the pinned
public PS5 Payload SDK, the ps5-opengl SDK and the PacBrew libraries into `.deps/` by itself.

## Host tools

`make doctor` checks them: bash, make, ninja, python3, clang/clang++, clang-format,
clang-tidy, llvm-ar/llvm-ranlib, wget, unzip, tar, sha256sum, git, curl, pkg-config, ffmpeg.
`ccache` is optional (`USE_CCACHE=0` turns it off).

## Build

```sh
make DEVELOPMENT=1 USE_CCACHE=0 PS5_CLANG=clang app
```

- `PS5_CLANG=clang` uses the system clang; the build otherwise looks for `clang-18`.
- The app folder is `dist/PPSA99400/` (and `dist/PPSA99400.zip`).

## macOS

Tested on Apple silicon (macOS 27) with Homebrew:

```sh
brew install llvm bash coreutils findutils gnu-sed gnu-tar wget ninja
tools/mac-make.sh DEVELOPMENT=1 USE_CCACHE=0 app -j8   # the PS5 app
bash tools/store-test.sh                                # the host checks
```

- `tools/mac-make.sh` puts Homebrew's GNU tools, bash and LLVM first on the PATH and runs
  make with Homebrew's clang (it has the `x86_64-sie-ps5` target).
- That clang has no x86-64 Linux compiler builtins, so `tools/build-builtins.sh` builds them
  once from compiler-rt 18.1.8's source (pinned by hash) into `.deps/native/compiler-rt`.
- `tools/store-test.sh` runs with Apple's own clang. It turns ASan's leak checking off
  there (Apple's ASan has none), and fuzzes with Homebrew's LLVM (Apple's clang has no
  libFuzzer).

## Home-screen art

The art uses EA's Need for Speed emblem, so it is **not** in this repository.
`tools/blackbox-assets.sh` (run by `make app`) copies it in from the private
nfs-blackbox-ps5 checkout beside this one (`NFSBB`, default `../nfs-blackbox-ps5`):

| Variable | Default | What |
| --- | --- | --- |
| `PB_ICON` | `$NFSBB/art/prospero-blackbox-icon0.png` | 512x512 PNG |
| `PB_BACKGROUND` | `$NFSBB/art/prospero-blackbox-bg.png` | 3840x2160 RGB PNG |
| `PB_ART` | `$NFSBB/art` | the games' icons (`<game>.png`) |
| `PS5_ASSETS_TOOL` | `$NFSBB/PS5_Vulkan/tools/prepare-assets.sh` | BC7 converter for `pic0.dds`/`pic1.dds` |

Without them the app builds with no Home-screen art.

## Deploy

Copy `dist/PPSA99400` into a folder ShadowMountPlus scans (for example `/data/homebrew`),
with every file `777`.

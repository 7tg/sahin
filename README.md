# Sahin

Sahin runs your own copy of the PC game *Blur* (Bizarre Creations, 2010) on a jailbroken
PS5, as a homebrew app with its own Home-screen tile (`PPSA99404`). It runs the PC version
through [prospero-win](https://github.com/mpereiraesaa/prospero-win): Wine, DXVK and the
PS5's Vulkan driver.

Türkçe kılavuz: [docs/KILAVUZ.md](docs/KILAVUZ.md).

**You need your own copy of the game. Nothing here contains it.** The repo and the app
hold only our own code, patches, configuration and art, plus open-source projects built from
source. The mods the game uses are downloaded by the console when you install, each pinned by
SHA-256. Sahin is not affiliated with Activision, Bizarre Creations or Sony.

## What the app does

- Installs Blur from your copy into its Wine prefix. Your copy is never changed.
- Mods from an earlier install in your copy (`dsound.dll`, `scripts/`, `amax/`, …) are left
  out, and the app adds its own:
  - **Ultimate ASI Loader** (ThirteenAG), as `dsound.dll`
  - **Online through Amax** (on by default): `amax-redirect.asi`, so multiplayer goes to the
    [Amax](https://amax-emu.com) community servers
  - **Amax client extras** (off by default): Amax's client, with profile pictures and its Lua
    plugins
- Starts the game straight from its tile. **Hold □ while the tile starts** to open Sahin's
  page instead. The page has the resolution (3840×2160, 2560×1440 or 1920×1080), a
  performance overlay, the two options above, Repair (△) and Play.
- In the game, the touchpad moves the mouse pointer and pressing it clicks. To quit, hold
  Options and the touchpad together for about a second.
- The DualSense rumbles when Blur asks for it, and L2 and R2 vibrate with it. The PS5's
  trigger effect intensity setting (Accessibility or Accessories, Controllers) turns the
  triggers' part down or off.
- Blur plays in the PS5's language when your copy has it. A copy's languages are listed in
  the language file in its folder (`english.txt` in an English copy: English, French and
  Italian); Sahin renames that file as the game starts. Blur has no Turkish; a language
  your copy doesn't have leaves it as it is.

## What you need

- A PS5 that runs homebrew: an FTP server and an ELF loader with elfldr on local port 9021,
  and [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) to install apps from
  `/data/homebrew`. Developed for firmware 13.60 with kstuff-lite, ShadowMount+ and Payload
  Manager.
- **Your Blur folder.** Blur's retail exe, from the disc or the old Steam release, carries the
  SecuROM copy protection, which does not run under Wine: such a copy installs, then closes as
  soon as it starts, and the page tells you so on the next start. Sahin does not remove or get
  around SecuROM and does not point to anything that does.

## On the console

1. Copy `release/PPSA99404/` to `/data/homebrew/PPSA99404/` over FTP. ShadowMount+ adds the
   tile; reboot if it doesn't appear.
2. Copy your Blur folder to `/data/sahin/base-games/Blur` over FTP, or put it on a USB drive
   in a folder named `Blur`. A USB copy is copied to the console as part of the install.
3. Open Sahin and press Install. It downloads the mods, copies your game into the prefix and
   adds them. Downloads are about 4 MB, or 7 MB with the Amax client extras.

To start from a save you have (your own, or a finished one), put Blur's `blursave` file in
`/data/sahin/saves/Blur/` before you install. A fresh install places it where Blur keeps its
save; once Blur has a save of its own, the install never replaces it.

After that, the tile starts the game.

## Building

Everything builds with one script inside a pinned container (Ubuntu 24.04 by digest, apt from
a dated snapshot), from the committed sources. The same commit gives the same bytes.

```sh
git clone --recurse-submodules <this repo> sahin
cd sahin && tools/build.sh
```

You need x86-64 Linux with Docker. The app and its zip land in `release/PPSA99404/` and
`release/PPSA99404.zip`, with `release/SHA256SUMS`. Steps can be run on their own:
`tools/build.sh game-app audit`. The `audit` step fails the build if the repo or the app holds
a Windows executable not built from source, game data or publisher media.

Checks on the build machine, without a console:

- `python3 -m pytest tests`: the plan generator, the copy scan, the art and the audit.
- `cd launcher && bash tools/store-test.sh`: the launcher's tests (clang, ASan/UBSan).
- `tools/host-install.sh`: the launcher's own installer run against a copy of your Blur folder
  (`BLUR=…`). Your folder is read only, never changed or linked to.
- `tools/host-run.sh 120`: that install started under the host's Wine with the DXVK the build
  made. `HEADLESS=1920x1080` runs it inside a headless gamescope.

## What is where

| Path | What |
| --- | --- |
| `tools/` | the build (`build.sh`), prospero-win, DXVK, the ps5-opengl SDK, the bare prefix, the bundle, the app's `SOURCES.txt` and `THIRD_PARTY.md` (`make-notices.py`), the app zip, the audit, the host checks |
| `patches/` | changes to prospero-win, PS5_Mesa, PS5_Vulkan and DXVK, from nfs-blackbox-ps5 |
| `launcher/` | the app's launcher (`eboot.bin`): copy check, install, settings page. A fork of nfs-blackbox-launcher. |
| `profile/blur/` | Blur as the install sees it: the plan generator, our config files, the known data files' sizes and hashes (numbers only; `base.json`, written by `scan-copy.py`, is reference data: the launcher uses only `cache/root.pak`'s size and CRC-32, built into `base_game.cpp`) |
| `art/` | the Home-screen art, drawn by `make-art.py` (nothing from the game) |
| `sce_sys/param.json` | a copy of the `param.json` the app ships (written by the build), so the title ID and content version can be read from the repo |
| submodules | prospero-win, PS5_Mesa, PS5_Vulkan, PS5_PayloadSDK, ps5-native-app-boilerplate and ps5-opengl, at pinned commits |

## Credits

- [prospero-win](https://github.com/mpereiraesaa/prospero-win), PS5_Mesa, PS5_Vulkan and the
  [ps5-opengl](https://github.com/mpereiraesaa/ps5-opengl) fork by mpereiraesaa; ps5-opengl,
  the native app boilerplate and the launcher's UI originally by BlackBearReloaded; the PS5
  Payload SDK.
- [nfs-blackbox-ps5](https://github.com/lavavex/nfs-blackbox-ps5) and
  [nfs-blackbox-launcher](https://github.com/lavavex/nfs-blackbox-launcher) by lavavex. Sahin
  is built from their tools, patches and launcher.
- Wine, Mesa, DXVK and LLVM: the stack underneath.
- The mods, each downloaded from its own page: the
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (ThirteenAG),
  [amax-redirect](https://github.com/Aib0t/amax-dns-asi) (Aib0t) and the Amax client (the Amax
  team).
- *Blur* is Activision's. Sahin is not affiliated with Activision, Bizarre Creations or Sony.

## License

GPL-3.0 (`LICENSE`). The patches in `patches/` follow the licences of the projects they
change, and the submodules keep their own. The app folder's `THIRD_PARTY.md` says what each
part of it is and under which licence, with the texts in `LICENSES/`, and its `SOURCES.txt`
names the exact source of each part: this repository at the commit the app was built from,
the launcher, DXVK, and prospero-win's own records of Wine, Mesa and the rest.

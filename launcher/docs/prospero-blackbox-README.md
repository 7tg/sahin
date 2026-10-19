# Prospero BlackBox

**A native PS5 manager for the EA Black Box era Need for Speed games.** Put your own clean PC
install of a game on the console, and Prospero BlackBox adds the mods and files it needs and
deploys it to the Home screen as its own title, running on
[prospero-win](https://github.com/mpereiraesaa/prospero-win).

> [!WARNING]
> **Pre-alpha, in development.** Nothing here is released yet. Version 1 will install
> **Need for Speed: Most Wanted (2005)**; the other games are listed but not available yet.

Prospero BlackBox never starts a game itself. It holds the files, installs, updates and
removes games, and owns the Wine prefix they share (so cross-game save bonuses work).

## Games

| Title ID | Game | Folder | Status |
| --- | --- | --- | --- |
| PPSA99400 | Prospero BlackBox (this app) | | in development |
| PPSA99401 | Need for Speed: Underground (2003) | `NFSU` | in progress |
| PPSA99402 | Need for Speed: Underground 2 (2004) | `NFSU2` | in progress |
| PPSA99403 | Need for Speed: Most Wanted (2005) | `NFSMW` | **version 1** |
| PPSA99404 | Need for Speed: Carbon (2006) | `NFSC` | in progress |
| PPSA99405 | Need for Speed: ProStreet (2007) | `NFSPS` | in progress |
| PPSA99406 | Need for Speed: Undercover (2008) | `NFSUC` | in progress |

## How it will work

1. Copy your clean, installed PC copy of a game to the console, into
   `/data/prospero-blackbox/base-games/<Folder>/` over FTP, or onto a USB drive in a folder
   named `<Folder>` (for example `NFSMW`). Prospero BlackBox copies a USB copy into
   `base-games/` and never changes it.
2. It checks the files against what it knows about that game and refuses an unknown version.
3. It downloads the game's mods from their original authors (nothing is bundled), installs
   the game and its mods into its Wine prefix, and applies the settings.
4. It deploys the game as its own Home-screen title.

**You need your own copy of each game.** Prospero BlackBox does not include or download any
game files.

## Requirements

- A jailbroken PS5 with an ELF loader on TCP port 9021 (etaHEN, for example) and
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) 1.7beta4 or newer.
- Space on `/data` for the base games, the prefix and the deployed titles.

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## Credits

- **Prospero BlackBox is a fork of [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore)**
  by BlackBearReloaded (GPL-3.0): the native interface, controller input, installs, the
  payload-side file worker and the elevation helper come from it.
- [prospero-win](https://github.com/mpereiraesaa/prospero-win) runs the games on the PS5.
- [PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) (one-shot
  elevation helper), the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and the
  other projects in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The mods each game uses belong to their authors; Prospero BlackBox downloads them from
  their own pages.

## License and trademarks

GPL-3.0-or-later; see [LICENSE](LICENSE). Not affiliated with or endorsed by Electronic Arts.
Need for Speed and its logos are trademarks of Electronic Arts Inc.; the app's Home-screen
art is not part of this repository.

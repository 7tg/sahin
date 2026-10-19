# NFS Black Box launcher

The source of the `eboot.bin` in each [NFS Black Box PS5](https://github.com/lavavex/nfs-blackbox-ps5)
game app: Prospero BlackBox, built as one game's own app (`make GAME=PPSA99403 app`).

Opened from its Home-screen tile, it starts the game straight away once the game is
installed. Before that it shows one page: how to put your own copy on the console, the Xbox
360 look on or off, and Install. The install copies your game (from `base-games` on the
console or a USB drive), downloads the mods pinned by the game's install plan
(`assets/plans/NFSMW.json`), builds the game in its Wine prefix and hands over to
prospero-win's runner, `game.bin`, in the same app folder. The game's own PS5 Options page
(the PS5 Options mod in the main repo) brings you back here to apply a change.

This is a one-commit export of the launcher at the commit the release was built from. The
full Prospero BlackBox manager it comes from, with its catalog and store, is described in
`docs/prospero-blackbox-README.md`.

## Building

With the main repo beside it as `nfs-blackbox-ps5` and this one as `prospero-blackbox`,
`tools/build.sh` there builds the game apps from this source (its `game-apps` step). The
quick host tests here run with `bash tools/store-test.sh`.

## License

GPL-3.0-or-later (`LICENSE`); `THIRD_PARTY_NOTICES.md` lists what is bundled and under which
licence.

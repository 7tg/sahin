#!/usr/bin/env python3
"""Makes the app's SOURCES.txt and THIRD_PARTY.md say what Sahin's app holds.

  tools/make-notices.py APP REV

prospero-win's packager writes both files for its own package (PPSA99995) and build_game_app.sh
copies them into the game's app; there they name neither this repo, nor the launcher that is
the app's eboot.bin, nor our DXVK. GPL-3 §6 and LGPL-2.1 §4 want whoever holds the binaries to
find their exact source, so this rewrites them, in place:

  SOURCES.txt     Sahin's repository and commit (REV), the launcher, DXVK (the version and commit
                  tools/build-dxvk.sh pins, with patches/dxvk), the patched trees (base commit +
                  patches/<project>), then prospero-win's own lines as they were.
  THIRD_PARTY.md  what the app's own parts are and their licences (the launcher, game.bin, DXVK,
                  ps5-opengl, the bare prefix, the art), then prospero-win's text, which in this
                  folder describes game.bin and PPSA99404 rather than eboot.bin and PPSA99995.

Running it twice gives the same files: the Sahin part is replaced, never stacked.
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO = "https://github.com/7tg/sahin"
MARK = "<!-- sahin-notices-end -->"
SOURCES_MARK = "---- prospero-win's package ----"


def dxvk_pin(script=ROOT / "tools/build-dxvk.sh"):
    """(version, commit) from the lines tools/build-dxvk.sh pins them on."""
    text = script.read_text()
    version = re.search(r"^version=([\w.-]+)$", text, re.M)
    commit = re.search(r"^commit=([0-9a-f]{40})$", text, re.M)
    if not version or not commit:
        sys.exit(f"{script}: no version=/commit= lines")
    return version.group(1), commit.group(1)


def ds2texinject_pin(readme=ROOT / "third_party/ds2texinject/README.md"):
    """The upstream commit third_party/ds2texinject was taken from."""
    m = re.search(r"commit ([0-9a-f]{40})", readme.read_text())
    if not m:
        sys.exit(f"{readme}: no commit")
    return m.group(1)


def patch_count(project):
    return len(list((ROOT / "patches" / project).rglob("*.patch")))


def pins(script=ROOT / "tools/build-prospero-win.sh"):
    """The base commits tools/build-prospero-win.sh patches: {name: sha}."""
    return dict(re.findall(r"\b([A-Z_]+_BASE)=([0-9a-f]{40})\b", script.read_text()))


def sources(old, rev, version, tid):
    """SOURCES.txt: Sahin's parts, then prospero-win's lines after SOURCES_MARK."""
    old = old.split(SOURCES_MARK + "\n", 1)[-1].lstrip("\n")
    dxvk_version, dxvk_commit = dxvk_pin()
    base = pins()
    lines = [
        f"Source for {tid} (Sahin {version}). THIRD_PARTY.md says what each part is and its licence.",
        "",
        f"Sahin  {REPO}  commit {rev}",
        f"  the repository that builds this folder (tools/build.sh), tag {version}; it holds the",
        "  launcher (eboot.bin, launcher/), the install profile, the art and the patches below",
        f"Sahin launcher (eboot.bin, store-worker.elf)  {REPO}  launcher/ at commit {rev}",
        "  a fork of https://github.com/lavavex/nfs-blackbox-launcher (Prospero BlackBox, from",
        "  BlackBearReloaded's ProsperoStore); its own notices: LICENSES/launcher/THIRD_PARTY_NOTICES.md",
        f"DXVK {dxvk_version} (d3d8, d3d9, d3d10core, d3d11, dxgi in assets/bundle/bare-prefix-sahin.zip)",
        f"  https://github.com/doitsujin/dxvk  commit {dxvk_commit} (v3.1.1)",
        f"  with patches/dxvk ({patch_count('dxvk')} patches) from the Sahin repository",
        "DS2TexInject (scripts/DS2TexInject.asi in the PlayStation button prompts' zip in assets/bundle)",
        f"  https://github.com/sidnei-almeida/dead-space-2-texmod-linux  commit {ds2texinject_pin()}",
        "  unchanged, in third_party/ds2texinject of the Sahin repository",
        "ps5-opengl (linked into eboot.bin)  https://github.com/mpereiraesaa/ps5-opengl",
        f"  the submodule commit the Sahin repository pins at commit {rev}",
        "",
        "prospero-win, PS5_Mesa and PS5_Vulkan are built from the commits below with the patches in",
        "the Sahin repository's patches/ directory (tools/build-prospero-win.sh), so their revisions",
        "named by prospero-win's packager are those patched trees:",
        f"  prospero-win  commit {base.get('PW_BASE', '?')} + patches/prospero-win ({patch_count('prospero-win')} patches)",
        f"  PS5_Mesa  commit {base.get('MESA_BASE', '?')} + patches/PS5_Mesa ({patch_count('PS5_Mesa')} patches)",
        f"  PS5_Vulkan  commit {base.get('VULKAN_BASE', '?')} + patches/PS5_Vulkan ({patch_count('PS5_Vulkan')} patches)",
        "",
        SOURCES_MARK,
        old.rstrip("\n"),
        "",
    ]
    return "\n".join(lines)


def third_party(old, rev, version, tid):
    """THIRD_PARTY.md: Sahin's parts, then prospero-win's text, read for this folder."""
    old = old.split(MARK + "\n", 1)[-1].lstrip("\n")
    pw = (old.replace("PPSA99995", tid)
             .replace("## The title: `eboot.bin`, `sce_module/libc.prx`",
                      "## The runner: `game.bin`, `sce_module/libc.prx`")
             .replace("The boilerplate's startup code is linked into\n`eboot.bin`,",
                      "The boilerplate's startup code is linked into\n`game.bin`,")
             .replace("# Third-party software in the app folder",
                      "# prospero-win's package, as it describes itself"))
    dxvk_version, _ = dxvk_pin()
    head = f"""# Third-party software in the app folder

The `{tid}` folder is Sahin {version}, built from <{REPO}> at commit
`{rev}` by its `tools/build.sh`. `SOURCES.txt` names the exact source revision
of every part, so you can get the source that matches the binaries you have.
The licence texts are in `LICENSES/`. The folder holds no game file: the
player's own copy of Blur is installed from their folder on the console.

## The launcher: `eboot.bin`, `store-worker.elf`, `assets/`

Sahin's launcher, GPL-3.0-or-later (`LICENSE` in `launcher/` of the
repository; `LICENSES/GPL-3.0.txt`). It is a fork of
[nfs-blackbox-launcher](https://github.com/lavavex/nfs-blackbox-launcher)
by lavavex (Prospero BlackBox), itself from BlackBearReloaded's ProsperoStore
and PS5 Native App Boilerplate, GPL-3.0-or-later. What it bundles and under
which licence is in `LICENSES/launcher/THIRD_PARTY_NOTICES.md`; the licences
of its network libraries (curl, OpenSSL, zlib, zstd, libpsl, the public
suffix list, qrcodegen) are in `assets/licenses/`, those of the libraries in
its source tree (miniz, monocypher, picosha2, stb, yyjson) in
`LICENSES/launcher/`. The fonts under `assets/fonts/` are Inter, Montserrat,
DejaVu Sans Mono, Patrick Hand and Press Start 2P, each with its licence
beside it. `eboot.bin` links [ps5-opengl](https://github.com/mpereiraesaa/ps5-opengl),
GPL-3.0-or-later, whose licence and notices are in `LICENSES/ps5-opengl/`.

## prospero-win: `game.bin`, `win/`, `lapy.elf`

prospero-win's title, which starts the game, with Wine, FreeType, the Vulkan
driver, Mesa WGL/Zink and the privilege helper: prospero-win's own text below
describes them, section by section.

## DXVK: `assets/bundle/bare-prefix-sahin.zip`

The bare Wine prefix the launcher unpacks carries Wine's builtin DLLs (Wine,
LGPL-2.1-or-later, below) and our build of [DXVK](https://github.com/doitsujin/dxvk)
{dxvk_version} (`d3d8.dll`, `d3d9.dll`, `d3d10core.dll`, `d3d11.dll`, `dxgi.dll` in
`system32/` and `syswow64/`), under the zlib licence (`LICENSES/dxvk/LICENSE`),
with the licences of what its DLLs build in (`LICENSES/dxvk/`: dxbc-spirv,
libdisplay-info, the Vulkan and SPIR-V headers). `SOURCES.txt` names the DXVK
commit and the patches applied.

## The PlayStation button prompts: `assets/bundle/` (the zip with `DS2TexInject.asi`)

Installed when the page's PlayStation button prompts option is on:
[DS2TexInject](https://github.com/sidnei-almeida/dead-space-2-texmod-linux)
(`scripts/DS2TexInject.asi`), MIT (`LICENSES/ds2texinject/LICENSE`), built from
the source `SOURCES.txt` names; its settings, `DS2TexInject.ini`; and the
button atlas it swaps in (`texmod/_cache/0xF6FA7B91.dds`), Sahin's own, drawn by
`art/make-prompts.py`, GPL-3.0-or-later. Nothing comes from the game.

## The art: `sce_sys/`

The icon and the Home-screen pictures are Sahin's own, drawn by `art/make-art.py`
in the repository, GPL-3.0-or-later. Nothing comes from the game.

## Mods

The app ships no mod. At install the console downloads, each pinned by SHA-256
in `assets/plans/BLUR.json`, the Ultimate ASI Loader (ThirteenAG, MIT),
amax-redirect.asi (Aib0t, MIT) and, if chosen, the Amax client files from
amax-emu.com and blur-tr's Turkish text (github.com/7tg/blur-tr: its ASI
Apache-2.0, its translation CC BY-SA 4.0), each from its own page.

{MARK}

"""
    return head + pw


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    app, rev = pathlib.Path(sys.argv[1]), sys.argv[2]
    if not re.fullmatch(r"[0-9a-f]{40}", rev):
        sys.exit(f"{rev}: not a commit hash")
    param = json.loads((app / "sce_sys/param.json").read_text())
    tid, version = param["titleId"], param["contentVersion"]
    for name, make in (("SOURCES.txt", sources), ("THIRD_PARTY.md", third_party)):
        path = app / name
        path.write_text(make(path.read_text(), rev, version, tid))
        print(f"{path}: {len(path.read_text().splitlines())} lines")


if __name__ == "__main__":
    main()

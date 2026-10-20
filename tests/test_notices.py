import json, pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/make-notices.py"
REV = "0123456789abcdef0123456789abcdef01234567"

PW_SOURCES = """Source for this package. THIRD_PARTY.md says what each part is and its licence.

prospero-win  https://github.com/mpereiraesaa/prospero-win  commit 9100ecc189c56b3600d24d51e72b0a214cdc1cf1
Wine  https://gitlab.winehq.org/wine/wine  commit 490f6d5dcbb2a5047345b8af88d114bbcaad69a8,
  with prospero-win's wine/patches (90 patches)
"""
PW_THIRD_PARTY = """# Third-party software in the app folder

The `PPSA99995` folder you install on the console is more than prospero-win.

## The title: `eboot.bin`, `sce_module/libc.prx`

The boilerplate's startup code is linked into
`eboot.bin`, and `sce_module/libc.prx` is the boilerplate's own runtime
library.

## Wine: `win/wine/`

Every module under `win/wine/lib/wine/` comes from Wine.
"""


def app(tmp_path):
    a = tmp_path / "PPSA99404"
    (a / "sce_sys").mkdir(parents=True)
    (a / "sce_sys/param.json").write_text(json.dumps({"titleId": "PPSA99404", "contentVersion": "01.002.000"}))
    (a / "SOURCES.txt").write_text(PW_SOURCES)
    (a / "THIRD_PARTY.md").write_text(PW_THIRD_PARTY)
    return a


def run(a, rev=REV):
    return subprocess.run([sys.executable, str(TOOL), str(a), rev], capture_output=True, text=True)


def dxvk_commit():
    import re
    return re.search(r"^commit=([0-9a-f]{40})$", (ROOT / "tools/build-dxvk.sh").read_text(), re.M).group(1)


def test_sources_name_sahin_the_launcher_and_dxvk(tmp_path):
    a = app(tmp_path)
    assert run(a).returncode == 0
    text = (a / "SOURCES.txt").read_text()
    assert text.startswith("Source for PPSA99404 (Sahin 01.002.000).")
    assert f"https://github.com/7tg/sahin  commit {REV}" in text
    assert "nfs-blackbox-launcher" in text
    assert dxvk_commit() in text and "patches/dxvk (2 patches)" in text
    assert "644e32e400c555e340d507383398c5d21b39b0b0 + patches/prospero-win" in text
    # prospero-win's own lines stay, after the marker, as they were
    assert text.endswith("---- prospero-win's package ----\n" + PW_SOURCES)


def test_third_party_describes_this_folder(tmp_path):
    a = app(tmp_path)
    assert run(a).returncode == 0
    text = (a / "THIRD_PARTY.md").read_text()
    assert "PPSA99995" not in text
    assert text.startswith("# Third-party software in the app folder\n\nThe `PPSA99404` folder is Sahin 01.002.000")
    assert f"commit\n`{REV}`" in text
    assert "## The launcher: `eboot.bin`" in text and "GPL-3.0-or-later" in text
    assert "## DXVK: `assets/bundle/bare-prefix-sahin.zip`" in text
    assert "## The runner: `game.bin`, `sce_module/libc.prx`" in text
    assert "linked into\n`game.bin`," in text and "linked into\n`eboot.bin`" not in text
    assert "## Wine: `win/wine/`" in text  # prospero-win's text follows
    assert text.count("# Third-party software in the app folder") == 1


def test_running_twice_gives_the_same_files(tmp_path):
    a = app(tmp_path)
    run(a)
    once = [(a / n).read_text() for n in ("SOURCES.txt", "THIRD_PARTY.md")]
    run(a)
    assert [(a / n).read_text() for n in ("SOURCES.txt", "THIRD_PARTY.md")] == once
    assert once[1].count("## The launcher") == 1


def test_rejects_a_bad_rev(tmp_path):
    a = app(tmp_path)
    assert run(a, "HEAD").returncode != 0
    assert (a / "SOURCES.txt").read_text() == PW_SOURCES

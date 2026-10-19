#!/usr/bin/env python3
"""Records a Blur copy's data files (path, size, CRC-32, SHA-256) as profile/blur/base.json.

  profile/blur/scan-copy.py GAME_FOLDER OUT.json

Only numbers are written. Executables, mods and the installer's leftovers are skipped: the
plan accepts any exe, and mods in a copy are left out of the install.
"""
import hashlib, json, pathlib, sys, zlib

SKIP_TOP = {"blur.exe", "dsound.dll", "dinput8.dll", "d3d9.dll", "lua5.1.dll",
            "discord-rpc.dll", "blur_rpc.log", "scripts", "amax", "uninstall", "redist",
            "docs", "5.ico", "firewallinstallhelper.dll"}


def digest(path):
    sha, crc = hashlib.sha256(), 0
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            sha.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return sha.hexdigest(), f"{crc & 0xffffffff:08x}"


def main(game, out):
    game = pathlib.Path(game)
    files = []
    for path in sorted(game.rglob("*")):
        rel = path.relative_to(game)
        if not path.is_file() or rel.parts[0].lower() in SKIP_TOP:
            continue
        sha, crc = digest(path)
        files.append({"path": rel.as_posix(), "size": path.stat().st_size, "crc32": crc, "sha256": sha})
    pathlib.Path(out).write_text(json.dumps({"files": files}, indent=1) + "\n")


if __name__ == "__main__":
    main(*sys.argv[1:3])

#!/usr/bin/env python3
# Prospero BlackBox - FTP-only folder install of a frozen build, verified by hash.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""FTP-only folder install of a frozen build. Never starts or closes anything.
usage: deploy.py look|install|verify HOST FROZEN_DIR
"""
import hashlib, io, sys
from ftplib import FTP, error_perm
from pathlib import Path
from posixpath import dirname, join

mode, host, frozen = sys.argv[1], sys.argv[2], Path(sys.argv[3])
TITLE = "PPSA99400"
ROOT = f"/data/homebrew/{TITLE}"


def connect():
    ftp = FTP()
    ftp.connect(host, 2121, timeout=20)
    ftp.login("anonymous", "codex")
    return ftp


def listing(ftp, path):
    # Some ftpsrv versions list the current folder when MLSD names a missing
    # path, so enter the folder first: CWD fails properly when it isn't there.
    ftp.cwd(path)
    try:
        return list(ftp.mlsd())
    finally:
        ftp.cwd("/")


def names(ftp, path):
    try:
        return sorted(n for n, _ in listing(ftp, path) if n not in {".", ".."})
    except error_perm:
        return None


def walk(ftp, path, out, prefix="", depth=0):
    try:
        entries = listing(ftp, path)
    except error_perm:
        return
    if depth == 0 and "--raw" in sys.argv:
        print(entries[:6])
    for name, facts in entries:
        if name in {".", "..", ""} or "/" in name or facts.get("type") in {"cdir", "pdir"}:
            continue
        if facts.get("type") == "dir":
            if depth < 8:
                walk(ftp, join(path, name), out, prefix + name + "/", depth + 1)
        else:
            out[prefix + name] = int(facts.get("size", -1))


def digest(ftp, remote):
    h = hashlib.sha256()
    ftp.retrbinary(f"RETR {remote}", h.update, blocksize=256 * 1024)
    return h.hexdigest()


def local_files():
    files = sorted(p for p in frozen.rglob("*") if p.is_file())
    last = [frozen / "eboot.bin", frozen / "sce_sys/param.json"]
    return [p for p in files if p not in last] + last


def running(ftp):
    sandboxes = names(ftp, "/mnt/sandbox") or []
    return [n for n in sandboxes if n.startswith("PPSA") or n.startswith("CUSA")]


def compare(ftp):
    """Hash every installed file against the frozen build. Returns mismatches."""
    bad = []
    selfmode = False
    for path in local_files():
        relative = path.relative_to(frozen).as_posix()
        want = hashlib.sha256(path.read_bytes()).hexdigest()
        try:
            got = digest(ftp, join(ROOT, relative))
            if got != want and not selfmode:
                # Signed files are served as their ELF view until SELF is sent once.
                ftp.sendcmd("SELF")
                selfmode = True
                got = digest(ftp, join(ROOT, relative))
        except error_perm as error:
            got = f"missing ({error})"
        if got != want:
            bad.append((relative, got[:16]))
    return bad


ftp = connect()
active = running(ftp)
print(f"[{host}] titles with a sandbox now: {active or 'none'}")
installed = {}
walk(ftp, ROOT, installed)
print(f"[{host}] installed files before: {len(installed)}")
copies = [n for n in (names(ftp, "/data/homebrew") or []) if TITLE in n]
print(f"[{host}] entries for {TITLE} in /data/homebrew: {copies or 'none'}")
if any(n != TITLE for n in copies):
    print(f"[{host}] another copy of the title is present (an image?): stopping")
    sys.exit(4)

if mode == "look":
    for extra in sorted(set(installed) - {p.relative_to(frozen).as_posix() for p in local_files()}):
        print(f"  only on console: {extra} ({installed[extra]} bytes)")
    print(f"[{host}] /data/nfs-blackbox: {names(ftp, '/data/nfs-blackbox')}")
    ftp.quit()
    sys.exit(0)

if mode == "install":
    if any(name.startswith(TITLE) for name in active):
        print(f"[{host}] {TITLE} is running: nothing uploaded")
        sys.exit(3)
    made = set()
    files = local_files()
    for index, path in enumerate(files, 1):
        relative = path.relative_to(frozen).as_posix()
        remote = join(ROOT, relative)
        folder = dirname(remote)
        current = ""
        for part in folder.strip("/").split("/"):
            current += "/" + part
            if current in made or not current.startswith(ROOT):
                continue
            try:
                ftp.mkd(current)
            except error_perm:
                pass
            made.add(current)
        temporary = join(folder, "." + path.name + ".upload")
        try:
            ftp.sendcmd(f"DELE {temporary}")
        except error_perm:
            pass
        with path.open("rb") as source:
            ftp.storbinary(f"STOR {temporary}", source, blocksize=256 * 1024)
        try:
            ftp.sendcmd(f"DELE {remote}")
        except error_perm:
            pass
        ftp.rename(temporary, remote)
    print(f"[{host}] uploaded {len(files)} files")

bad = compare(ftp)
after = {}
walk(ftp, ROOT, after)
wanted = {p.relative_to(frozen).as_posix() for p in local_files()}
extras = sorted(set(after) - wanted)
print(f"[{host}] verify: {len(wanted) - len(bad)}/{len(wanted)} files match by SHA-256; "
      f"extra files on console: {extras or 'none'}")
for relative, got in bad:
    print(f"  MISMATCH {relative}: {got}")
try:
    ftp.quit()
except Exception:
    pass
sys.exit(1 if bad else 0)

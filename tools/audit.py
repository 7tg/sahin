#!/usr/bin/env python3
"""Fails when a tree holds what Sahin must never ship: Windows executables we didn't build,
game data, publisher media or Microsoft redistributables.

  tools/audit.py ROOT [ROOT...]

ROOT/.audit-allow lists globs (relative to ROOT; blank lines and # comments ignored) of files built from source that may be PE
files (Wine's DLLs, DXVK). Every *.zip is audited member by member too (findings read ZIP!MEMBER);
a line ZIPGLOB!MEMBERGLOB allows the members matching MEMBERGLOB in the zips matching ZIPGLOB,
e.g. assets/bundle/*.zip!drive_c/windows/system32/* (fnmatch globs: * crosses folders). A plain
line never covers a zip's members. Exit 0 clean, 1 with one line per finding.
"""
import fnmatch, pathlib, sys, zipfile

BANNED_NAMES = ["blur.exe", "speed.exe", "binkw32.dll", "d3dx9_*.dll", "d3dcompiler_*.dll",
                "msvcr*.dll", "msvcp*.dll", "xinput1_*.dll", "physx*.dll", "*.securom"]
BANNED_EXT = {".bik", ".bk2", ".at9", ".pak", ".sig", ".level", ".levelphysics",
              ".levelrender", ".levelstream", ".asi", ".cab"}
SKIP_DIRS = {".git", "prospero-win", "PS5_Mesa", "PS5_Vulkan", "PS5_PayloadSDK",
             "ps5-native-app-boilerplate", "ps5-opengl", ".deps", "build", "node_modules"}


def head(path):
    try:
        with open(path, "rb") as f:
            return f.read(2)
    except OSError:
        return b""


def check(rel, name, first):
    """The finding for one file (name lowercased, first its first bytes), or None."""
    if any(fnmatch.fnmatch(name, glob) for glob in BANNED_NAMES):
        return f"{rel}: banned file name"
    if pathlib.PurePath(name).suffix in BANNED_EXT:
        return f"{rel}: game data or media type"
    if first[:2] == b"MZ":  # any MZ file: DOS stubs and truncated PEs are executables too
        return f"{rel}: Windows executable not in .audit-allow"
    return None


def audit_zip(path, rel, member_allow):
    """Findings for the members of one zip; member_allow holds the MEMBERGLOBs for it."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        return [f"{rel}: unreadable zip"]
    findings = []
    with z:
        for info in z.infolist():
            if info.is_dir() or any(fnmatch.fnmatch(info.filename, g) for g in member_allow):
                continue
            with z.open(info) as f:
                first = f.read(2)
            finding = check(f"{rel}!{info.filename}", info.filename.rsplit("/", 1)[-1].lower(), first)
            if finding:
                findings.append(finding)
    return findings


def audit(root):
    root = pathlib.Path(root)
    allow_file = root / ".audit-allow"
    allow = []
    if allow_file.exists():
        allow = [line.strip() for line in allow_file.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
    zip_allow = [line.split("!", 1) for line in allow if "!" in line]
    allow = [line for line in allow if "!" not in line]
    findings = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts) or not path.is_file():
            continue
        name = path.name.lower()
        if path.suffix.lower() == ".zip":
            findings += audit_zip(path, rel, [m for z, m in zip_allow if fnmatch.fnmatch(str(rel), z)])
        if any(fnmatch.fnmatch(str(rel), glob) for glob in allow):
            continue
        finding = check(rel, name, head(path))
        if finding:
            findings.append(finding)
    return findings


def main():
    findings = [f for root in sys.argv[1:] or ["."] for f in audit(root)]
    for line in findings:
        print(line)
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Fails when a tree holds what Sahin must never ship: Windows executables we didn't build,
game data, publisher media or Microsoft redistributables.

  tools/audit.py ROOT [ROOT...]

ROOT/.audit-allow lists globs (relative to ROOT; blank lines and # comments ignored) of files built from source that may be PE
files (Wine's DLLs, DXVK). Exit 0 clean, 1 with one line per finding.
"""
import fnmatch, pathlib, sys

BANNED_NAMES = ["blur.exe", "speed.exe", "binkw32.dll", "d3dx9_*.dll", "d3dcompiler_*.dll",
                "msvcr*.dll", "msvcp*.dll", "xinput1_*.dll", "physx*.dll", "*.securom"]
BANNED_EXT = {".bik", ".bk2", ".at9", ".pak", ".sig", ".level", ".levelphysics",
              ".levelrender", ".levelstream", ".asi", ".cab"}
SKIP_DIRS = {".git", "prospero-win", "PS5_Mesa", "PS5_Vulkan", "PS5_PayloadSDK",
             "ps5-native-app-boilerplate", ".deps", "build", "node_modules"}


def is_pe(path):
    """Any MZ file counts: DOS stubs and truncated PEs are executables too."""
    try:
        with open(path, "rb") as f:
            return f.read(2) == b"MZ"
    except OSError:
        return False


def audit(root):
    root = pathlib.Path(root)
    allow_file = root / ".audit-allow"
    allow = []
    if allow_file.exists():
        allow = [line.strip() for line in allow_file.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
    findings = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts) or not path.is_file():
            continue
        name = path.name.lower()
        if any(fnmatch.fnmatch(str(rel), glob) for glob in allow):
            continue
        if any(fnmatch.fnmatch(name, glob) for glob in BANNED_NAMES):
            findings.append(f"{rel}: banned file name")
        elif path.suffix.lower() in BANNED_EXT:
            findings.append(f"{rel}: game data or media type")
        elif is_pe(path):
            findings.append(f"{rel}: Windows executable not in .audit-allow")
    return findings


def main():
    findings = [f for root in sys.argv[1:] or ["."] for f in audit(root)]
    for line in findings:
        print(line)
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()

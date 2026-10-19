#!/usr/bin/env python3
"""Make the bare Wine prefix Prospero BlackBox unpacks on the console, as a zip.

    tools/make-bare-prefix.py OUT.zip

Uses the trees tools/build-prospero-win.sh built (host Wine, wowprospero.dll,
wow64native.dll) and tools/build-dxvk.sh built (our DXVK, 3.1.1 with patches/dxvk/).
First prospero-win's pw_install.py creates the prefix from profiles/nfsmw/bare-prefix.yml
(an empty WoW64 prefix) with that DXVK, in .deps/bare-lib. Then it is packed the way tools/pw_prefix.py pushes a prefix to the
console, with pw_prefix's own rules:
- links inside the prefix become .pw-symlinks tables (a title can't make links);
- links out of it become files or empty folders;
- system.reg's WoW64 x86 CPU is wowprospero.dll;
- wowprospero.dll and wow64native.dll go in system32;
- the console user's folders exist.

The zip holds the prefix's contents (no top folder), sorted, with fixed timestamps, so
the same inputs give the same bytes.
"""
import hashlib
import os
import re
import resource
import shutil
import subprocess
import sys
import uuid
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PW = ROOT / "prospero-win"
sys.path.insert(0, str(PW / "tools"))
import pw_prefix  # noqa: E402

STAMP = (2026, 1, 1, 0, 0, 0)
# Registry keys carry the time Wine wrote them ("[key] <unix time>", "#time=<FILETIME>"):
# all set to STAMP's moment, so two runs give the same files.
UNIX_TIME = 1767225600
FILETIME = (UNIX_TIME + 11644473600) * 10_000_000
HOST = b"prospero"
LUID = b"00,04,00,00,00,00,00,00"  # the display adapter's (0x400)
DXVK_VERSION = "3.1.1-nfsbb"
DXVK = ROOT / f".deps/dxvk-{DXVK_VERSION}.tar.gz"
EXTRA = {"drive_c/windows/system32/wowprospero.dll":
         PW / ".deps/wine-host/build/dlls/wowprospero/x86_64-windows/wowprospero.dll",
         "drive_c/windows/system32/wow64native.dll": PW / ".deps/wow64native/x86_64-windows/wow64native.dll"}


def fixed_ids(data: bytes, key: str = "") -> bytes:
    """The values wineboot makes up at random, fixed: every console unpacking this prefix
    shares them anyway, as with any prefix copied from a PC."""
    for name in (b"MachineGuid", b"MachineId", b"VideoID"):
        value = str(uuid.uuid5(uuid.NAMESPACE_URL, "nfs-blackbox-ps5/bare-prefix/" + name.decode()))
        data = re.sub(rb'^("' + name + rb'"=")(\{?)[0-9a-fA-F-]{36}(\}?")',
                      lambda m: m.group(1) + m.group(2) + value.encode() + m.group(3), data, flags=re.M)
    # A HID device and its compatible device share a ContainerId; its random part is zeroed,
    # keeping the device number that tells them apart.
    data = re.sub(rb'("ContainerId"="\{[0-9A-F]{4}845E-0000-FFFF-)[0-9A-F]{4}-[0-9A-F]{12}(\}")',
                  rb"\g<1>0000-000000000000\g<2>", data)
    # Device install times (DEVPROPTYPE FILETIME), as the keys' time.
    stamp = ",".join(f"{b:02x}" for b in FILETIME.to_bytes(8, "little")).encode()
    data = re.sub(rb"^@=hex\(ffff0010\):[0-9a-f,]{23}$", b"@=hex(ffff0010):" + stamp, data, flags=re.M)
    # The display driver's date is the day wineboot ran: STAMP's day.
    data = re.sub(rb'^"DriverDate"="[0-9-]+"$', b'"DriverDate"="%02d-%02d-%04d"' % (STAMP[1], STAMP[2], STAMP[0]),
                  data, flags=re.M)
    data = re.sub(rb'^"DriverDateData"=hex:[0-9a-f,]{23}$', b'"DriverDateData"=hex:' + stamp, data, flags=re.M)
    # The build machine's name; the console has none of its own (it used to carry EVA-01's).
    for name, value in ((b"ComputerName", HOST.upper()), (b"USERDOMAIN", HOST.upper()),
                        (b"LOGONSERVER", b"\\\\\\\\" + HOST.upper()), (b"Hostname", HOST), (b"Domain", b"")):
        data = re.sub(rb'^("' + name + rb'"=")[^"\n]*(")$', lambda m: m.group(1) + value + m.group(2), data, flags=re.M)
    # The build machine's CPU, which wineboot writes into the environment: the console's
    # instead (the PS5's 8 Zen 2 cores, as the same silicon reports itself: 4700S).
    for name, value in ((b"NUMBER_OF_PROCESSORS", b"8"),
                        (b"PROCESSOR_IDENTIFIER", b"AMD64 Family 23 Model 71 Stepping 0, AuthenticAMD"),
                        (b"PROCESSOR_LEVEL", b"23"), (b"PROCESSOR_REVISION", b"4700")):
        data = re.sub(rb'^("' + name + rb'"=")[^"\n]*(")$', lambda m: m.group(1) + value + m.group(2), data, flags=re.M)
    # The display adapter's LUID (DEVPKEY_GPU_LUID and the monitor's reference to it), handed
    # out at run time in an order that varies: one fixed value for both.
    if key == "system.reg":
        for prop, kind in ((rb"\{60B193CB-5276-4D0F-96FC-F173ABAD3EC6\}\\\\0002", rb"ffff0009"),
                           (rb"\{CA085853-16CE-48AA-B114-DE9C72334223\}\\\\0001", rb"ffff0008")):
            data, count = re.subn(rb"(" + prop + rb"\] \d+\n(?:#time=[0-9a-f]+\n)?@=hex\(" + kind + rb"\):)[0-9a-f,]{23}",
                                  rb"\g<1>" + LUID, data)
            assert count == 1, f"system.reg: {prop!r} is not where it was"
    return data


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n")[2].strip())
    out = Path(sys.argv[1]).resolve()
    wine = PW / ".deps/wine-host/install/usr/bin/wine"
    for needed in [wine, *EXTRA.values()]:
        if not needed.is_file():
            sys.exit(f"missing {needed}: run tools/build-prospero-win.sh first")
    if not DXVK.is_file():
        sys.exit(f"missing {DXVK}: run tools/build-dxvk.sh first")
    library = ROOT / ".deps/bare-lib"
    prefix = library / "prefixes/nfs-blackbox"
    if library.exists():
        shutil.rmtree(library)  # always from scratch
    # The recipe with our DXVK (pw_install's local-archive keys) under script.wine.
    recipe = (ROOT / "profiles/nfsmw/bare-prefix.yml").read_text()
    digest = hashlib.sha256(DXVK.read_bytes()).hexdigest()
    marker = "\n  wine:\n"
    assert recipe.count(marker) == 1, "bare-prefix.yml: no script.wine section"
    recipe = recipe.replace(marker, marker + f"    dxvk_version: {DXVK_VERSION}\n"
                            f"    dxvk_archive: {DXVK}\n    dxvk_sha256: {digest}\n")
    library.mkdir(parents=True)
    (library / "bare-prefix.yml").write_text(recipe)
    python = PW / ".deps/mesa-python/bin/python"  # has PyYAML, pinned
    subprocess.run([str(python if python.exists() else sys.executable), str(PW / "tools/pw_install.py"),
                    str(library / "bare-prefix.yml"), "--library", str(library), "--wine", str(wine)],
                   check=True, preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    # A Wine process that crashed while the prefix was made leaves it half done; the 32-bit
    # side (syswow64) is the first to go (Rosetta in a Linux VM runs no 32-bit code).
    wow = prefix / "drive_c/windows/syswow64"
    crashed = [p for p in prefix.rglob("core*") if p.is_file() and p.name.split(".")[0] == "core"]
    if crashed or not (wow / "kernel32.dll").is_file() or sum(1 for _ in wow.iterdir()) < 500:
        sys.exit(f"the prefix is incomplete (syswow64: {sum(1 for _ in wow.iterdir()) if wow.is_dir() else 0}"
                 f" files; crash dumps: {[str(p.relative_to(prefix)) for p in crashed]}): it needs a"
                 " machine that runs 32-bit x86 code (tools/build.sh: the prefix VM)")
    files, dirs, links = pw_prefix.local_tree(prefix)
    for key, path in EXTRA.items():
        files[key] = path
    users = "drive_c/users"
    for directory in pw_prefix.CONSOLE_USER_DIRS:
        parts = f"{users}/{pw_prefix.CONSOLE_USER}/{directory}".split("/")
        dirs.update("/".join(parts[:i]) for i in range(1, len(parts) + 1))
    entries = {}
    for key, path in files.items():
        data = path.read_bytes()
        if key in pw_prefix.REGISTRY:
            data = pw_prefix.to_console(key, data)
            data = re.sub(rb"^(\[.*\]) \d+$", rb"\1 " + str(UNIX_TIME).encode(), data, flags=re.M)
            data = re.sub(rb"^#time=[0-9a-f]+$", b"#time=" + format(FILETIME, "x").encode(), data, flags=re.M)
            data = fixed_ids(data, key)
        if key == ".update-timestamp":  # wine.inf's time when wineboot ran (only wineboot reads it)
            data = f"{UNIX_TIME}\r\n".encode()
        entries[key] = data
    for directory, table in links.items():
        key = f"{directory}/{pw_prefix.LINK_TABLE}" if directory else pw_prefix.LINK_TABLE
        entries[key] = "".join(f"{name}\t{target}\n" for name, target in sorted(table.items())).encode()
    tmp = out.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for directory in sorted(d for d in dirs if d):
            info = zipfile.ZipInfo(directory + "/", STAMP)
            info.external_attr = (0o40755 << 16) | 0x10
            zf.writestr(info, b"")
        for key in sorted(entries):
            info = zipfile.ZipInfo(key, STAMP)
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, entries[key], compresslevel=6)
    os.replace(tmp, out)
    total = sum(len(v) for v in entries.values())
    cpu = b'@="wowprospero.dll"' in entries["system.reg"]
    print(f"{out}: {len(entries)} files ({total} bytes), {len(dirs) - 1} folders, "
          f"{sum(len(t) for t in links.values())} links in tables; WoW64 CPU wowprospero.dll: {cpu}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

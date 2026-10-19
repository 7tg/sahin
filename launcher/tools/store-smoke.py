#!/usr/bin/env python3
# Prospero BlackBox - One locked, bounded, exact-title console startup case.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import threading
import time
import uuid
import sys


def complete_run(receipt, token):
    markers = [f"[STORE] run start token={token}",
               "[STORE] interactive width=3840 height=2160", "[STORE] first-swap ok",
               f"[STORE] remote clean exit token={token}", "[STORE] teardown complete",
               f"[STORE] run end token={token}"]
    start, end = markers[0].encode(), markers[-1].encode()
    if receipt.count(start) != 1 or receipt.count(end) != 1:
        return False
    position = -1
    for marker in markers:
        position = receipt.find(marker.encode(), position + 1)
        if position < 0:
            return False
    return True


def artwork_ready(receipt):
    return any(int(count) >= 5 for count in re.findall(
        rb"\[STORE\] frames=600 [^\r\n]* icons=(\d+)(?:\r?\n|$)", receipt))


def launch_accepted(output, title):
    results = re.findall(rb"\[launch\] title=" + title.encode() +
                         rb" result=0x([0-9a-fA-F]{8})(?:\r?\n|$)", output)
    return len(results) == 1 and int(results[0], 16) < 0x80000000


if sys.argv[1:] == ["--self-test"]:
    valid = (b"[STORE] run start token=test\n"
             b"[STORE] interactive width=3840 height=2160\n[STORE] first-swap ok\n"
             b"[STORE] remote clean exit token=test\n[STORE] teardown complete\n"
             b"[STORE] run end token=test\n")
    assert complete_run(valid, "test")
    assert not complete_run(valid, "other")
    assert not complete_run(valid + valid, "test")
    assert not complete_run(valid.replace(b"remote clean exit", b"shell close"), "test")
    assert not complete_run(valid.replace(b"teardown complete", b"interrupted"), "test")
    assert not complete_run(b"[STORE] teardown complete\n" + valid.replace(
        b"[STORE] teardown complete\n", b""), "test")
    assert artwork_ready(b"[STORE] frames=600 mean=16.68ms icons=10\n")
    assert not artwork_ready(b"[STORE] frames=600 mean=16.68ms icons=0\n")
    assert not artwork_ready(b"icons=10\n")
    assert launch_accepted(b"[launch] title=PPSA99400 result=0x60000001\n", "PPSA99400")
    assert not launch_accepted(b"[launch] title=PPSA99400 result=0x80940010\n", "PPSA99400")
    assert not launch_accepted(b"", "PPSA99400")
    assert not launch_accepted(b"[launch] title=PPSA99001 result=0x00000000\n", "PPSA99400")
    raise SystemExit(0)

parser = argparse.ArgumentParser(__doc__)
parser.add_argument("--host", required=True)
parser.add_argument("--lock", type=Path, required=True)
parser.add_argument("--protocol", type=Path, required=True)
parser.add_argument("--ui-tools", type=Path, required=True)
parser.add_argument("--results", type=Path, required=True)
parser.add_argument("--close-prior", help="Exact previously identified title to close before the case")
parser.add_argument("--require-catalog", action="store_true")
parser.add_argument("--require-storage", action="store_true")
parser.add_argument("--require-icons", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("console_tour", args.ui_tools / "console-tour.py")
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)
title = json.loads((root / "sce_sys/param.json").read_text())["titleId"]
assert title == "PPSA99400"
if subprocess.check_output(["git", "status", "--porcelain"], cwd=root):
    raise SystemExit("Commit the exact candidate before hardware testing")
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
package = root / "dist" / title
files = sorted(path for path in package.rglob("*") if path.is_file())
if not files or not (package / "eboot.bin").is_file():
    raise SystemExit("Build the development candidate first")
args.results.mkdir(parents=True, exist_ok=False)
manifest = {str(path.relative_to(package)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
(args.results / "candidate.json").write_text(json.dumps({
    "commit": commit, "files": manifest,
    "package_sha256": hashlib.sha256((root / "dist" / (title + ".zip")).read_bytes()).hexdigest(),
    "require_catalog": args.require_catalog,
    "require_storage": args.require_storage,
    "require_icons": args.require_icons,
    "transport_sha256": hashlib.sha256((args.ui_tools / "console-tour.py").read_bytes()).hexdigest()
}, indent=2))
token = "prospero-blackbox-" + uuid.uuid4().hex
while True:
    try:
        with args.lock.open("x") as lock:
            lock.write(token)
        break
    except FileExistsError:
        print("Console lock is held; waiting", flush=True)
        time.sleep(15)
done = threading.Event()
logger = None
console = None
result = {"classification": "no-run", "title": title, "commit": commit}
remote = "/data/homebrew/" + title
state_root = "/data/sahin"
log_root = state_root + "/logs"
try:
    if not all(transport.port_open(args.host, port) for port in (2121, 3232, 9021)):
        raise RuntimeError("Required console services are unavailable")
    console = transport.Console(args.host, 2121, raw_self=True)
    names = console.names("/mnt/sandbox")
    if names is None:
        raise RuntimeError("Cannot establish an idle console")
    active = [name for name in names if name.startswith("PPSA")]
    if args.close_prior and active == [args.close_prior + "_000"]:
        prior = subprocess.run(
            ["bash", str(args.protocol / "scripts/send-controller.sh"), "close", args.close_prior, args.host, "9021"],
            env={**os.environ, "PS5_PAYLOAD_SDK": str(root / ".deps/native/ps5-payload-sdk")},
            capture_output=True, timeout=60)
        (args.results / "close-prior.log").write_bytes(prior.stdout + prior.stderr)
        if prior.returncode:
            raise RuntimeError("Exact prior title could not be closed")
        for _ in range(12):
            time.sleep(5)
            names = console.names("/mnt/sandbox")
            if names is None:
                raise RuntimeError("Console health became uncertain during preflight")
            active = [name for name in names if name.startswith("PPSA")]
            if not active:
                break
    if active:
        raise RuntimeError("Console has a running title: " + ", ".join(active))
    logger = threading.Thread(target=transport.record_klog,
                              args=(args.host, 3232, args.results / "klog.txt", done), daemon=True)
    logger.start()
    # Metadata and executable last; every uploaded file is read back by the shared helper.
    files.sort(key=lambda p: (p.name in ("eboot.bin", "param.json"), str(p)))
    for path in files:
        target = remote + "/" + path.relative_to(package).as_posix()
        data = path.read_bytes()
        if console.read(target) != data:
            console.write(target, data)
    deadline = time.monotonic() + 120
    while "mount.lnk" not in (console.names("/user/app/" + title) or []):
        if time.monotonic() >= deadline:
            raise RuntimeError("Title registration did not become ready")
        time.sleep(5)
    time.sleep(15)
    for relative, digest in manifest.items():
        data = console.read(remote + "/" + relative)
        if data is None or hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError("Remote verification failed: " + relative)
    console.write(state_root + "/dev/run.txt", token.encode())
    console.close()
    console = None
    result["classification"] = "inconclusive"
    launched = subprocess.run(
        ["bash", str(args.protocol / "scripts/send-controller.sh"), "launch", title, args.host, "9021"],
        env={**os.environ, "PS5_PAYLOAD_SDK": str(root / ".deps/native/ps5-payload-sdk")},
        capture_output=True, timeout=60)
    (args.results / "launch.log").write_bytes(launched.stdout + launched.stderr)
    if launched.returncode or not launch_accepted(launched.stdout + launched.stderr, title):
        raise RuntimeError("Exact-title launch was rejected or not acknowledged; inspect launch.log")
    print("Candidate verified; launch request sent, observing 60 seconds", flush=True)
    log = b""
    for _ in range(12):
        time.sleep(5)
        console = transport.Console(args.host, 2121, raw_self=True)
        log = console.read(log_root + "/app.log") or log
        (args.results / "app.log").write_bytes(log)
        crash = console.read(log_root + "/crash-latest.txt")
        names = console.names("/mnt/sandbox")
        if names is None or not any(name.startswith(title + "_") for name in names):
            raise RuntimeError("Candidate is not running after the launch request; inspect evidence")
        if crash:
            (args.results / "crash.txt").write_bytes(crash)
        console.close()
        console = None
        if crash:
            result["classification"] = "failed"
            raise RuntimeError("App crash detected; do not retry")
    console = transport.Console(args.host, 2121, raw_self=True)
    console.write(state_root + "/dev/request.txt", f"quit - {token}\n".encode())
    closed = False
    for _ in range(12):
        time.sleep(5)
        current = console.read(log_root + "/app.log")
        if current:
            log = current
            (args.results / "app.log").write_bytes(log)
        names = console.names("/mnt/sandbox")
        if names is not None and not any(name.startswith(title + "_") for name in names):
            closed = True
            break
    lifecycle = console.read("/data/shadowmount/debug.log") or b""
    (args.results / "shadowmount.log").write_text("\n".join(
        line for line in lifecycle.decode(errors="replace").splitlines() if title in line))
    if closed:
        console.ftp.sendcmd("DELE " + state_root + "/dev/request.txt")
        console.ftp.sendcmd("DELE " + state_root + "/dev/run.txt")
    healthy = all(transport.port_open(args.host, port) for port in (2121, 3232, 9021))
    result.update(closed=closed, healthy=healthy)
    done.set()
    logger.join(timeout=5)
    klog = (args.results / "klog.txt").read_bytes()
    passed = closed and healthy and complete_run(klog, token)
    if passed:
        current_run = klog.split(f"[STORE] run start token={token}".encode(), 1)[1]
        current_run = current_run.split(f"[STORE] run end token={token}".encode(), 1)[0]
        if args.require_catalog:
            passed = b"[STORE] catalog verified=1 online=1" in current_run
        if args.require_storage:
            passed = passed and any(
                line.startswith(b"[STORE] storage path=/data/homebrew ") and
                b" rename=1 work-safe=1 error=" in line and line.endswith(b"error=")
                for line in current_run.splitlines())
        if args.require_icons:
            passed = passed and artwork_ready(current_run)
    result["classification"] = "pass" if passed else "failed"
    if not passed:
        raise RuntimeError("Startup or teardown criterion failed; inspect saved evidence")
finally:
    if result["classification"] != "no-run" and "healthy" not in result:
        try:
            if console is None:
                console = transport.Console(args.host, 2121, raw_self=True)
            names = console.names("/mnt/sandbox")
            result["closed"] = names is not None and not any(
                name.startswith(title + "_") for name in names)
            lifecycle = console.read("/data/shadowmount/debug.log") or b""
            (args.results / "shadowmount.log").write_text("\n".join(
                line for line in lifecycle.decode(errors="replace").splitlines() if title in line))
            result["healthy"] = all(transport.port_open(args.host, port) for port in (2121, 3232, 9021))
        except Exception as health_error:
            result["healthy"] = False
            result["health_error"] = str(health_error)
    if console:
        console.close()
    done.set()
    if logger:
        logger.join(timeout=5)
    (args.results / "result.json").write_text(json.dumps(result, indent=2))
    if args.lock.read_text() == token:
        args.lock.unlink()
print(json.dumps(result), flush=True)

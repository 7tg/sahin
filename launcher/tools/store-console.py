#!/usr/bin/env python3
# Prospero BlackBox - One scripted session on a console: launch, requests, clean quit, logs.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""One scripted Prospero BlackBox session on a console: verify the installed build,
launch, send requests, ask the app to quit, collect the log. Never kills.

usage: store-console.py HOST FROZEN_DIR RESULTS_DIR STEP...
  PS5_LOCK       a lock file shared with other sessions (taken before, released after)
  PS5_PROTOCOL   the ps5-homebrew-dev-protocol checkout (default: beside the home folder)
  requests the development build understands: open, install, uninstall, cancel,
  tour <seconds>, texbench, stress <apps>, pool <textures>, quit
  STEP is  verb:argument:seconds   (wait that long after the app took it)
       or  verb:argument:/regex/:timeout   (wait for a log line)
       or  wait::seconds
"""
import io, os, re, socket, subprocess, sys, threading, time, uuid
from ftplib import FTP, error_perm
from pathlib import Path

host, frozen, results = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
steps = sys.argv[4:]
TITLE = "PPSA99400"
STATE = "/data/nfs-blackbox"
LOCK = Path(os.environ["PS5_LOCK"]) if os.environ.get("PS5_LOCK") else None
NAME = "Prospero BlackBox-run"
HOME = Path.home()
TOOLS = Path(__file__).resolve().parent
PROTOCOL = Path(os.environ.get("PS5_PROTOCOL", HOME / "ps5-homebrew-dev-protocol"))
results.mkdir(parents=True, exist_ok=True)


def say(text):
    print(text, flush=True)


def port(number):
    try:
        socket.create_connection((host, number), timeout=4).close()
        return True
    except OSError:
        return False


def ftp():
    f = FTP()
    f.connect(host, 2121, timeout=20)
    f.login("anonymous", "codex")
    return f


def names(f, path):
    try:
        f.cwd(path)
    except error_perm:
        return None
    try:
        return [n for n, _ in f.mlsd() if n not in (".", "..")]
    finally:
        f.cwd("/")


def read(f, path):
    data = io.BytesIO()
    try:
        f.retrbinary("RETR " + path, data.write)
    except error_perm:
        return None
    return data.getvalue()


def write(f, path, data):
    f.storbinary("STOR " + path, io.BytesIO(data))


def running(f):
    # NPXS titles are the console's own services and are always there.
    return [n for n in (names(f, "/mnt/sandbox") or []) if re.match(r"(PPSA|CUSA)\d{5}_", n)]


def log(f):
    return (read(f, STATE + "/logs/app.log") or b"").decode(errors="replace")


locked = False
if LOCK:
    waited = 0
    while True:
        try:
            with LOCK.open("x") as handle:
                handle.write(NAME)
            locked = True
            break
        except FileExistsError:
            if waited >= 600:
                say("lock held by: " + LOCK.read_text() + " : giving up")
                sys.exit(5)
            time.sleep(15)
            waited += 15
    say(f"lock taken after {waited}s")

done = threading.Event()


def klog():
    try:
        with socket.create_connection((host, 3232), timeout=5) as s, (results / "klog.txt").open("wb") as out:
            s.settimeout(1)
            while not done.is_set():
                try:
                    data = s.recv(65536)
                    if not data:
                        break
                    out.write(data)
                    out.flush()
                except socket.timeout:
                    pass
    except OSError as error:
        (results / "klog-error.txt").write_text(str(error))


code = 1
try:
    if not all(port(p) for p in (2121, 3232, 9021)):
        say("console services are not all up: stopping")
        sys.exit(6)
    f = ftp()
    active = running(f)
    if active and not os.environ.get("PS5_ALLOW_RUNNING"):
        say(f"a title is running ({active}): stopping")
        sys.exit(7)
    if active:
        say(f"running before the launch: {active}")
    f.quit()
    check = subprocess.run(["python3", str(TOOLS / "store-deploy.py"), "verify", host, str(frozen)],
                           capture_output=True, text=True)
    if os.environ.get("PS5_SKIP_DEPLOY"):
        say("using the build that is on the console")
    elif check.returncode != 0:
        say("installing the build")
        put = subprocess.run(["python3", str(TOOLS / "store-deploy.py"), "install", host, str(frozen)],
                             capture_output=True, text=True)
        say(put.stdout.strip().splitlines()[-1] if put.stdout.strip() else put.stderr[-300:])
        if put.returncode != 0:
            sys.exit(8)
        # A new folder needs ShadowMountPlus to register it before it can start.
        f = ftp()
        deadline = time.time() + 150
        while "mount.lnk" not in (names(f, "/user/app/" + TITLE) or []) and time.time() < deadline:
            time.sleep(5)
        say("registered: " + str("mount.lnk" in (names(f, "/user/app/" + TITLE) or [])))
        f.quit()
        time.sleep(20)
    else:
        say("installed build matches")
    f = ftp()
    token = uuid.uuid4().hex[:12]
    for folder in (STATE, STATE + "/dev"):
        try:
            f.mkd(folder)
        except error_perm:
            pass
    write(f, STATE + "/dev/run.txt", token.encode())
    try:
        f.sendcmd("DELE " + STATE + "/dev/request.txt")
    except error_perm:
        pass
    f.quit()
    recorder = threading.Thread(target=klog, daemon=True)
    recorder.start()
    time.sleep(1)
    launched = subprocess.run(
        ["bash", str(PROTOCOL / "scripts/send-controller.sh"), "launch", TITLE, host, "9021"],
        env={**os.environ, "PS5_PAYLOAD_SDK": str(TOOLS.parent / ".deps/native/ps5-payload-sdk")},
        capture_output=True, timeout=90)
    (results / "launch.log").write_bytes(launched.stdout + launched.stderr)
    say("launch sent rc=%d %s" % (launched.returncode, (launched.stdout + launched.stderr)[-120:].decode(errors="replace").strip()))
    f = ftp()
    started = False
    for _ in range(30):
        time.sleep(3)
        text = log(f)
        if f"run start token={token}" in text and "first-swap ok" in text:
            started = True
            break
    if not started:
        say("the app did not reach its first frame: stopping (nothing is killed)")
        (results / "app.log").write_text(log(f))
        say("sandboxes: " + str(running(f)))
        sys.exit(9)
    say("app is up")

    def request(verb, argument):
        tag = uuid.uuid4().hex[:10]
        write(f, STATE + "/dev/request.txt", f"{verb} {argument or '-'} {tag}\n".encode())
        for _ in range(40):
            time.sleep(0.5)
            if f"token={tag}" in log(f):
                return True
        return False

    for step in steps:
        parts = step.split(":")
        verb, argument = parts[0], parts[1]
        # The app keeps its log in memory while a job runs, so the line that
        # acknowledges a request can arrive together with the job's result.
        watched = len(parts) >= 4 and parts[2].startswith("/")
        mark = len(log(f))
        if verb != "wait" and not request(verb, argument) and not watched:
            say(f"request not taken: {step}")
            break
        if watched:
            pattern, limit = re.compile(parts[2].strip("/")), float(parts[3])
            end = time.time() + limit
            hit = None
            while time.time() < end and not hit:
                time.sleep(2)
                hit = pattern.search(log(f)[max(0, mark - 200):])
            say(f"{step}: {'seen: ' + hit.group(0)[:150] if hit else 'NOT seen'}")
        else:
            time.sleep(float(parts[2]))
            say(f"{step}: done")
        if not any(n.startswith(TITLE) for n in running(f)):
            say("the app is no longer running: stopping the script")
            break
    if any(n.startswith(TITLE) for n in running(f)):
        write(f, STATE + "/dev/request.txt", f"quit - {token}\n".encode())
    say("titles with a sandbox before the quit: " + str(running(f)))
    closed = False
    for _ in range(24):
        time.sleep(5)
        if not any(n.startswith(TITLE) for n in running(f)):
            closed = True
            break
    text = log(f)
    (results / "app.log").write_text(text)
    crash = read(f, STATE + "/logs/crash-latest.txt")
    if crash:
        (results / "crash.txt").write_bytes(crash)
    for leftover in ("/dev/request.txt", "/dev/run.txt"):
        try:
            f.sendcmd("DELE " + STATE + leftover)
        except error_perm:
            pass
    f.quit()
    time.sleep(3)
    healthy = all(port(p) for p in (2121, 3232, 9021))
    say(f"closed={closed} healthy={healthy} crash={'yes' if crash else 'no'} teardown={'teardown complete' in text}")
    code = 0 if closed and healthy and not crash else 2
finally:
    done.set()
    time.sleep(1.5)
    if locked and LOCK and LOCK.exists() and LOCK.read_text() == NAME:
        LOCK.unlink()
        say("lock released")
sys.exit(code)

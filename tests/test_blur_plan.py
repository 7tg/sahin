import json, subprocess, sys, pathlib, re
ROOT = pathlib.Path(__file__).resolve().parents[1]
GEN = ROOT / "profile/blur/make-blur-plan.py"

def plan(tmp_path):
    out = tmp_path / "BLUR.json"
    subprocess.run([sys.executable, str(GEN), str(out)], check=True)
    return json.loads(out.read_text())

def all_placements(p):
    for o in [p] + p["options"]:
        for key in ("game_files", "system_files"):
            yield from o.get(key, [])

def test_identity(tmp_path):
    p = plan(tmp_path)
    assert (p["schema"], p["game"], p["prefix"]) == (1, "BLUR", "sahin")
    assert p["profile"]["id"] == "pb-blur"
    assert p["profile"]["executable"] == "C:\\Games\\BLUR\\Blur.exe"
    assert "dsound=n,b" in p["profile"]["dll_overrides"]

def test_every_source_pinned_and_downloadable(tmp_path):
    for sid, s in plan(tmp_path)["sources"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", s["sha256"]), sid
        assert s["size"] > 0 and s["url"].startswith("https://"), sid

def test_placements_match_the_parser(tmp_path):
    p = plan(tmp_path)
    for g in all_placements(p):
        assert g["source"] in p["sources"]
        m, to = g.get("member", ""), g["to"]
        if m.endswith("/"):
            assert to == "" or to.endswith("/")
        else:
            assert to and not to.endswith("/")
        assert "\\" not in m + to

def test_mods_and_options(tmp_path):
    p = plan(tmp_path)
    targets = {g["to"].lower() for g in p["game_files"]}
    assert "dsound.dll" in targets
    opts = {o["id"]: o for o in p["options"]}
    assert opts["online"]["default"] is True
    assert {g["to"].lower() for g in opts["online"]["game_files"]} == {"scripts/amax-redirect.asi"}
    assert any(t["to"].lower() == "scripts/amax-redirect.cfg" for t in opts["online"]["texts"])
    assert opts["amax-client"]["default"] is False
    placed = {(g.get("member", ""), g["to"]) for g in opts["amax-client"]["game_files"]}
    assert ("amax/", "amax/") in placed and ("d3d9.dll", "d3d9.dll") in placed

def test_strip_lists_mod_leftovers(tmp_path):
    strip = {s.lower() for s in plan(tmp_path)["strip"]}
    assert {"dsound.dll", "dinput8.dll", "d3d9.dll", "scripts", "amax", "lua5.1.dll", "discord-rpc.dll"} <= strip

def test_no_copyrighted_inline_content(tmp_path):
    text = json.dumps(plan(tmp_path))
    assert "blur_12" not in text and "speed.exe" not in text

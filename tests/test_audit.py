import subprocess, sys, pathlib

AUDIT = pathlib.Path(__file__).resolve().parents[1] / "tools/audit.py"

def run(root):
    return subprocess.run([sys.executable, str(AUDIT), str(root)], capture_output=True, text=True)

def test_clean_tree_passes(tmp_path):
    (tmp_path / "a.txt").write_text("hello")
    assert run(tmp_path).returncode == 0

def test_pe_file_fails(tmp_path):
    (tmp_path / "x.bin").write_bytes(b"MZ" + b"\0" * 62 + b"PE\0\0")
    r = run(tmp_path)
    assert r.returncode == 1 and "x.bin" in r.stdout

def test_media_and_game_data_fail(tmp_path):
    for name in ("intro.bik", "cover.at9", "gamedata.pak", "Blur.exe", "d3dx9_42.dll"):
        (tmp_path / name).write_bytes(b"x")
    out = run(tmp_path).stdout
    for name in ("intro.bik", "cover.at9", "gamedata.pak", "Blur.exe", "d3dx9_42.dll"):
        assert name in out

def test_allowlist_file_skips_known_builds(tmp_path):
    (tmp_path / "wine.dll").write_bytes(b"MZ" + b"\0" * 100)
    (tmp_path / ".audit-allow").write_text("wine.dll\n")
    assert run(tmp_path).returncode == 0

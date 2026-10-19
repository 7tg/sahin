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

def test_allowlist_ignores_blank_and_comment_lines(tmp_path):
    (tmp_path / "wine.dll").write_bytes(b"MZ" + b"\0" * 100)
    (tmp_path / "#note").write_bytes(b"MZ" + b"\0" * 100)
    (tmp_path / ".audit-allow").write_text("#note\n\nwine.dll\n   \n")
    r = run(tmp_path)
    assert "wine.dll" not in r.stdout and "#note" in r.stdout

def make_zip(path, members):
    import zipfile
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            z.writestr(name, data)

def test_zip_with_pe_member_fails(tmp_path):
    make_zip(tmp_path / "b.zip", {"ok.txt": b"hi", "dir/tool.dll": b"MZ" + b"\0" * 62})
    r = run(tmp_path)
    assert r.returncode == 1 and "b.zip!dir/tool.dll" in r.stdout and "ok.txt" not in r.stdout

def test_zip_allowed_member_passes(tmp_path):
    (tmp_path / "sub").mkdir()
    make_zip(tmp_path / "sub/b.zip", {"win/system32/wine.dll": b"MZ" + b"\0" * 62,
                                      "win/system32/drivers/x.sys": b"MZ" + b"\0" * 62})
    (tmp_path / ".audit-allow").write_text("sub/*.zip!win/system32/*\n")
    assert run(tmp_path).returncode == 0

def test_zip_allow_glob_does_not_cover_other_members(tmp_path):
    make_zip(tmp_path / "b.zip", {"win/system32/wine.dll": b"MZ" + b"\0" * 62,
                                  "win/other/evil.exe": b"MZ" + b"\0" * 62})
    (tmp_path / ".audit-allow").write_text("b.zip!win/system32/*\n")
    r = run(tmp_path)
    assert r.returncode == 1 and "evil.exe" in r.stdout and "wine.dll" not in r.stdout

def test_banned_name_inside_zip_fails(tmp_path):
    make_zip(tmp_path / "b.zip", {"game/Blur.exe": b"x", "media/intro.bik": b"x"})
    out = run(tmp_path).stdout
    assert "b.zip!game/Blur.exe" in out and "b.zip!media/intro.bik" in out

def test_plain_allow_line_does_not_cover_zip_members(tmp_path):
    # A plain allow line for the zip path does not vouch for its members.
    make_zip(tmp_path / "b.zip", {"x.dll": b"MZ" + b"\0" * 62})
    (tmp_path / ".audit-allow").write_text("b.zip\n")
    assert run(tmp_path).returncode == 1

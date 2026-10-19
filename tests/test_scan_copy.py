import json, subprocess, sys, pathlib, zlib, hashlib
SCAN = pathlib.Path(__file__).resolve().parents[1] / "profile/blur/scan-copy.py"

def test_scan_lists_data_files_and_skips_mods(tmp_path):
    game = tmp_path / "Blur"
    (game / "cache").mkdir(parents=True)
    (game / "cache/root.pak").write_bytes(b"root")
    (game / "Blur.exe").write_bytes(b"MZexe")
    (game / "scripts").mkdir()
    (game / "scripts/x.asi").write_bytes(b"mod")
    (game / "dsound.dll").write_bytes(b"mod")
    out = tmp_path / "base.json"
    subprocess.run([sys.executable, str(SCAN), str(game), str(out)], check=True)
    data = json.loads(out.read_text())
    files = {f["path"]: f for f in data["files"]}
    assert set(files) == {"cache/root.pak"}
    assert files["cache/root.pak"]["size"] == 4
    assert files["cache/root.pak"]["crc32"] == f"{zlib.crc32(b'root'):08x}"
    assert files["cache/root.pak"]["sha256"] == hashlib.sha256(b"root").hexdigest()

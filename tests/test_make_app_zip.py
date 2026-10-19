import json, pathlib, subprocess, sys, zipfile

TOOL = pathlib.Path(__file__).resolve().parents[1] / "tools/make-app-zip.py"

def test_entries_are_0777_under_the_title_folder(tmp_path):
    app = tmp_path / "PPSA99404"
    (app / "sce_sys").mkdir(parents=True)
    (app / "sce_sys/param.json").write_text(json.dumps({"titleId": "PPSA99404", "contentVersion": "01.000.000"}))
    (app / "eboot.bin").write_bytes(b"\x7fELF")
    out = tmp_path / "app.zip"
    subprocess.run([sys.executable, str(TOOL), str(app), str(out)], check=True, capture_output=True)
    with zipfile.ZipFile(out) as z:
        infos = z.infolist()
    assert sorted(i.filename for i in infos) == ["PPSA99404/eboot.bin", "PPSA99404/sce_sys/param.json"]
    for i in infos:
        assert i.create_system == 3 and (i.external_attr >> 16) == 0o100777

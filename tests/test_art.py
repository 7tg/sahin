import subprocess, sys, pathlib
from PIL import Image
ROOT = pathlib.Path(__file__).resolve().parents[1]

# Sizes and modes the app build validates (build_game_app.sh): icon 512x512,
# background 3840x2160 RGB, logo 3840x2160 RGBA.
FILES = (("sahin.png", (512, 512), "RGB"),
         ("sahin-bg.png", (3840, 2160), "RGB"),
         ("sahin-logo.png", (3840, 2160), "RGBA"))


def test_art_sizes_and_determinism(tmp_path):
    for run in (tmp_path / "a", tmp_path / "b"):
        subprocess.run([sys.executable, str(ROOT / "art/make-art.py"), str(run)], check=True)
    for name, size, mode in FILES:
        im = Image.open(tmp_path / "a" / name)
        assert im.size == size and im.mode == mode
        assert (tmp_path / "a" / name).read_bytes() == (tmp_path / "b" / name).read_bytes()

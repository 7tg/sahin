#!/usr/bin/env python3
"""Draws Sahin's Home-screen art (our own, nothing from the game).

  art/make-art.py [OUT_DIR]   (default: art/)

Writes the names the app build looks up for art name "sahin":
  sahin.png        512x512 icon (icon0.png; also the launcher's game picture)
  sahin-bg.png     3840x2160 RGB background (pic0/pic1; build_game_app.sh needs this exact size)
  sahin-logo.png   3840x2160 RGBA title logo layer (pic2; transparent but for the logo)
"""
import pathlib, random, sys
from PIL import Image, ImageDraw, ImageFilter

COLOURS = [(45, 226, 230), (255, 60, 172), (255, 176, 0)]
CYAN = (45, 226, 230)


def gradient(w, h):
    """Dark blue at the top fading to near black, built from one 1xH ramp (fast, deterministic)."""
    ramp = Image.linear_gradient("L").resize((1, h), Image.BILINEAR)  # 0 at top .. 255 at bottom
    top, bottom = (11, 15, 42), (0, 0, 6)
    chans = [ramp.point(lambda v, a=a, b=b: round(a + (b - a) * v / 255)) for a, b in zip(top, bottom)]
    return Image.merge("RGB", chans).resize((w, h), Image.NEAREST)


def streaks(img, rng, count):
    w, h = img.size
    k = w / 1920
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sharp = Image.new("RGBA", img.size, (0, 0, 0, 0))
    g, s = ImageDraw.Draw(glow), ImageDraw.Draw(sharp)
    for _ in range(count):
        y = rng.randrange(h)
        x = rng.randrange(int(-200 * k), w)
        length = rng.randrange(int(200 * k), int(1200 * k))
        alpha = rng.randrange(40, 200)
        colour = rng.choice(COLOURS)
        width = rng.randrange(4, 14)
        g.line([(x, y), (x + length, y)], fill=colour + (alpha,), width=int(width * 2 * k))
        s.line([(x, y), (x + length, y)], fill=colour + (alpha,), width=max(2, int(2 * k)))
    out = img.convert("RGBA")
    out.alpha_composite(glow.filter(ImageFilter.GaussianBlur(6 * k)))
    out.alpha_composite(sharp)
    return out


def falcon(img, scale=1.0):
    """A white falcon-wing chevron (two wings and a tail notch) with a cyan glow."""
    w, h = img.size
    cx, cy, s = w / 2, h / 2, min(w, h) / 3 * scale
    wings = []
    for sign in (-1, 1):
        wings.append([(cx, cy + s * 0.35), (cx + sign * s, cy - s * 0.45),
                      (cx + sign * s * 0.35, cy - s * 0.2)])
        # inner feather tip, gives the wing a swept second blade
        wings.append([(cx, cy + s * 0.62), (cx + sign * s * 0.62, cy - s * 0.02),
                      (cx + sign * s * 0.28, cy + s * 0.12)])
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for poly in wings:
        gd.polygon(poly, fill=CYAN + (220,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s * 0.06)))
    d = ImageDraw.Draw(img)
    for poly in wings:
        d.polygon(poly, fill=(255, 255, 255, 255))
    return img


def main(out):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1923)
    icon = falcon(streaks(gradient(512, 512), rng, 18))
    icon.convert("RGB").save(out / "sahin.png", optimize=False)
    bg = falcon(streaks(gradient(3840, 2160), rng, 40), 0.8)
    bg.convert("RGB").save(out / "sahin-bg.png", optimize=False)
    logo = falcon(Image.new("RGBA", (3840, 2160), (0, 0, 0, 0)), 0.45)
    logo.save(out / "sahin-logo.png", optimize=False)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).parent)

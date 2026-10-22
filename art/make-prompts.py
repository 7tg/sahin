#!/usr/bin/env python3
"""Draws Blur's button-prompt atlas with PlayStation buttons: art/prompts.png.

  art/make-prompts.py [OUT]

Blur draws its on-screen prompts from one 512x512 texture (TexMod hash 0xF6FA7B91), with
Xbox 360 buttons and keyboard keys. The texture plugin (third_party/ds2texinject) swaps in
this one. Every pixel is drawn here from shapes and the launcher's own fonts: nothing is taken
from the game. Only the slots are Blur's, measured from its texture (positions, not pixels):
each element sits where the game's code looks for it.

The keyboard keys that stand for a controller button are drawn as that button, as the
DualSense is Blur's Xbox 360 controller: Enter, Esc, Shift and Ctrl beside A, B, X and Y are
cross, circle, square and triangle; Page Up, Page Down, Insert and Del beside LT, RT, LB and
RB are L2, R2, L1 and R1. F1, F2, +, Space and the speaker have no button and stay as keys.
"""
import pathlib, sys
from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
FONT = HERE.parent / "launcher/third_party/fonts/Inter-SemiBold.ttf"
OUT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "prompts.png"
SIZE, K = 512, 4  # drawn at K times the size, then scaled down for smooth edges

# The PlayStation symbols' colours.
CROSS, CIRCLE, SQUARE, TRIANGLE = (124, 178, 232), (255, 102, 102), (231, 162, 214), (64, 224, 160)

# Blur's slots: (x, y, w, h) of what the game shows there, and what we draw.
FACE = {"cross": CROSS, "circle": CIRCLE, "square": SQUARE, "triangle": TRIANGLE}
SLOTS = [
    # Keys without a button.
    ((1, 1, 62, 62), "key", "F2"), ((65, 1, 62, 62), "key", "F1"),
    ((129, 1, 62, 62), "key", "F2"), ((193, 1, 62, 62), "key", "F1"),
    ((1, 257, 62, 62), "key", "+"), ((1, 465, 62, 46), "key", "Space"),
    ((132, 460, 56, 40), "speaker", None),
    # Page Up, Page Down, Insert, Del, and LT, RT, LB, RB.
    ((1, 65, 62, 62), "trigger", "L2"), ((65, 65, 62, 62), "trigger", "R2"),
    ((129, 65, 62, 62), "bumper", "L1"), ((193, 65, 62, 62), "bumper", "R1"),
    ((8, 131, 49, 58), "trigger", "L2"), ((72, 131, 49, 58), "trigger", "R2"),
    ((132, 139, 57, 42), "bumper", "L1"), ((196, 139, 57, 42), "bumper", "R1"),
    # A, B, X, Y, and Enter, Esc, Shift, Ctrl.
    ((8, 200, 49, 49), "face", "cross"), ((72, 200, 49, 49), "face", "circle"),
    ((135, 200, 50, 49), "face", "square"), ((200, 200, 49, 49), "face", "triangle"),
    ((257, 449, 62, 62), "face", "cross"), ((321, 449, 62, 62), "face", "circle"),
    ((385, 465, 62, 46), "face", "square"), ((449, 449, 62, 62), "face", "triangle"),
]

DARK, BODY, RIM, WHITE = (24, 24, 28, 255), (58, 58, 64, 255), (150, 150, 158, 255), (255, 255, 255, 255)


def font(px):
    return ImageFont.truetype(str(FONT), px * K)


def box(slot):
    x, y, w, h = slot
    return x * K, y * K, (x + w) * K, (y + h) * K


def label(d, rect, text, px, anchor="mm", at=None):
    x0, y0, x1, y1 = rect
    pos = at or ((x0 + x1) / 2, (y0 + y1) / 2)
    d.text(pos, text, font=font(px), fill=WHITE, anchor=anchor)


def key(d, slot, text):
    x0, y0, x1, y1 = box(slot)
    d.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius=8 * K, fill=DARK)
    d.rounded_rectangle((x0 + 2 * K, y0 + 2 * K, x1 - 2 * K - 1, y1 - 4 * K - 1), radius=6 * K, fill=BODY,
                        outline=RIM, width=K)
    px = 13 if len(text) > 2 else 24 if text == "+" else 17
    label(d, (x0, y0, x1, y1), text, px, anchor="la", at=(x0 + 7 * K, y0 + 5 * K))


def face(d, slot, symbol):
    x0, y0, x1, y1 = box(slot)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = min(x1 - x0, y1 - y0) / 2 - K
    r = min(r, 24 * K)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=DARK)
    d.ellipse((cx - r + 2 * K, cy - r + 2 * K, cx + r - 2 * K, cy + r - 2 * K), fill=BODY, outline=RIM, width=K)
    colour, s, w = FACE[symbol] + (255,), r * 0.48, int(4.5 * K)
    if symbol == "cross":
        d.line((cx - s, cy - s, cx + s, cy + s), fill=colour, width=w)
        d.line((cx - s, cy + s, cx + s, cy - s), fill=colour, width=w)
    elif symbol == "circle":
        d.ellipse((cx - s, cy - s, cx + s, cy + s), outline=colour, width=w)
    elif symbol == "square":
        s *= 0.85
        d.rectangle((cx - s, cy - s, cx + s, cy + s), outline=colour, width=w)
    else:
        top, base = cy - s * 1.05, cy + s * 0.75
        d.polygon([(cx, top), (cx + s * 1.05, base), (cx - s * 1.05, base)], outline=colour, width=w)


def bumper(d, slot, text):
    x0, y0, x1, y1 = box(slot)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    w, h = 54 * K, 34 * K
    rect = (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
    d.rounded_rectangle(rect, radius=12 * K, fill=DARK)
    inner = (rect[0] + 2 * K, rect[1] + 2 * K, rect[2] - 2 * K, rect[3] - 2 * K)
    d.rounded_rectangle(inner, radius=10 * K, fill=BODY, outline=RIM, width=K)
    label(d, rect, text, 18)


def trigger(d, slot, text):
    x0, y0, x1, y1 = box(slot)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    w, h = 44 * K, 54 * K
    left, top, right, bottom = cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2
    # A trigger: rounder than a bumper, taller than wide.
    d.rounded_rectangle((left, top, right, bottom), radius=16 * K, fill=DARK)
    d.rounded_rectangle((left + 2 * K, top + 2 * K, right - 2 * K, bottom - 2 * K), radius=14 * K, fill=BODY,
                        outline=RIM, width=K)
    label(d, (left, top, right, bottom), text, 18, at=(cx, cy + 3 * K))


def speaker(d, slot, _):
    x0, y0, x1, y1 = box(slot)
    cx, cy = (x0 + x1) / 2 - 8 * K, (y0 + y1) / 2
    body = [(cx - 16 * K, cy - 7 * K), (cx - 8 * K, cy - 7 * K), (cx + 4 * K, cy - 16 * K),
            (cx + 4 * K, cy + 16 * K), (cx - 8 * K, cy + 7 * K), (cx - 16 * K, cy + 7 * K)]
    d.polygon(body, fill=WHITE, outline=DARK, width=2 * K)
    for radius in (10, 17, 24):
        r = radius * K
        d.arc((cx + 4 * K - r, cy - r, cx + 4 * K + r, cy + r), start=-45, end=45, fill=WHITE, width=3 * K)


DRAW = {"key": key, "face": face, "bumper": bumper, "trigger": trigger, "speaker": speaker}


def main():
    img = Image.new("RGBA", (SIZE * K, SIZE * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for slot, kind, what in SLOTS:
        DRAW[kind](d, slot, what)
    img.resize((SIZE, SIZE), Image.LANCZOS).save(OUT, optimize=True)
    print(f"{OUT}")


if __name__ == "__main__":
    main()

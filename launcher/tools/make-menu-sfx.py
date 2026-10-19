#!/usr/bin/env python3
# Original menu clicks for Prospero BlackBox.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Short ticks for the store's buttons (move, confirm, back, and the rest of
# the interface cues). Generated here so a clone can rebuild the same bytes.
# These are not recordings from any console.

import math
import struct
import wave
from pathlib import Path

RATE = 48000
PEAK = 0.32

# Each cue is a list of noise bursts: start_ms, centre Hz, Q, decay ms, level.
CUES = {
    "focus": [(0, 3400, 9, 8, 1.0)],
    "select": [(0, 2500, 7, 12, 1.0), (18, 4100, 11, 9, 0.8)],
    "back": [(0, 1400, 6, 16, 1.0)],
    "tab": [(0, 2100, 5, 14, 1.0)],
    "open": [(0, 1800, 6, 12, 0.8), (32, 2800, 8, 12, 1.0)],
    "modal_open": [(0, 1600, 5, 14, 0.75), (40, 2600, 7, 14, 1.0)],
    "modal_close": [(0, 2600, 7, 12, 1.0), (36, 1500, 5, 16, 0.8)],
    "toggle": [(0, 2200, 8, 10, 1.0)],
    "slider": [(0, 4200, 12, 5, 0.7)],
    "error": [(0, 380, 3, 40, 1.0), (70, 280, 3, 50, 0.85)],
    "notify": [(0, 1800, 8, 18, 0.7), (90, 2400, 8, 22, 1.0)],
    "launch": [(0, 1400, 6, 14, 0.6), (50, 2100, 7, 14, 0.85), (100, 3000, 8, 16, 1.0)],
    "favorite_on": [(0, 2400, 8, 10, 0.7), (40, 3600, 10, 12, 1.0)],
    "favorite_off": [(0, 1600, 6, 16, 1.0)],
    "saved": [(0, 2000, 7, 12, 0.75), (48, 3000, 9, 14, 1.0)],
    "welcome": [(0, 1200, 5, 20, 0.55), (70, 1800, 6, 18, 0.8), (140, 2600, 8, 20, 1.0)],
    "resume": [(0, 1900, 6, 14, 1.0)],
    "tick": [(0, 3800, 12, 4, 0.6)],
    "type": [(0, 3000, 10, 6, 0.65)],
    "erase": [(0, 1100, 5, 12, 0.8)],
}


def noise(seed):
    value = seed & 0xFFFFFFFF
    while True:
        value = (value * 1664525 + 1013904223) & 0xFFFFFFFF
        yield (value >> 8) / 16777216.0 * 2.0 - 1.0


def burst(start, frames, seed, centre, q, decay_ms, level):
    samples = [0.0] * frames
    rng = noise(seed)
    omega = 2.0 * math.pi * centre / RATE
    alpha = math.sin(omega) / (2.0 * q)
    cos = math.cos(omega)
    a0 = 1.0 + alpha
    b0, b1, b2 = alpha / a0, 0.0, -alpha / a0
    a1, a2 = -2.0 * cos / a0, (1.0 - alpha) / a0
    x1 = x2 = y1 = y2 = 0.0
    decay = decay_ms / 1000.0
    for i in range(frames - start):
        t = i / RATE
        excite = math.exp(-t / 0.0012)
        x0 = next(rng) * excite
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, x0
        y2, y1 = y1, y0
        samples[start + i] = y0 * math.exp(-t / decay) * level
    return samples


def render(pulses, seed):
    end_ms = max(start + decay * 4 + 12 for start, _, _, decay, _ in pulses)
    frames = int(RATE * end_ms / 1000.0)
    mixed = [0.0] * frames
    for index, (start_ms, centre, q, decay, level) in enumerate(pulses):
        start = int(RATE * start_ms / 1000.0)
        layer = burst(start, frames, seed + index * 997, centre, q, decay, level)
        for i, sample in enumerate(layer):
            mixed[i] += sample
    peak = max(abs(sample) for sample in mixed) or 1.0
    scale = PEAK / peak
    fade = min(64, frames // 4)
    for i, sample in enumerate(mixed):
        gain = scale
        if i < 8:
            gain *= i / 8.0
        if i >= frames - fade:
            gain *= (frames - i) / fade
        mixed[i] = max(-1.0, min(1.0, sample * gain))
    return mixed


def write_wav(path, samples):
    with wave.open(str(path), "w") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(b"".join(struct.pack("<h", int(sample * 32767.0)) for sample in samples))


def main():
    root = Path(__file__).resolve().parents[1] / "assets" / "audio" / "sfx" / "menu"
    root.mkdir(parents=True, exist_ok=True)
    names = []
    for index, (name, pulses) in enumerate(CUES.items()):
        samples = render(pulses, 0xA11CE + index * 131)
        file_name = f"{name}_01.wav"
        write_wav(root / file_name, samples)
        names.append(file_name)
        peak = max(abs(sample) for sample in samples)
        if not 0.2 <= peak <= 0.45:
            raise SystemExit(f"{file_name} peak {peak:.3f} is out of range")
    (root / "index.txt").write_text("".join(name + "\n" for name in sorted(names)))
    print(f"wrote {len(names)} clicks to {root}")


if __name__ == "__main__":
    main()

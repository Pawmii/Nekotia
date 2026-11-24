#!/usr/bin/env python3
"""initrd/usr/share/sounds/meow.wav: a short synthesized chime and meow (22.05 kHz, 16-bit mono — `play`
converts it to 48 kHz stereo). Regenerate with: python3 scripts/make-meow.py"""
import math, struct, sys

RATE = 22050
out = []

def tone(f0, f1, ms, amp=0.5, bell=False, vibrato=0.0):
    n = int(RATE * ms / 1000)
    phase = 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        f *= 1 + vibrato * math.sin(2 * math.pi * 6 * i / RATE)
        phase += f / RATE
        env = min(1, i / (0.008 * RATE)) * (math.exp(-4 * t) if bell else min(1, (n - i) / (0.04 * RATE)))
        s = math.sin(2 * math.pi * phase) * 0.75 + math.sin(4 * math.pi * phase) * 0.18 + math.sin(6 * math.pi * phase) * 0.07
        out.append(s * env * amp)

def rest(ms):
    out.extend([0.0] * int(RATE * ms / 1000))

for f in (523.25, 659.25, 783.99):          # C5 E5 G5, a little chime
    tone(f, f, 160, 0.40, bell=True)
tone(1046.5, 1046.5, 420, 0.40, bell=True)  # C6
rest(120)
tone(600, 980, 110, 0.45)                   # "mi-"
tone(980, 620, 380, 0.45, vibrato=0.01)     # "-aow"

path = sys.argv[1] if len(sys.argv) > 1 else "initrd/usr/share/sounds/meow.wav"
data = b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767)) for s in out)
with open(path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
    f.write(b"data" + struct.pack("<I", len(data)) + data)

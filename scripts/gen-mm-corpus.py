#!/usr/bin/env python3
"""Generate a tiny multimodal test corpus with stdlib only (no pillow on this
box): PNG via zlib/struct + a 5x7 bitmap font, WAV via the wave module.

  python3 scripts/gen-mm-corpus.py <outdir>

Produces:
  notes.md       text:  ground truth that ties the corpus together
  banner.png     image: high-contrast pixel-text "PHOENIX BUDGET 480K"
  scene.png      image: red square on white background (description test)
  beeps.wav      audio: three rising tones (description test)
"""
import struct
import sys
import wave
import zlib
from pathlib import Path

# --- minimal PNG -------------------------------------------------------------

def png_write(path, w, h, rgb_rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    out = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    Path(path).write_bytes(out)

FONT = {  # 5x7, rows top->bottom, bit 4 = leftmost
    "A": [0x0E,0x11,0x11,0x1F,0x11,0x11,0x11], "B": [0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E],
    "C": [0x0E,0x11,0x10,0x10,0x10,0x11,0x0E], "D": [0x1E,0x11,0x11,0x11,0x11,0x11,0x1E],
    "E": [0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F], "G": [0x0E,0x11,0x10,0x17,0x11,0x11,0x0F],
    "H": [0x11,0x11,0x11,0x1F,0x11,0x11,0x11], "I": [0x0E,0x04,0x04,0x04,0x04,0x04,0x0E],
    "K": [0x11,0x12,0x14,0x18,0x14,0x12,0x11], "N": [0x11,0x19,0x15,0x13,0x11,0x11,0x11],
    "O": [0x0E,0x11,0x11,0x11,0x11,0x11,0x0E], "P": [0x1E,0x11,0x11,0x1E,0x10,0x10,0x10],
    "T": [0x1F,0x04,0x04,0x04,0x04,0x04,0x04], "U": [0x11,0x11,0x11,0x11,0x11,0x11,0x0E],
    "X": [0x11,0x11,0x0A,0x04,0x0A,0x11,0x11], "0": [0x0E,0x11,0x13,0x15,0x19,0x11,0x0E],
    "4": [0x02,0x06,0x0A,0x12,0x1F,0x02,0x02], "8": [0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E],
    " ": [0]*7,
}

def text_png(path, text, scale=8):
    cw, ch = 6 * scale, 7 * scale  # 5px glyph + 1px gap
    w, h = cw * len(text) + 2 * scale, ch + 4 * scale
    rows = [[255] * (w * 3) for _ in range(h)]
    for ci, c in enumerate(text):
        glyph = FONT.get(c.upper(), FONT[" "])
        for gy in range(7):
            for gx in range(5):
                if glyph[gy] >> (4 - gx) & 1:
                    for sy in range(scale):
                        for sx in range(scale):
                            x = scale + ci * cw + gx * scale + sx
                            y = 2 * scale + gy * scale + sy
                            rows[y][x*3:x*3+3] = [0, 0, 0]
    png_write(path, w, h, rows)

def scene_png(path):
    w = h = 224
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            inside = 60 <= x < 164 and 60 <= y < 164
            row += [220, 30, 30] if inside else [255, 255, 255]
        rows.append(row)
    png_write(path, w, h, rows)

def beeps_wav(path):
    import math
    sr = 16000
    samples = []
    for freq in (440, 660, 880):
        for i in range(int(sr * 0.4)):
            samples.append(int(12000 * math.sin(2 * math.pi * freq * i / sr)))
        samples += [0] * int(sr * 0.1)
    with wave.open(str(path), "w") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(sr)
        f.writeframes(b"".join(struct.pack("<h", s) for s in samples))

def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "mm-corpus")
    out.mkdir(parents=True, exist_ok=True)
    (out / "notes.md").write_text(
        "# Phoenix artifacts\n\n"
        "The banner image shows the Phoenix budget figure. The scene image "
        "is a red square used as the project's placeholder logo. The audio "
        "file contains the three-tone Phoenix build-success chime.\n")
    text_png(out / "banner.png", "PHOENIX BUDGET 480K")
    scene_png(out / "scene.png")
    beeps_wav(out / "beeps.wav")
    print(f"wrote {out}/: notes.md banner.png scene.png beeps.wav")

if __name__ == "__main__":
    main()

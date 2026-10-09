"""
Draws the Pocket Nova icon (Nova's happy face on a glowing 5x5 LED grid)
and writes PocketNova.ico (16-256 px) and icon.png (256 px).
Plain Python, no image libraries: each pixel is supersampled 4x4.

    python make_icon.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# Nova's happy face, as on the device: '.' off, W eye white, C pupil, P smile.
FACE = [
    ".....",
    "WW.WW",
    "WC.WC",
    "WW.WW",
    ".PPP.",
]
COLORS = {"W": (235, 232, 255), "C": (40, 230, 255), "P": (255, 70, 170)}

TOP, BOTTOM = (132, 70, 255), (52, 18, 120)      # tile gradient
GRID0, CELL, LED_R = 0.17, 0.132, 0.054          # LED layout (unit coords)

LEDS = []
for r, row in enumerate(FACE):
    for c, ch in enumerate(row):
        cx, cy = GRID0 + CELL * (c + 0.5), GRID0 + 0.03 + CELL * (r + 0.5)
        LEDS.append((cx, cy, COLORS.get(ch)))


def rounded_rect_alpha(x, y, inset=0.035, rad=0.23):
    # signed-distance test for a rounded square
    qx = abs(x - 0.5) - (0.5 - inset - rad)
    qy = abs(y - 0.5) - (0.5 - inset - rad)
    d = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - rad
    return 1.0 if d < 0 else 0.0


def sample(x, y):
    """Colour (r, g, b, a) of one sample point in unit coordinates."""
    if not rounded_rect_alpha(x, y):
        return (0, 0, 0, 0)
    t = y
    r = TOP[0] + (BOTTOM[0] - TOP[0]) * t
    g = TOP[1] + (BOTTOM[1] - TOP[1]) * t
    b = TOP[2] + (BOTTOM[2] - TOP[2]) * t
    # soft sheen on the upper half
    sheen = max(0.0, 0.18 - 0.5 * ((x - 0.35) ** 2 + (y - 0.12) ** 2)) * 255
    r, g, b = r + sheen, g + sheen, b + sheen
    # glow from lit LEDs
    for cx, cy, col in LEDS:
        if col:
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            k = 0.55 * math.exp(-d2 / 0.0045)
            r, g, b = r + col[0] * k, g + col[1] * k, b + col[2] * k
    # the LEDs themselves
    for cx, cy, col in LEDS:
        if (x - cx) ** 2 + (y - cy) ** 2 <= LED_R ** 2:
            if col:
                r, g, b = col
            else:
                r, g, b = r * 0.55, g * 0.55, b * 0.62   # unlit LED: darker dimple
            break
    return (min(255, r), min(255, g), min(255, b), 255)


def render(size, ss=4):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    r, g, b, a = sample((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                row += bytes((int(acc[0] / acc[3]), int(acc[1] / acc[3]), int(acc[2] / acc[3]), int(a)))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png_bytes(rows, size):
    raw = b"".join(b"\0" + r for r in rows)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [256, 64, 48, 32, 24, 16]
    pngs = {s: png_bytes(render(s), s) for s in sizes}
    with open(os.path.join(HERE, "icon.png"), "wb") as f:
        f.write(pngs[256])
    # ICO: header, one directory entry per size, then the PNG images.
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries, blobs = b"", b""
    for s in sizes:
        data = pngs[s]
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    with open(os.path.join(HERE, "PocketNova.ico"), "wb") as f:
        f.write(header + entries + blobs)
    print("Wrote icon.png and PocketNova.ico")


if __name__ == "__main__":
    main()

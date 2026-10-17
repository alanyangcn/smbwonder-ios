#!/usr/bin/env python3
"""App icon for the iOS port: original artwork (gradient, a white "W", sparkles). No Nintendo characters, logos or game
art. Writes the 1024x1024 single-size icon of ios-probe/Assets.xcassets/AppIcon.appiconset (iOS rounds the corners).
Usage: make_app_icon.py [--out <png>] (needs Pillow: work/tools/venv)."""
import argparse, math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'ios-probe/Assets.xcassets/AppIcon.appiconset/AppIcon.png'

def gradient(size, top, bottom):
    img = Image.new('RGB', (size, size)); px = img.load()
    for y in range(size):
        for x in range(size):
            t = min(1.0, max(0.0, (0.75 * y + 0.25 * x) / size))   # mostly vertical, a little diagonal
            px[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return img

def w_polygon(cx, cy, width, height, stroke):
    """A bold W as one polygon: four slanted strokes meeting at three points."""
    x0, x1 = cx - width / 2, cx + width / 2; top, bot = cy - height / 2, cy + height / 2
    q = width / 4; s = stroke
    return [(x0, top), (x0 + s, top), (x0 + q + s * 0.35, bot - s * 1.6), (cx - s * 0.5, top + height * 0.28),
            (cx + s * 0.5, top + height * 0.28), (x1 - q - s * 0.35, bot - s * 1.6), (x1 - s, top), (x1, top),
            (x1 - q + s * 0.45, bot), (x1 - q - s * 0.45, bot), (cx, top + height * 0.28 + s * 1.15),
            (x0 + q + s * 0.45, bot), (x0 + q - s * 0.45, bot)]

def sparkle(draw, cx, cy, r, fill):
    pts = []
    for k in range(8):
        a = math.pi / 4 * k - math.pi / 2; rr = r if k % 2 == 0 else r * 0.28
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    draw.polygon(pts, fill=fill)

def build(out):
    img = gradient(SIZE, (255, 92, 70), (138, 58, 230)).convert('RGBA')
    glow = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0)); g = ImageDraw.Draw(glow)
    g.ellipse((140, 120, 884, 864), fill=(255, 230, 120, 110)); glow = glow.filter(ImageFilter.GaussianBlur(90))
    img = Image.alpha_composite(img, glow)
    poly = w_polygon(SIZE / 2, SIZE / 2 + 30, 700, 520, 128)
    shadow = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0)); ImageDraw.Draw(shadow).polygon([(x + 18, y + 26) for x, y in poly], fill=(40, 10, 70, 150))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(14)))
    d = ImageDraw.Draw(img)
    d.polygon(poly, fill=(255, 255, 255, 255))
    d.line(poly + [poly[0]], fill=(255, 214, 90, 255), width=10, joint='curve')
    for cx, cy, r in ((230, 210, 70), (820, 250, 52), (790, 820, 40), (200, 790, 30)):
        sparkle(d, cx, cy, r, (255, 247, 200, 255))
    out.parent.mkdir(parents=True, exist_ok=True)
    img.convert('RGB').save(out)   # app icons are opaque

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--out', type=Path, default=OUT)
    a = p.parse_args(); build(a.out); print(f'icon -> {a.out}')

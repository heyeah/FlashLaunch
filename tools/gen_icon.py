#!/usr/bin/env python3
"""Generate app icon (lightning bolt) as multi-size ICO."""
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "app.ico")
SIZES = [16, 24, 32, 48, 64, 128, 256]
imgs = []
for s in SIZES:
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = max(1, s // 16)
    d.rounded_rectangle([m, m, s - m, s - m], radius=max(2, s // 5),
                        fill=(32, 38, 52, 255), outline=(76, 180, 255, 255), width=max(1, s // 32))
    # lightning polygon
    pts = [(0.56, 0.12), (0.30, 0.54), (0.47, 0.54), (0.40, 0.88),
           (0.72, 0.42), (0.54, 0.42), (0.66, 0.12)]
    poly = [(int(x * s), int(y * s)) for x, y in pts]
    d.polygon(poly, fill=(255, 214, 92, 255))
    imgs.append(im)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
imgs[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES])
print(OUT, os.path.getsize(OUT), "bytes")

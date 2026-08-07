#!/usr/bin/env python3
"""Convert an image to C header with RGB888 pixel data for kernel boot screen."""
import sys
from PIL import Image

if len(sys.argv) < 4:
    print("Usage: img2c.py <input.png> <width> <height> [output.h]")
    sys.exit(1)

src = sys.argv[1]
w = int(sys.argv[2])
h = int(sys.argv[3])
out = sys.argv[4] if len(sys.argv) > 4 else src.replace('.png', '.h')

img = Image.open(src).convert('RGB')
img = img.resize((w, h), Image.LANCZOS)
pixels = list(img.getdata())

with open(out, 'w') as f:
    f.write(f'#ifndef BOOT_LOGO_H\n#define BOOT_LOGO_H\n\n')
    f.write(f'#define LOGO_W {w}\n#define LOGO_H {h}\n\n')
    f.write(f'static const unsigned int logo_data[{w*h}] = {{\n')
    for i, (r, g, b) in enumerate(pixels):
        color = (r << 16) | (g << 8) | b
        f.write(f'    0x{color:06X},\n')
    f.write(f'}};\n\n#endif\n')

print(f"Written {w}x{h} logo to {out}")

#!/usr/bin/env python3
"""Generate KairoOS Windows app assets (PNG tiles + ICO) without any dependency.

Draws a simple logo: dark navy tile, rounded accent-blue window with a white
mountain / window outline, echoing the KairoOS desktop look.
"""

import struct
import zlib
import os


BG = (8, 8, 24)          # 0x080818
ACCENT = (59, 130, 246)  # 0x3B82F6
WHITE = (232, 238, 255)
DARK = (16, 18, 44)


def rounded_rect_mask(size, radius):
    def inside(x, y):
        cx = min(max(x, radius), size - 1 - radius)
        cy = min(max(y, radius), size - 1 - radius)
        dx, dy = x - cx, y - cy
        return dx * dx + dy * dy <= radius * radius
    return inside


def render(size):
    """Return a list of (R,G,B,A) rows for the logo tile."""
    return render_rect(size, size)


def render_rect(w, h):
    """Render the logo on a w x h canvas (square logo centered)."""
    size = min(w, h)
    pad = (w - size) // 2, (h - size) // 2
    tile = render_square(size)
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            if pad[0] <= x < pad[0] + size and pad[1] <= y < pad[1] + size:
                row.append(tile[y - pad[1]][x - pad[0]])
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def render_square(size):
    """Return a list of (R,G,B,A) rows for the square logo tile."""
    p = rounded_rect_mask(size, max(3, size // 8))
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            if not p(x, y):
                row.append((0, 0, 0, 0))
                continue
            # tile background: vertical gradient from BG to DARK
            t = y / max(1, size - 1)
            c = tuple(int(BG[i] + (DARK[i] - BG[i]) * t) for i in range(3))
            # centered accent window (rounded square, ~62% of tile)
            ws = int(size * 0.62)
            wx = (size - ws) // 2
            wy = int(size * 0.20)
            inside_w = rounded_rect_mask(ws, ws // 4)
            if wx <= x < wx + ws and wy <= y < wy + ws and inside_w(x - wx, y - wy):
                # window body with subtle top gradient in accent
                wdt = (y - wy) / max(1, ws)
                c = tuple(int(ACCENT[i] * (1 - 0.25 * wdt)) for i in range(3))
                # white rounded rectangle "screen" inside
                ss = int(ws * 0.66)
                sx = wx + (ws - ss) // 2
                sy = wy + (ws - ss) // 2
                inside_s = rounded_rect_mask(ss, ss // 5)
                if sx <= x < sx + ss and sy <= y < sy + ss and inside_s(x - sx, y - sy):
                    c = (222, 230, 250)
                    # little blue "window" glyph
                    gs = int(ss * 0.42)
                    gx = sx + (ss - gs) // 2
                    gy = sy + (ss - gs) // 2
                    if gx <= x < gx + gs and gy <= y < gy + gs:
                        c = tuple(int(ACCENT[i] * 0.85) for i in range(3))
            row.append((c[0], c[1], c[2], 255))
        rows.append(row)
    return rows


def write_png(path, size, height=None):
    if height is None:
        height = size
    rows = render_rect(size, height)
    raw = b"".join(b"\x00" + b"".join(bytes(px) for px in row) for row in rows)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def write_ico(path, sizes=(16, 32, 48, 256)):
    images = []
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    body = b""
    for size in sizes:
        rows = render(size)
        bmp_rows = b""
        for row in reversed(rows):
            line = b""
            for (r, g, b, a) in row:
                line += bytes((b, g, r, a))
            bmp_rows += line
        hdr = struct.pack("<IiiHHIIiiII",
                          40, size, size * 2, 1, 32, 0, len(bmp_rows), 0, 0, 0, 0)
        data = hdr + bmp_rows
        byte_size = 1 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", byte_size, byte_size, 0, 0, 1, 32, len(data), offset)
        body += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + body)


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
    os.makedirs(out, exist_ok=True)
    write_png(os.path.join(out, "Logo.png"), 150)
    write_png(os.path.join(out, "SmallLogo.png"), 44)
    write_png(os.path.join(out, "StoreLogo.png"), 50)
    write_png(os.path.join(out, "WideLogo.png"), 310, 150)  # wide canvas, logo centered
    write_ico(os.path.join(out, "app.ico"))
    print("assets generated in", out)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate assets/icon.png -- the icon Sileo shows for the package.

Node's hexagon, drawn as a stroked outline with a filled inner mark. It has to
read at 40px, which is the only size that matters in a package list, so the
hexagon is heavy-stroked and the interior stays mostly empty.

Pure stdlib, same approach as the sibling ports: render at 4x and box-downsample
for antialiasing, then write the PNG by hand, so the build needs no image
libraries on any runner.
"""
import math
import os
import struct
import zlib

S = 256          # final size
SS = 4           # supersample factor
N = S * SS

BG = (0x0D, 0x11, 0x17)      # dark canvas, matching the sibling ports
GREEN = (0x5F, 0xA0, 0x4E)   # node green
LIGHT = (0x8C, 0xC8, 0x4B)   # node light green

R = int(N * 0.225)           # corner radius


def inside_rounded(x, y):
    if R <= x < N - R or R <= y < N - R:
        return 0 <= x < N and 0 <= y < N
    cx = R if x < R else N - R - 1
    cy = R if y < R else N - R - 1
    return (x - cx) ** 2 + (y - cy) ** 2 <= R * R


def stroke(buf, x0, y0, x1, y1, w, color):
    """Filled capsule from (x0,y0) to (x1,y1), radius w."""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    lo_x, hi_x = int(min(x0, x1) - w - 1), int(max(x0, x1) + w + 2)
    lo_y, hi_y = int(min(y0, y1) - w - 1), int(max(y0, y1) + w + 2)
    for y in range(max(0, lo_y), min(N, hi_y)):
        for x in range(max(0, lo_x), min(N, hi_x)):
            t = 0.0 if L2 == 0 else ((x - x0) * dx + (y - y0) * dy) / L2
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            px, py = x0 + t * dx, y0 + t * dy
            if (x - px) ** 2 + (y - py) ** 2 <= w * w:
                buf[y * N + x] = color


def hexagon(cx, cy, rad):
    """Flat-left/right hexagon, the orientation Node's mark uses."""
    return [(cx + rad * math.cos(math.radians(a)),
             cy + rad * math.sin(math.radians(a)))
            for a in (-90, -30, 30, 90, 150, 210)]


def polyline(buf, pts, w, color, close=True):
    n = len(pts)
    last = n if close else n - 1
    for i in range(last):
        a, b = pts[i], pts[(i + 1) % n]
        stroke(buf, a[0], a[1], b[0], b[1], w, color)


def main():
    buf = [None] * (N * N)
    for y in range(N):
        for x in range(N):
            if inside_rounded(x, y):
                buf[y * N + x] = BG

    u = N / 100.0
    cx = cy = 50 * u

    # Outer hexagon: the silhouette that has to survive being 40px tall.
    polyline(buf, hexagon(cx, cy, 36 * u), 5.0 * u, GREEN)

    # Inner mark: a bold "N" stroke inside the hexagon. Two uprights and the
    # diagonal, which is what stays legible once the hexagon is small.
    w = 4.6 * u
    top, bot = 32 * u, 68 * u
    left, right = 38 * u, 62 * u
    stroke(buf, left,  bot, left,  top, w, LIGHT)
    stroke(buf, left,  top, right, bot, w, LIGHT)
    stroke(buf, right, bot, right, top, w, LIGHT)

    # downsample with alpha from coverage
    out = bytearray()
    k = SS * SS
    for y in range(S):
        out.append(0)
        for x in range(S):
            r = g = b = a = 0
            for j in range(SS):
                row = (y * SS + j) * N + x * SS
                for i in range(SS):
                    p = buf[row + i]
                    if p is not None:
                        r += p[0]; g += p[1]; b += p[2]; a += 255
            if a:
                n = a // 255
                out += bytes((r // n, g // n, b // n, a // k))
            else:
                out += b"\0\0\0\0"

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(out), 9))
           + chunk(b"IEND", b""))

    dst = os.path.join(os.path.dirname(__file__), "..", "assets", "icon.png")
    with open(dst, "wb") as f:
        f.write(png)
    print("wrote %s (%d bytes, %dx%d)" % (os.path.normpath(dst), len(png), S, S))


main()

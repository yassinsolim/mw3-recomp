#!/usr/bin/env python3
"""Make the desktop launcher's icon from the project's own emblem, the one the
Android apps use (android/app/src/main/res/drawable/ic_launcher_foreground.xml):
a reticle ring with four ticks around three stencil bars, on gunmetal. The
left half takes the campaign's amber, the right half multiplayer's green.
Nothing of the game's goes into it.

    tools/launcher_icon.py

Writes launcher/icon/launcher.ico (what Windows shows for the program) and
launcher/icon/launcher.bmp (what the window is given, on both systems).
"""
import os
from PIL import Image, ImageDraw

SIZE = 256
SCALE = 4                      # drawn large, then reduced, for smooth edges
CAMPAIGN = (0xE8, 0xA3, 0x3D, 255)
MULTIPLAYER = (0x3F, 0xCF, 0x8E, 255)
BARS = (0xEC, 0xEF, 0xF2, 255)


def emblem(accent):
    n = SIZE * SCALE
    u = n / 108.0              # the vector's 108-unit viewport
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Gunmetal tile with rounded corners, lit from the upper left.
    shade = Image.new("RGBA", (n, n))
    for y in range(n):
        for_row = []
        for x in range(0, n, 4):
            t = min(1.0, (((x - 38 * u) ** 2 + (y - 34 * u) ** 2) ** 0.5) / (90 * u))
            c = tuple(int(a + (b - a) * t) for a, b in zip((0x2A, 0x32, 0x3C), (0x0B, 0x0E, 0x12)))
            for_row.append(c)
        for i, c in enumerate(for_row):
            shade.paste(c + (255,), (i * 4, y, i * 4 + 4, y + 1))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, n - 1, n - 1), radius=int(22 * u), fill=255)
    im.paste(shade, (0, 0), mask)
    # Ring, ticks and bars: the vector's geometry.
    r, w = 27 * u, 3.2 * u
    d.ellipse((54 * u - r - w / 2, 54 * u - r - w / 2, 54 * u + r + w / 2, 54 * u + r + w / 2), outline=accent, width=int(w))
    for x0, y0, x1, y1 in ((52.4, 22, 55.6, 31), (52.4, 77, 55.6, 86), (22, 52.4, 31, 55.6), (77, 52.4, 86, 55.6)):
        d.rectangle((x0 * u, y0 * u, x1 * u, y1 * u), fill=accent)
    for x, top, bottom in ((43, 45.5, 64.5), (51.5, 43.5, 64.5), (60, 45.5, 64.5)):
        d.polygon([(x * u, top * u), ((x + 5) * u, (top - 2.5) * u), ((x + 5) * u, bottom * u), (x * u, bottom * u)], fill=BARS)
    return im.resize((SIZE, SIZE), Image.LANCZOS)


icon = emblem(CAMPAIGN)
icon.paste(emblem(MULTIPLAYER).crop((SIZE // 2, 0, SIZE, SIZE)), (SIZE // 2, 0))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launcher", "icon")
icon.save(os.path.join(out, "launcher.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
icon.save(os.path.join(out, "launcher.bmp"))
print("wrote launcher/icon/launcher.ico and launcher.bmp")

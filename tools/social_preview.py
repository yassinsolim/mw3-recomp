#!/usr/bin/env python3
"""Make the four repositories' social preview pictures (1280x640) in docs/media/.

    python tools/social_preview.py [output folder]

Original art only: the port's short name in the launcher's fonts
(third_party/fonts/), what it is, a drawn handheld and the frame rates the
README measured. No game art, logo or screenshot. GitHub has no API for the
picture: upload each at Settings > General > Social preview of its repository.
Needs Pillow (pip install pillow).
"""
import os
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageFilter

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(REPO, 'third_party/fonts')
W, H, S = 1280, 640, 2          # drawn at S times the size, then reduced

BG_TOP, BG_BOTTOM = (28, 30, 34), (12, 13, 15)
WHITE, GREY = (236, 236, 232), (150, 154, 160)

# directory key, short name, full name, repository, accent colour
GAMES = [
    ('mw3', 'MW3', 'Call of Duty: Modern Warfare 3', 'mw3-recomp', (232, 178, 52)),
    ('mw2', 'MW2', 'Call of Duty: Modern Warfare 2', 'mw2-recomp', (110, 190, 232)),
    ('cod4', 'CoD4', 'Call of Duty 4: Modern Warfare', 'cod4-recomp', (150, 214, 120)),
    ('aw', 'AW', 'Call of Duty: Advanced Warfare', 'aw-recomp', (226, 120, 100)),
]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size * S)


def gradient():
    img = Image.new('RGB', (W * S, H * S))
    d = ImageDraw.Draw(img)
    for y in range(H * S):
        t = y / (H * S - 1)
        d.line([(0, y), (W * S, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    return img


def add(a, b):
    return ImageChops.add(a, b)


def handheld(d, x, y, w, h, accent):
    """A landscape handheld: body, screen, two sticks, a d-pad and four buttons."""
    s = S
    body = [x * s, y * s, (x + w) * s, (y + h) * s]
    d.rounded_rectangle(body, radius=int(h * 0.22 * s), outline=GREY, width=4 * s, fill=(20, 22, 25))
    sw, sh = w * 0.56, h * 0.80
    sx, sy = x + (w - sw) / 2, y + (h - sh) / 2
    d.rounded_rectangle([sx * s, sy * s, (sx + sw) * s, (sy + sh) * s], radius=10 * s,
                        outline=accent, width=3 * s, fill=(10, 11, 13))
    # the screen says what runs on it
    f = font('BarlowCondensed-Medium.ttf', 34)
    for i, line in enumerate(('ARM64', 'Vulkan')):
        tw = d.textlength(line, font=f)
        d.text(((sx + sw / 2) * s - tw / 2, (sy + sh / 2 - 40 + i * 42) * s), line, font=f, fill=WHITE)
    # sticks and d-pad on the left, buttons on the right
    lx, rx = x + (sx - x) / 2, sx + sw + (x + w - sx - sw) / 2
    r = 17
    d.ellipse([(lx - r) * s, (y + h * 0.30 - r) * s, (lx + r) * s, (y + h * 0.30 + r) * s], outline=GREY, width=3 * s)
    d.ellipse([(rx - r) * s, (y + h * 0.66 - r) * s, (rx + r) * s, (y + h * 0.66 + r) * s], outline=GREY, width=3 * s)
    px, py, a, t = lx, y + h * 0.68, 22, 8
    d.rectangle([(px - t) * s, (py - a) * s, (px + t) * s, (py + a) * s], outline=GREY, width=3 * s)
    d.rectangle([(px - a) * s, (py - t) * s, (px + a) * s, (py + t) * s], outline=GREY, width=3 * s)
    by, br = y + h * 0.30, 7
    for dx, dy in ((0, -17), (0, 17), (-17, 0), (17, 0)):
        d.ellipse([(rx + dx - br) * s, (by + dy - br) * s, (rx + dx + br) * s, (by + dy + br) * s], outline=GREY, width=2 * s)


def arrow(d, x, y, length, color):
    s = S
    d.line([(x * s, y * s), ((x + length) * s, y * s)], fill=color, width=5 * s)
    d.polygon([((x + length + 14) * s, y * s), ((x + length - 4) * s, (y - 12) * s), ((x + length - 4) * s, (y + 12) * s)], fill=color)


def make(short, name, full, repo, accent, out):
    img = add(gradient(), glow_layer(accent))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 14 * S, H * S], fill=accent)

    d.text((70 * S, 70 * S), name, font=font('Michroma-Regular.ttf', 150), fill=WHITE)
    d.text((74 * S, 262 * S), full, font=font('BarlowCondensed-Medium.ttf', 50), fill=accent)

    f = font('BarlowCondensed-Medium.ttf', 40)
    y = 352
    d.text((74 * S, y * S), 'Xbox 360', font=f, fill=WHITE)
    x = 74 + d.textlength('Xbox 360', font=f) / S + 22
    arrow(d, x, y + 28, 48, accent)
    d.text(((x + 88) * S, y * S), 'native ARM64 Android', font=f, fill=WHITE)

    d.text((74 * S, 432 * S), 'Static recompilation of your own disc. Not an emulator.',
           font=font('BarlowSemiCondensed-Regular.ttf', 30), fill=GREY)

    chip = font('BarlowCondensed-Medium.ttf', 34)
    said = 'AYN Thor 115-119 FPS   ·   Retroid Pocket Flip 2 about 60 FPS'
    wide = d.textlength(said, font=chip) / S + 36
    d.rounded_rectangle([70 * S, 500 * S, (70 + wide) * S, 556 * S], radius=10 * S, outline=accent, width=2 * S)
    d.text((88 * S, 507 * S), said, font=chip, fill=WHITE)

    d.text((74 * S, 584 * S), f'github.com/yassinsolim/{repo}   ·   bring your own disc   ·   no game files',
           font=font('BarlowSemiCondensed-Regular.ttf', 24), fill=GREY)

    handheld(d, 760, 190, 450, 250, accent)

    img = img.resize((W, H), Image.LANCZOS)
    img.save(out, optimize=True)


def glow_layer(accent):
    layer = Image.new('RGB', (W * S, H * S), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy, r = 120 * S, 720 * S, 420 * S
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=tuple(c // 4 for c in accent))
    return layer.filter(ImageFilter.GaussianBlur(110 * S))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'docs/media')
    os.makedirs(out, exist_ok=True)
    for short, name, full, repo, accent in GAMES:
        path = os.path.join(out, f'social-{short}.png')
        make(short, name, full, repo, accent, path)
        print(path)


if __name__ == '__main__':
    main()

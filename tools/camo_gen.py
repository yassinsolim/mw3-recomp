#!/usr/bin/env python3
"""Original procedural camo generator for the port's texture replacement.

Given a texture dump `<key>.png` (MW2_DUMP_TEXTURES) this writes a new picture
for that key, sized to the dump and optionally keeping the dump's alpha as the
mask. Every pixel is computed here from a seed: the dump's colours are never
copied and no third-party art is read, bundled or downloaded.

Styles
  carbon       woven carbon fibre (2/2 twill, anisotropic sheen)
  voronoi      scales: domed Voronoi cells with dark seams and a highlight
  marble       domain-warped noise with thin veins
  digital      blocky pixel camo in a 4-colour palette
  iridescent   thin-film interference colours over warped noise
  plasma       sum-of-sines plasma with a cosine palette
Animated (--frames N): marble, iridescent, plasma. The loop is seamless: frame N
would equal frame 0. Frames are written as <key>.0.png .. <key>.<N-1>.png plus
<key>.txt holding `fps=<number>`; put them in the game's replace/ folder.

    python tools/camo_gen.py dumps/0123456789abcdef.png --style carbon --out replace
    python tools/camo_gen.py dumps/0123456789abcdef.png --style digital --palette 1e2a1c,3a4a2a,6b7a4a,a89a6a
    python tools/camo_gen.py dumps/0123456789abcdef.png --style marble --keep-alpha
    python tools/camo_gen.py dumps/0123456789abcdef.png --style iridescent --frames 24 --fps 15
    python tools/camo_gen.py --key 0123456789abcdef --size 512x512 --style voronoi --seed 7
    python tools/camo_gen.py --self-test

Same seed, style and size give the same picture (on one numpy build; floating
point may differ in the last bit between builds). Requires Python 3, Pillow, numpy.
"""
import argparse
import math
import os
import re
import sys
import tempfile

import numpy as np
from PIL import Image

STATIC_STYLES = ("carbon", "voronoi", "marble", "digital", "iridescent", "plasma")
ANIMATED_STYLES = ("marble", "iridescent", "plasma")
TAU = 2.0 * math.pi


# ------------------------------------------------------------------ helpers ---
class Noise:
    """Seeded periodic value noise: one random lattice per octave, each rotated
    and offset so octaves do not align. Values lie in 0..1, mean 0.5."""

    def __init__(self, rng, octaves=8, size=256):
        self.mask = size - 1
        self.lat = rng.random((octaves, size, size)).astype(np.float32)
        self.off = (rng.random((octaves, 2)) * size).astype(np.float32)
        ang = rng.random(octaves) * TAU
        self.cs = [(float(math.cos(a)), float(math.sin(a))) for a in ang]

    def octave(self, o, x, y):
        c, s = self.cs[o]
        xr = x * c - y * s + self.off[o, 0]
        yr = x * s + y * c + self.off[o, 1]
        x0, y0 = np.floor(xr), np.floor(yr)
        fx, fy = xr - x0, yr - y0
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        ix, iy = x0.astype(np.int64), y0.astype(np.int64)
        ix1, iy1 = (ix + 1) & self.mask, (iy + 1) & self.mask
        ix, iy = ix & self.mask, iy & self.mask
        lat = self.lat[o]
        a, b = lat[iy, ix], lat[iy, ix1]
        c2, d = lat[iy1, ix], lat[iy1, ix1]
        top = a + (b - a) * fx
        return top + ((c2 + (d - c2) * fx) - top) * fy

    def fbm(self, x, y, octaves=5, lacunarity=2.0, gain=0.5):
        total, amp, norm, freq = 0.0, 1.0, 0.0, 1.0
        for o in range(octaves):
            total = total + amp * self.octave(o, x * freq, y * freq)
            norm += amp
            amp *= gain
            freq *= lacunarity
        return total / norm


def loop_mix(a, b, phase):
    """Two independent 0..1 fields blended around a circle: variance is the same
    at every phase and the result is periodic in `phase` (radians)."""
    return 0.5 + math.cos(phase) * (a - 0.5) + math.sin(phase) * (b - 0.5)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def hsv(h, s, v):
    """Scalars or arrays (h wraps) -> (..., 3) float32 rgb."""
    h = (np.asarray(h, np.float32) % 1.0) * 6.0
    s, v = np.asarray(s, np.float32), np.asarray(v, np.float32)
    i = np.floor(h)
    f = h - i
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    i = i.astype(np.int32) % 6
    r = np.choose(i, [v, q, p, p, t, v])
    g = np.choose(i, [t, v, v, q, p, p])
    b = np.choose(i, [p, p, t, v, v, q])
    return np.stack([r, g, b], axis=-1).astype(np.float32)


def gradient(t, stops):
    """Piecewise-linear colour ramp: t (0..1 array), stops (k,3) -> (..., 3)."""
    stops = np.asarray(stops, np.float32)
    xs = np.linspace(0.0, 1.0, len(stops))
    return np.stack([np.interp(t, xs, stops[:, c]) for c in range(3)], axis=-1).astype(np.float32)


def coords(w, h):
    """Pixel centres in units of the long side, so patterns keep their scale on any aspect."""
    m = float(max(w, h))
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return (x + 0.5) / m, (y + 0.5) / m


# ------------------------------------------------------------------- styles ---
def carbon(w, h, rng, hue, t):
    nz = Noise(rng, 4)
    px = max(max(w, h) / 32.0, 4.0)                           # one tow, in pixels
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = (x + 0.5) / px, (y + 0.5) / px
    i, j = np.floor(cx).astype(np.int64), np.floor(cy).astype(np.int64)
    fx, fy = cx - i, cy - j

    def horizontal_over(ii, jj):                              # 2/2 twill
        return ((ii - jj) % 4) < 2

    hov = horizontal_over(i, j)
    # Where a tow dives under, it darkens toward the end of its float.
    end_l = ~horizontal_over(i - 1, j)
    end_r = ~horizontal_over(i + 1, j)
    end_t = horizontal_over(i, j - 1)
    end_b = horizontal_over(i, j + 1)
    dark_h = np.where(end_l, smoothstep(0.0, 0.45, fx), 1.0) * np.where(end_r, smoothstep(0.0, 0.45, 1 - fx), 1.0)
    dark_v = np.where(end_t, smoothstep(0.0, 0.45, fy), 1.0) * np.where(end_b, smoothstep(0.0, 0.45, 1 - fy), 1.0)
    round_h = np.sin(np.pi * fy) ** 0.7                        # a tow is a cylinder
    round_v = np.sin(np.pi * fx) ** 0.7
    shade = np.where(hov, round_h * dark_h, round_v * dark_v)
    fibre_h = nz.fbm(cx * 1.0, cy * 22.0, 3)
    fibre_v = nz.fbm(cx * 22.0, cy * 1.0, 3)
    fibre = 0.82 + 0.35 * (np.where(hov, fibre_h, fibre_v) - 0.5)
    u, v = x / max(w, h), y / max(w, h)
    sheen = 0.5 + 0.5 * np.sin(TAU * (u + v) * 1.3 + 3.0 * nz.fbm(u * 3, v * 3, 3))
    sheen = np.where(hov, sheen, 1.0 - sheen) ** 2
    base = hsv(hue, 0.35, 1.0) if hue is not None else np.ones(3, np.float32)
    dark = np.array([0.075, 0.075, 0.085], np.float32) * (0.6 + 0.4 * base)
    light = np.array([0.50, 0.52, 0.58], np.float32) * (0.5 + 0.5 * base)
    k = (shade * fibre)[..., None]
    col = dark * (0.35 + 0.9 * k) + light * (sheen * shade * 0.55)[..., None]
    return col


def voronoi(w, h, rng, hue, t):
    n = 14.0 / max(w, h)                                      # cells per pixel
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    px, py = (x + 0.5) * n, (y + 0.5) * n * 1.15              # slightly taller cells
    ci, cj = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
    size = 256
    jit = rng.random((size, size, 2)).astype(np.float32)
    rnd = rng.random((size, size, 3)).astype(np.float32)
    d1 = np.full(px.shape, 9.0, np.float32)
    d2 = np.full(px.shape, 9.0, np.float32)
    best = np.zeros(px.shape + (2,), np.float32)               # offset to the nearest feature
    bi = np.zeros(px.shape, np.int64)
    bj = np.zeros(px.shape, np.int64)
    for dj in (-2, -1, 0, 1, 2):
        for di in (-1, 0, 1):
            gi, gj = ci + di, cj + dj
            jj = jit[gj & 255, gi & 255]
            fxp = gi + 0.5 * (gj & 1) + 0.15 + 0.7 * jj[..., 0] * 0.9
            fyp = gj + 0.15 + 0.7 * jj[..., 1] * 0.9
            ox, oy = px - fxp, py - fyp
            d = np.sqrt(ox * ox + oy * oy)
            nearer = d < d1
            second = ~nearer & (d < d2)
            d2 = np.where(nearer, d1, np.where(second, d, d2))
            best[..., 0] = np.where(nearer, ox, best[..., 0])
            best[..., 1] = np.where(nearer, oy, best[..., 1])
            bi = np.where(nearer, gi, bi)
            bj = np.where(nearer, gj, bj)
            d1 = np.where(nearer, d, d1)
    r = rnd[bj & 255, bi & 255]
    h0 = rng.random() if hue is None else hue
    gradient_across = (px / (w * n)) * 0.18 + (py / (h * n * 1.15)) * 0.08
    cell = hsv(h0 + gradient_across + 0.07 * (r[..., 0] - 0.5), 0.6 + 0.35 * r[..., 1],
               0.45 + 0.4 * r[..., 2])
    seam = smoothstep(0.0, 0.18, d2 - d1)                       # dark where cells meet
    dome = np.clip(1.0 - d1 / 0.75, 0.0, 1.0)
    lightdir = np.array([-0.55, -0.83], np.float32)
    spec = np.clip((best[..., 0] * lightdir[0] + best[..., 1] * lightdir[1]) * -1.4 + 0.15, 0.0, 1.0) * dome
    lit = (0.45 + 0.7 * dome)[..., None]
    return cell * lit * seam[..., None] + (spec ** 2 * 0.45 * seam)[..., None]


def marble(w, h, rng, hue, t):
    na, nb = Noise(rng, 6), Noise(rng, 6)
    dark = rng.random() < 0.4
    h0 = rng.random() if hue is None else hue
    u, v = coords(w, h)
    phase = TAU * t

    def f(x, y, octaves=5):
        return loop_mix(na.fbm(x, y, octaves), nb.fbm(x, y, octaves), phase)

    px, py = u * 3.0, v * 3.0
    w1x, w1y = f(px + 5.2, py + 1.3), f(px + 8.3, py + 2.8)
    qx, qy = px + 1.6 * (w1x - 0.5), py + 1.6 * (w1y - 0.5)
    w2x, w2y = f(qx * 1.3 + 1.7, qy * 1.3 + 9.2), f(qx * 1.3 + 8.3, qy * 1.3 + 2.8)
    rx, ry = qx + 1.2 * (w2x - 0.5), qy + 1.2 * (w2y - 0.5)
    val = f(rx, ry, 6)
    vein = (1.0 - np.abs(np.sin((rx * 0.8 + ry * 0.6 + 2.2 * (val - 0.5)) * 3.0 * np.pi))) ** 7
    fine = (1.0 - np.abs(np.sin((rx * 0.5 - ry * 0.9 + 5.0 * (val - 0.5)) * 9.0 * np.pi))) ** 14
    veins = np.clip(vein + 0.45 * fine, 0.0, 1.0)[..., None]
    if dark:
        ramp = [hsv(h0, 0.25, 0.05), hsv(h0, 0.3, 0.12), hsv(h0, 0.2, 0.22)]
        vein_col = hsv(h0 + 0.08, 0.5, 0.95)
        return gradient(val, ramp) * (1 - veins) + vein_col * veins * 0.85
    ramp = [hsv(h0, 0.06, 0.78), hsv(h0, 0.05, 0.90), hsv(h0, 0.03, 0.97)]
    vein_col = hsv(h0 + 0.05, 0.35, 0.22)
    return gradient(val, ramp) * (1 - 0.8 * veins) + vein_col * veins * 0.3


def digital(w, h, rng, hue, t, palette=None):
    if palette is None:
        h0 = rng.random() if hue is None else hue
        palette = [hsv(h0, 0.55, 0.20), hsv(h0 + 0.02, 0.45, 0.36), hsv(h0 - 0.04, 0.35, 0.55),
                   hsv(h0 + 0.09, 0.30, 0.74)]
    palette = np.asarray(palette, np.float32)
    cell = max(2, int(round(max(w, h) / 48.0)))
    gw, gh = -(-w // cell), -(-h // cell)
    nz = Noise(rng, 4)
    gy, gx = np.mgrid[0:gh, 0:gw].astype(np.float32)
    v = nz.fbm(gx * 0.11, gy * 0.11, 3) + 0.22 * rng.random((gh, gw)).astype(np.float32)
    cuts = np.quantile(v, [0.32, 0.62, 0.86])
    idx = np.searchsorted(cuts, v)
    for _ in range(max(4, gw * gh // 45)):                        # small off-grid blocks
        bw, bh = (2, 1) if rng.random() < 0.5 else (1, 2)
        x0, y0 = int(rng.integers(0, gw)), int(rng.integers(0, gh))
        idx[y0:y0 + bh, x0:x0 + bw] = int(rng.choice([0, 3]))
    ys = np.minimum(np.arange(h) // cell, gh - 1)
    xs = np.minimum(np.arange(w) // cell, gw - 1)
    return palette[idx[ys][:, xs]]


def iridescent(w, h, rng, hue, t):
    na, nb = Noise(rng, 6), Noise(rng, 6)
    u, v = coords(w, h)
    phase = TAU * t
    flow = loop_mix(na.fbm(u * 2.2, v * 2.2, 5), nb.fbm(u * 2.2, v * 2.2, 5), phase)
    tilt = u * 0.7 + v * 0.7 + 0.18 * math.sin(phase)
    thickness = 250.0 + 650.0 * (0.55 * flow + 0.45 * tilt)          # nanometres
    cos_t = 1.0 - 0.3 * smoothstep(0.0, 1.0, loop_mix(na.fbm(u + 3, v + 3, 3), nb.fbm(u + 3, v + 3, 3), phase))
    n_film = 1.33
    cols = []
    for lo, hi in ((590.0, 690.0), (500.0, 590.0), (410.0, 500.0)):    # red, green, blue bands
        acc = 0.0
        for wavelength in np.linspace(lo, hi, 3):
            acc = acc + 0.5 - 0.5 * np.cos(TAU * 2.0 * n_film * thickness * cos_t / wavelength)
        cols.append(acc / 3.0)
    film = np.stack(cols, axis=-1).astype(np.float32)
    grey = film.mean(-1, keepdims=True)
    film = np.clip(grey + 1.5 * (film - grey), 0.0, 1.0)               # the band average pales; bring the hue back
    return 0.04 + 0.92 * film ** 1.3


def plasma(w, h, rng, hue, t):
    u, v = coords(w, h)
    phase = TAU * t
    a = 2.0 + 6.0 * rng.random(4)
    o = rng.random(4) * TAU
    cx, cy = rng.random(2) * 0.6 + 0.2
    r = np.hypot(u - cx, v - cy)
    val = (np.sin(u * a[0] * TAU * 0.5 + o[0] + phase) + np.sin(v * a[1] * TAU * 0.5 + o[1] + 2 * phase)
           + np.sin((u + v) * a[2] * TAU * 0.35 + o[2] + phase)
           + np.sin(r * a[3] * TAU - 2 * phase + o[3])) * 0.25
    h0 = rng.random() if hue is None else hue
    k = val * 0.5 + 0.5
    ang = TAU * (k[..., None] * 1.2 + h0 + np.array([0.0, 0.33, 0.67], np.float32))
    return (0.5 + 0.5 * np.cos(ang)).astype(np.float32) * (0.35 + 0.65 * k[..., None])


def render(style, w, h, seed=0, t=0.0, hue=None, palette=None):
    """One picture as (h, w, 3) uint8. `t` in [0, 1) is the loop position of the
    animated styles; the others ignore it."""
    if style not in STATIC_STYLES:
        raise ValueError("unknown style %r (one of %s)" % (style, ", ".join(STATIC_STYLES)))
    rng = np.random.default_rng(seed)
    if style == "digital":
        img = digital(w, h, rng, hue, t, palette)
    else:
        img = {"carbon": carbon, "voronoi": voronoi, "marble": marble, "iridescent": iridescent,
               "plasma": plasma}[style](w, h, rng, hue, t)
    return (np.clip(img, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


# ----------------------------------------------------------------------- CLI ---
def parse_palette(text):
    parts = [p.strip().lstrip("#") for p in text.split(",")]
    if len(parts) != 4 or not all(re.fullmatch(r"[0-9a-fA-F]{6}", p) for p in parts):
        raise argparse.ArgumentTypeError("--palette wants four rrggbb colours, comma separated")
    return [[int(p[i:i + 2], 16) / 255.0 for i in (0, 2, 4)] for p in parts]


def parse_size(text):
    m = re.fullmatch(r"(\d+)x(\d+)", text)
    if not m or int(m.group(1)) < 4 or int(m.group(2)) < 4:
        raise argparse.ArgumentTypeError("--size wants WIDTHxHEIGHT, at least 4x4")
    return int(m.group(1)), int(m.group(2))


def fit(w, h, limit):
    if max(w, h) <= limit:
        return w, h
    s = limit / max(w, h)
    return max(4, round(w * s)), max(4, round(h * s))


def generate(style, key, w, h, out, seed=0, mask=None, hue=None, palette=None, frames=0, fps=12.0):
    """Write the picture or animation for `key` into `out`; returns the files written.
    `mask` is an (h, w) uint8 alpha, or None for opaque."""
    os.makedirs(out, exist_ok=True)

    def save(path, rgb):
        alpha = np.full((h, w), 255, np.uint8) if mask is None else mask
        Image.fromarray(np.dstack([rgb, alpha])).save(path)
        return path

    if frames:
        if style not in ANIMATED_STYLES:
            raise ValueError("%s has no animated form (use %s)" % (style, ", ".join(ANIMATED_STYLES)))
        if frames < 2:
            raise ValueError("an animation needs at least 2 frames")
        paths = [save(os.path.join(out, "%s.%d.png" % (key, n)),
                      render(style, w, h, seed, n / frames, hue, palette)) for n in range(frames)]
        txt = os.path.join(out, key + ".txt")
        with open(txt, "w") as f:
            f.write("fps=%g\n" % fps)
        return paths + [txt]
    return [save(os.path.join(out, key + ".png"), render(style, w, h, seed, 0.0, hue, palette))]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Original procedural camo for a texture dump key. Styles: " + ", ".join(STATIC_STYLES) +
                    ". Animated (--frames): " + ", ".join(ANIMATED_STYLES) + ".")
    ap.add_argument("dump", nargs="?", help="a dump <key>.png: gives the key, the size and (--keep-alpha) the alpha")
    ap.add_argument("--style", choices=STATIC_STYLES, help="camo style")
    ap.add_argument("--out", default="replace", metavar="DIR", help="output folder (default: replace)")
    ap.add_argument("--seed", type=int, default=0, help="random seed (default 0); same seed, same picture")
    ap.add_argument("--key", help="texture key to write for (default: the dump's file name)")
    ap.add_argument("--size", type=parse_size, metavar="WxH", help="output size (default: the dump's)")
    ap.add_argument("--max-dim", type=int, metavar="N",
                    help="longest side cap for a size taken from the dump (default 1024; 256 when animated)")
    ap.add_argument("--keep-alpha", action="store_true", help="use the dump's alpha channel as the output's alpha")
    ap.add_argument("--hue", type=float, metavar="0..1", help="base hue for carbon tint, voronoi, marble, plasma, digital")
    ap.add_argument("--palette", type=parse_palette, metavar="RRGGBB,x4", help="digital: the four colours, dark to light")
    ap.add_argument("--frames", type=int, default=0, metavar="N", help="animate: write N looping frames")
    ap.add_argument("--fps", type=float, default=12.0, help="animation speed written to <key>.txt (default 12)")
    ap.add_argument("--self-test", action="store_true", help="render every style to a temp folder and check it")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.style:
        ap.error("--style is required")
    key, mask, size = args.key, None, args.size
    if args.dump:
        with Image.open(args.dump) as im:
            dw, dh = im.size
            alpha = np.asarray(im.convert("RGBA"))[..., 3] if args.keep_alpha else None
        key = key or os.path.splitext(os.path.basename(args.dump))[0]
        if size is None:
            size = fit(dw, dh, args.max_dim or (256 if args.frames else 1024))
        if alpha is not None:
            mask = np.asarray(Image.fromarray(alpha).resize(size, Image.BILINEAR)) if size != (dw, dh) else alpha
            if mask.min() == 255:
                mask = None
    if not key or not size:
        ap.error("give a dump, or --key and --size")
    if not re.fullmatch(r"[0-9a-fA-F]{1,16}", key):
        ap.error("key %r is not 1-16 hex digits" % key)
    w, h = size
    if args.frames and args.frames * w * h * 4 * 4 // 3 > 32 << 20:
        print("warning: %d frames of %dx%d need about %d MB decoded; the Android build keeps 32 MB and drops "
              "the rest" % (args.frames, w, h, args.frames * w * h * 16 // 3 >> 20), file=sys.stderr)
    stale = os.path.join(args.out, key + (".png" if args.frames else ".0.png"))
    if os.path.exists(stale):
        print("warning: %s exists and would compete with what is written now" % stale, file=sys.stderr)
    for path in generate(args.style, key, w, h, args.out, args.seed, mask, args.hue, args.palette,
                         args.frames, args.fps):
        print(path)
    return 0


# ----------------------------------------------------------------- self-test ---
def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise AssertionError(what)
        checks += 1

    w, h = 96, 64
    summary = []
    with tempfile.TemporaryDirectory() as tmp:
        for style in STATIC_STYLES:
            a = render(style, w, h, seed=3)
            check(a.shape == (h, w, 3) and a.dtype == np.uint8, style + " shape")
            check(np.array_equal(a, render(style, w, h, seed=3)), style + " deterministic")
            check(not np.array_equal(a, render(style, w, h, seed=4)), style + " seed matters")
            sd = float(a.astype(np.float32).std())
            check(sd > 8.0, "%s is not flat (std %.1f)" % (style, sd))
            check(len(np.unique(a.reshape(-1, 3), axis=0)) >= 4, style + " has colours")
            key = "00c0ffee00000001"
            (path,) = generate(style, key, w, h, tmp, seed=3)
            im = Image.open(path)
            check(im.size == (w, h) and im.mode == "RGBA" and np.asarray(im)[..., 3].min() == 255,
                  style + " png opaque RGBA at the dump's size")
            summary.append("%s std %.0f" % (style, sd))
        d = render("digital", w, h, seed=3)
        check(len(np.unique(d.reshape(-1, 3), axis=0)) == 4, "digital uses exactly its 4 colours")
        pal = parse_palette("112233,445566,778899,aabbcc")
        d2 = render("digital", w, h, seed=3, palette=pal)
        check({tuple(p) for p in np.unique(d2.reshape(-1, 3), axis=0)} <=
              {(17, 34, 51), (68, 85, 102), (119, 136, 153), (170, 187, 204)}, "digital palette honoured")

        # alpha mask from a (synthetic) dump through the CLI
        dump = os.path.join(tmp, "dump", "00c0ffee00000002.png")
        os.makedirs(os.path.dirname(dump))
        al = np.zeros((h, w), np.uint8)
        al[:, : w // 2] = 255
        al[10:20, w // 2:] = 128
        Image.fromarray(np.dstack([np.full((h, w, 3), 200, np.uint8), al])).save(dump)
        out = os.path.join(tmp, "replace")
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            main([dump, "--style", "voronoi", "--seed", "5", "--keep-alpha", "--out", out])
        got = np.asarray(Image.open(os.path.join(out, "00c0ffee00000002.png")))
        check(np.array_equal(got[..., 3], al), "dump alpha kept as the mask")
        check(abs(float(got[..., :3].mean()) - 200.0) > 10.0, "dump colours not copied")
        with contextlib.redirect_stdout(io.StringIO()):
            main([dump, "--style", "carbon", "--out", out, "--max-dim", "32"])
        check(Image.open(os.path.join(out, "00c0ffee00000002.png")).size == (32, 21), "max-dim keeps aspect")

        # animations: N frames, fps file, frames differ, loop closes
        for style in ANIMATED_STYLES:
            files = generate(style, "00c0ffee00000003", w, h, tmp, seed=9, frames=6, fps=15)
            names = sorted(os.path.basename(f) for f in files)
            check(names == sorted(["00c0ffee00000003.%d.png" % n for n in range(6)] + ["00c0ffee00000003.txt"]),
                  style + " frame names")
            check(open(os.path.join(tmp, "00c0ffee00000003.txt")).read().strip() == "fps=15", style + " fps line")
            fr = [np.asarray(Image.open(f)) for f in files if f.endswith(".png")]
            check(all(f.shape == (h, w, 4) for f in fr), style + " frame sizes")
            diffs = [float(np.abs(fr[n].astype(int) - fr[(n + 1) % 6].astype(int)).mean()) for n in range(6)]
            check(min(diffs) > 0.4, "%s frames move (min step %.2f)" % (style, min(diffs)))
            wrap = render(style, w, h, seed=9, t=1.0).astype(int)
            first = render(style, w, h, seed=9, t=0.0).astype(int)
            check(np.abs(wrap - first).max() <= 2, style + " loops seamlessly")
            step = float(np.abs(render(style, w, h, seed=9, t=1 / 6).astype(int) - first).mean())
            check(float(np.abs(wrap - first).mean()) < step / 4, style + " loop closes tighter than a frame step")
        try:
            generate("carbon", "1", w, h, tmp, frames=4)
            check(False, "carbon animation refused")
        except ValueError:
            check(True, "static-only style refuses --frames")
    print("camo_gen self-test: %d checks passed (%s)" % (checks, "; ".join(summary)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

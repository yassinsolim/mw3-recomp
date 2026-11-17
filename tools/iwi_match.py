#!/usr/bin/env python3
"""Match this port's texture dumps to the user's own PC .iwi files, and emit the
user's modded images under the dumps' keys (docs/textures.md, "Making skins").

Everything here works on files the USER supplies from their own PC copy of the
game (and mods they chose to install). Nothing is bundled or downloaded, and
nothing from a dump is ever copied to the output: the output holds only the
modded image taken from the user's own modded .iwi.

Inputs
  --stock  DIR...   the user's stock IW5 images: loose .iwi and/or .iwd (zip)
  --dumps  DIR      MW2_DUMP_TEXTURES output: <key>.png + textures.txt
  --mod    DIR...   the user's MODDED .iwi/.iwd, with the SAME NAMES as the
                    stock files they replace

Method (see matching notes in `--help`): every dump and every stock image is
reduced to a 32x32 luma thumbnail plus 8x8 colour and alpha thumbnails; a dump
and an image are compared by normalised correlation of those, gated by aspect
ratio. The best stock name for each dump is its original; the modded image of
that name is written to <out>/<key>.png when --apply is given.

    python tools/iwi_match.py --stock "D:/MW3/main" --dumps dumps --mod mods
    python tools/iwi_match.py --stock "D:/MW3/main" --dumps dumps --mod mods --apply --out replace
    python tools/iwi_match.py --self-test

Requires Python 3, Pillow and numpy only.
"""
import argparse
import os
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from collections import Counter

import numpy as np
from PIL import Image

# ---------------------------------------------------------------- IWI v8 ---
# 'IWi', u8 version, then (v8, IW4/IW5): u32 flags, u8 format, u8 unused,
# u16 dims[3] (width, height, depth), u32 fileSizeForPicmip[4]: 32 bytes.
# Mip levels follow, smallest first; the largest level is last.
HEADER = 32
F_BGRA, F_BGR, F_LA, F_L, F_A = 1, 2, 3, 4, 5
F_DXT1, F_DXT3, F_DXT5, F_DXN = 0xB, 0xC, 0xD, 0xE
FLAG_NOMIPS = 0x2
FLAG_CUBE = 0x10000
FLAG_VOLUME = 0x20000
FORMAT_NAMES = {1: "BGRA", 2: "BGR", 3: "LA", 4: "L", 5: "A", 0xB: "DXT1",
                0xC: "DXT3", 0xD: "DXT5", 0xE: "DXN"}
BYTES_PER_PIXEL = {F_BGRA: 4, F_BGR: 3, F_LA: 2, F_L: 1, F_A: 1}
BLOCK_BYTES = {F_DXT1: 8, F_DXT3: 16, F_DXT5: 16}


class IwiError(Exception):
    pass


class Iwi:
    """A parsed image: `levels` is [(w, h, offset, size)] smallest first."""

    def __init__(self, data, version, flags, fmt, width, height, depth, levels):
        self.data = data
        self.version = version
        self.flags = flags
        self.fmt = fmt
        self.width = width
        self.height = height
        self.depth = depth
        self.levels = levels

    @property
    def flat(self):
        return not (self.flags & (FLAG_CUBE | FLAG_VOLUME)) and self.depth <= 1


def level_size(fmt, w, h):
    if fmt in BLOCK_BYTES:
        return ((w + 3) // 4) * ((h + 3) // 4) * BLOCK_BYTES[fmt]
    return w * h * BYTES_PER_PIXEL[fmt]


def parse_iwi(data):
    if len(data) < HEADER or data[:3] != b"IWi":
        raise IwiError("not an IWi file")
    version = data[3]
    if version != 8:
        raise IwiError("IWi version %d (only v8, IW4/IW5, is read)" % version)
    flags, fmt, _unused, w, h, d = struct.unpack_from("<IBBHHH", data, 4)
    if fmt not in BYTES_PER_PIXEL and fmt not in BLOCK_BYTES:
        name = FORMAT_NAMES.get(fmt) or ("wavelet" if 6 <= fmt <= 10 else "0x%X" % fmt)
        raise IwiError("format %s is not decoded" % name)
    if not w or not h:
        raise IwiError("zero-sized image")
    chains = []
    full, x, y = [(w, h)], w, h
    while x > 1 or y > 1:
        x, y = max(1, x >> 1), max(1, y >> 1)
        full.append((x, y))
    if flags & FLAG_NOMIPS:
        chains.append([(w, h)])
    chains.append(full)
    if not flags & FLAG_NOMIPS:
        chains.append([(w, h)])
    body = len(data) - HEADER
    for chain in chains:
        sizes = [level_size(fmt, cw, ch) for cw, ch in chain]
        if sum(sizes) != body:
            continue
        levels, offset = [], HEADER
        for (cw, ch), size in zip(reversed(chain), reversed(sizes)):
            levels.append((cw, ch, offset, size))
            offset += size
        return Iwi(data, version, flags, fmt, w, h, d, levels)
    # Trimmed or padded: the largest level is the last bytes of the file.
    base = level_size(fmt, w, h)
    if body >= base:
        return Iwi(data, version, flags, fmt, w, h, d, [(w, h, len(data) - base, base)])
    raise IwiError("file is %d bytes, too short for a %dx%d image" % (len(data), w, h))


def _expand565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def decode_dxt(buf, w, h, fmt):
    """DXT1/3/5 -> (h, w, 4) uint8 RGBA, in numpy."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    stride = BLOCK_BYTES[fmt]
    n = bw * bh
    raw = np.frombuffer(buf, np.uint8, n * stride).reshape(n, stride)
    cb = raw[:, stride - 8:].astype(np.int32)
    c0 = cb[:, 0] | (cb[:, 1] << 8)
    c1 = cb[:, 2] | (cb[:, 3] << 8)
    p0, p1 = _expand565(c0), _expand565(c1)
    pal = np.zeros((n, 4, 4), np.int32)
    pal[:, :, 3] = 255
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    four = np.ones(n, bool) if fmt != F_DXT1 else c0 > c1
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal[:, 3, 3] = np.where(four, 255, 0)
    bits = cb[:, 4] | (cb[:, 5] << 8) | (cb[:, 6] << 16) | (cb[:, 7] << 24)
    idx = (bits[:, None] >> (2 * np.arange(16))) & 3
    pix = pal[np.arange(n)[:, None], idx]                      # (n, 16, 4)
    if fmt == F_DXT3:
        a = raw[:, :8].astype(np.int32)
        nib = np.stack([a & 15, a >> 4], axis=-1).reshape(n, 16)
        pix[:, :, 3] = nib * 17
    elif fmt == F_DXT5:
        a0, a1 = raw[:, 0].astype(np.int32), raw[:, 1].astype(np.int32)
        tab = np.zeros((n, 8), np.int32)
        tab[:, 0], tab[:, 1] = a0, a1
        big = a0 > a1
        for k in range(1, 7):
            hi = ((7 - k) * a0 + k * a1) // 7
            lo = ((5 - k) * a0 + k * a1) // 5 if k <= 4 else (0 if k == 5 else 255)
            tab[:, k + 1] = np.where(big, hi, lo)
        ib = raw[:, 2:8].astype(np.uint64)
        v = np.zeros(n, np.uint64)
        for i in range(6):
            v |= ib[:, i] << np.uint64(8 * i)
        ai = ((v[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
        pix[:, :, 3] = tab[np.arange(n)[:, None], ai]
    img = pix.astype(np.uint8).reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4)
    return np.ascontiguousarray(img.reshape(bh * 4, bw * 4, 4)[:h, :w])


def decode_level(iwi, index):
    """One mip level as (h, w, 4) uint8 RGBA.

    One-channel formats come out grey with alpha 255 (how the port's dump shows
    them); LA is grey with its alpha.
    """
    w, h, offset, size = iwi.levels[index]
    buf = memoryview(iwi.data)[offset:offset + size]
    fmt = iwi.fmt
    if fmt in BLOCK_BYTES:
        return decode_dxt(buf, w, h, fmt)
    bpp = BYTES_PER_PIXEL[fmt]
    a = np.frombuffer(buf, np.uint8, w * h * bpp).reshape(h, w, bpp)
    out = np.empty((h, w, 4), np.uint8)
    out[..., 3] = 255
    if fmt == F_BGRA:
        out[...] = a[..., [2, 1, 0, 3]]
    elif fmt == F_BGR:
        out[..., :3] = a[..., ::-1]
    elif fmt == F_LA:
        out[..., :3] = a[..., 0:1]
        out[..., 3] = a[..., 1]
    else:
        out[..., :3] = a[..., 0:1]
    return out


# ------------------------------------------------------------ file sources ---
def iter_iwi(paths):
    """Yield (name, reader) for every .iwi in the folders/files/.iwd given.

    A name is the lower-case file name without extension. Later sources replace
    earlier ones with the same name (as the game's search order does).
    """
    found = {}
    zips = []

    def from_zip(path):
        try:
            z = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError) as e:
            print("warning: %s: %s" % (path, e), file=sys.stderr)
            return
        zips.append(z)
        for member in z.namelist():
            if member.lower().endswith(".iwi"):
                name = os.path.splitext(os.path.basename(member))[0].lower()
                found[name] = (lambda z=z, m=member: z.read(m))

    for root in paths:
        if os.path.isfile(root):
            walk = [(os.path.dirname(root), [], [os.path.basename(root)])]
        else:
            walk = sorted(os.walk(root), key=lambda t: t[0])
        for d, _dirs, files in walk:
            for f in sorted(files):
                full = os.path.join(d, f)
                low = f.lower()
                if low.endswith(".iwi"):
                    found[os.path.splitext(low)[0]] = (lambda p=full: open(p, "rb").read())
                elif low.endswith(".iwd") or low.endswith(".zip"):
                    from_zip(full)
    return found


# ---------------------------------------------------------------- features ---
THUMB = 32        # luma thumbnail
CTHUMB = 8        # colour and alpha thumbnails
WEIGHTS = (0.55, 0.30, 0.15)
FLAT_RMS = 1.5    # below this a mean-removed thumbnail counts as flat (0..255)
ASPECT_TOLERANCE = 0.12   # log2 units
CLOSE_SPAN = 48.0         # mean difference at which two flat pictures stop agreeing


def thumbnail_level(iwi):
    """Index of the smallest level with at least 64x64 texels, else the largest."""
    for i, (w, h, _o, _s) in enumerate(iwi.levels):
        if w * h >= 4096:
            return i
    return len(iwi.levels) - 1


def _resize(a, size):
    return np.asarray(Image.fromarray(a).resize(size, Image.BILINEAR), np.float32)


def features(rgba):
    """Thumbnails of an (h, w, 4) uint8 image, as a dict of 1-D float32 arrays."""
    h, w = rgba.shape[:2]
    rgb = _resize(np.ascontiguousarray(rgba[..., :3]), (THUMB, THUMB))
    alpha = _resize(np.ascontiguousarray(rgba[..., 3]), (THUMB, THUMB))
    luma = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    k = THUMB // CTHUMB
    rgb8 = rgb.reshape(CTHUMB, k, CTHUMB, k, 3).mean((1, 3))
    alpha8 = alpha.reshape(CTHUMB, k, CTHUMB, k).mean((1, 3))
    return {
        "L": luma.ravel(), "C": rgb8.ravel(), "A": alpha8.ravel(),
        "mean": np.array([luma.mean(), *rgb8.mean((0, 1)), alpha8.mean()], np.float32),
        "aspect": np.float32(np.log2(w / h)),
    }


class FeatureSet:
    def __init__(self, items):
        n = len(items)
        self.n = n

        def unit(key):
            m = np.stack([i[key] for i in items]).astype(np.float32) if n else np.zeros((0, 1), np.float32)
            m = m - m.mean(1, keepdims=True)
            rms = np.sqrt((m * m).mean(1))
            flat = rms < FLAT_RMS
            norm = np.sqrt((m * m).sum(1))
            m = m / np.where(norm > 0, norm, 1)[:, None]
            m[flat] = 0
            return m.astype(np.float32), flat

        self.L, self.fL = unit("L")
        self.C, self.fC = unit("C")
        self.A, self.fA = unit("A")
        self.mean = np.stack([i["mean"] for i in items]) if n else np.zeros((0, 5), np.float32)
        self.aspect = np.array([i["aspect"] for i in items], np.float32)


def score_matrix(d, i, rows):
    """Similarity of dumps `rows` (slice) to every image, shape (len(rows), i.n), 0..1."""
    wl, wc, wa = WEIGHTS
    mean_d, mean_i = d.mean[rows], i.mean

    def close(diff):
        return np.clip(1.0 - diff / CLOSE_SPAN, 0.0, 1.0)

    sl = d.L[rows] @ i.L.T
    both = d.fL[rows][:, None] & i.fL[None, :]
    sl = np.where(both, close(np.abs(mean_d[:, 0:1] - mean_i[None, :, 0])), sl)

    sc = d.C[rows] @ i.C.T
    both = d.fC[rows][:, None] & i.fC[None, :]
    dist = sum(np.abs(mean_d[:, k:k + 1] - mean_i[None, :, k]) for k in (1, 2, 3)) / 3.0
    sc = np.where(both, close(dist), sc)

    sa = d.A[rows] @ i.A.T
    both = d.fA[rows][:, None] & i.fA[None, :]
    adiff = np.abs(mean_d[:, 4:5] - mean_i[None, :, 4])
    sa = np.where(both, close(adiff), sa)
    include = ~(both & (adiff < 8.0))          # two opaque pictures say nothing in alpha

    num = wl * sl + wc * sc + wa * np.where(include, sa, 0.0)
    den = wl + wc + wa * include
    score = num / den
    score[np.abs(d.aspect[rows][:, None] - i.aspect[None, :]) > ASPECT_TOLERANCE] = -1.0
    return score


# ------------------------------------------------------------------- dumps ---
LINE = re.compile(r"^([0-9a-f]{16}) (\S+) (\d+)x(\d+), (\d+) levels(.*)$")
ONE_CHANNEL = {"8", "DXT3A", "DXT5A"}


def read_dump_list(folder):
    """{key: info} from textures.txt, plus every <key>.png lacking a line."""
    info = {}
    listing = os.path.join(folder, "textures.txt")
    if os.path.isfile(listing):
        with open(listing, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = LINE.match(line.strip())
                if m:
                    tail = m.group(6)
                    info[m.group(1)] = {
                        "format": m.group(2), "w": int(m.group(3)), "h": int(m.group(4)),
                        "levels": int(m.group(5)), "gamma": "gamma" in tail,
                        "skip": ("not replaced" in tail) or ("no picture" in tail),
                    }
    for f in os.listdir(folder):
        m = re.fullmatch(r"([0-9a-f]{16})\.png", f)
        if m and m.group(1) not in info:
            info[m.group(1)] = {"format": None, "skip": False}
    return info


# ---------------------------------------------------------------- matching ---
def build_stock(stock_paths, log):
    sources = iter_iwi(stock_paths)
    names, items, meta, errors = [], [], {}, Counter()
    total = len(sources)
    for n, (name, reader) in enumerate(sorted(sources.items())):
        if n and n % 1000 == 0:
            log("  stock %d/%d" % (n, total))
        try:
            data = reader()
            iwi = parse_iwi(data)
            if not iwi.flat:
                raise IwiError("cube or volume")
            lv = thumbnail_level(iwi)
            items.append(features(decode_level(iwi, lv)))
        except IwiError as e:
            errors[str(e)] += 1
            continue
        names.append(name)
        meta[name] = {"w": iwi.width, "h": iwi.height, "fmt": FORMAT_NAMES[iwi.fmt],
                      "sig": (len(data), zlib.crc32(data))}
    return names, FeatureSet(items), meta, errors


def match_dumps(dump_folder, names, stock, log, top=4):
    info = read_dump_list(dump_folder)
    keys, items = [], []
    skipped = Counter()
    for key in sorted(info):
        if info[key]["skip"]:
            skipped["not a flat decodable dump"] += 1
            continue
        path = os.path.join(dump_folder, key + ".png")
        if not os.path.isfile(path):
            skipped["no png"] += 1
            continue
        try:
            with Image.open(path) as im:
                rgba = np.asarray(im.convert("RGBA"))
        except Exception as e:                     # noqa: BLE001 - any decode failure
            skipped["unreadable png: %s" % type(e).__name__] += 1
            continue
        items.append(features(rgba))
        keys.append(key)
        info[key]["w"], info[key]["h"] = rgba.shape[1], rgba.shape[0]
    dumps = FeatureSet(items)
    results = []
    k = min(top, stock.n)
    for start in range(0, dumps.n, 128):
        rows = slice(start, min(start + 128, dumps.n))
        if k:
            s = score_matrix(dumps, stock, rows)
            part = np.argpartition(-s, k - 1, axis=1)[:, :k]
        for r in range(rows.stop - rows.start):
            cands = []
            if k:
                order = sorted(part[r], key=lambda j: -s[r, j])
                cands = [(names[j], float(s[r, j])) for j in order if s[r, j] >= 0]
            results.append({"key": keys[start + r], "info": info[keys[start + r]], "cands": cands})
    return results, skipped


def decide(results, meta, mods, min_score, min_margin, allow_ambiguous):
    """Set r['status'], r['name'], r['score'], r['margin'] on each result."""
    def sig(name):
        mod = mods.get(name)
        if mod is None:
            return None
        return None if mod[1] == meta[name]["sig"] else mod[1]

    for r in results:
        c = r["cands"]
        if not c:
            r.update(status="no-candidate", name=None, score=0.0, margin=0.0)
            continue
        name, score = c[0]
        margin = score - c[1][1] if len(c) > 1 else 1.0
        r.update(name=name, score=score, margin=margin)
        if score < min_score:
            r["status"] = "weak"
            continue
        near = [n for n, s in c if score - s < min_margin]
        if len(near) > 1 and len({sig(n) for n in near}) > 1 and not allow_ambiguous:
            r["status"] = "ambiguous"
            r["near"] = near
        elif sig(name) is None:
            r["status"] = "unchanged"
        else:
            r["status"] = "replace"


def adapt(rgba, dump_format):
    """The modded picture, shaped as the dump shows the texture."""
    if dump_format in ONE_CHANNEL:
        grey = (rgba[..., :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32))
        g = np.clip(grey + 0.5, 0, 255).astype(np.uint8)
        out = np.empty_like(rgba)
        out[..., :3] = g[..., None]
        out[..., 3] = 255
        return out
    return rgba


def write_replacements(results, mod_readers, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    written = 0
    for r in results:
        if r["status"] != "replace":
            continue
        try:
            iwi = parse_iwi(mod_readers[r["name"]]())
            if not iwi.flat:
                raise IwiError("modded image is a cube or volume")
            rgba = adapt(decode_level(iwi, len(iwi.levels) - 1), r["info"].get("format"))
        except IwiError as e:
            r["status"] = "mod-error"
            r["why"] = str(e)
            continue
        Image.fromarray(np.ascontiguousarray(rgba)).save(os.path.join(out_dir, r["key"] + ".png"))
        r["status"] = "written"
        written += 1
    return written


def run(args):
    log = lambda m: print(m, file=sys.stderr)           # noqa: E731
    log("reading stock images ...")
    names, stock, meta, errors = build_stock(args.stock, log)
    if not names:
        sys.exit("no readable .iwi under --stock (%s)" % (dict(errors) or "none found"))
    mods, mod_readers = {}, {}
    if args.mod:
        for name, reader in iter_iwi(args.mod).items():
            data = reader()
            mods[name] = (name, (len(data), zlib.crc32(data)))
            mod_readers[name] = (lambda d=data: d)
        mods = {n: m for n, m in mods.items() if n in meta}
        log("%d modded images, %d of them with a stock original" % (len(mod_readers), len(mods)))
    log("%d stock images; matching dumps ..." % len(names))
    results, skipped = match_dumps(args.dumps, names, stock, log)
    decide(results, meta, mods, args.min_score, args.min_margin, args.allow_ambiguous)
    written = 0
    if args.apply:
        written = write_replacements(results, mod_readers, args.out)

    results.sort(key=lambda r: -r["score"])
    lines = []
    for r in results:
        i = meta.get(r["name"]) if r["name"] else None
        lines.append("%s  %.3f  %+.3f  %-11s %-40s %s -> %s" % (
            r["key"], r["score"], r["margin"], r["status"], r["name"] or "-",
            "%sx%s" % (r["info"].get("w"), r["info"].get("h")),
            "%dx%d %s" % (i["w"], i["h"], i["fmt"]) if i else "-"))
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    for r, line in zip(results, lines):
        if args.verbose or r["status"] not in ("weak", "no-candidate"):
            print(line)
    count = Counter(r["status"] for r in results)
    print("\n%d dumps: %s" % (len(results), ", ".join("%d %s" % (v, k) for k, v in sorted(count.items()))))
    for why, n in {**errors, **skipped}.items():
        print("  skipped %d: %s" % (n, why))
    if args.apply:
        print("wrote %d replacement(s) to %s" % (written, args.out))
    elif count["replace"]:
        print("%d replacement(s) ready; run again with --apply to write them to %s" % (count["replace"], args.out))
    return 0


# --------------------------------------------------------------- self-test ---
def _make_iwi(rgba, fmt, flags=0, mips=True):
    """Test-only IWI v8 writer (a simple DXT encoder; not for production use)."""
    h, w = rgba.shape[:2]
    chain = [rgba]
    while mips and (chain[-1].shape[0] > 1 or chain[-1].shape[1] > 1):
        ph, pw = chain[-1].shape[:2]
        size = (max(1, pw // 2), max(1, ph // 2))
        rgb = Image.fromarray(np.ascontiguousarray(chain[-1][..., :3])).resize(size, Image.BOX)
        a = Image.fromarray(np.ascontiguousarray(chain[-1][..., 3])).resize(size, Image.BOX)
        chain.append(np.dstack([np.asarray(rgb), np.asarray(a)]))
    parts = [_encode_level(lv, fmt) for lv in chain]
    body = b"".join(reversed(parts))
    total = HEADER + len(body)
    sizes, acc = [], total
    for p in parts:
        sizes.append(acc)
        acc -= len(p)
    sizes = (sizes + [0, 0, 0, 0])[:4]
    if not mips:
        flags |= FLAG_NOMIPS
    head = b"IWi" + bytes([8]) + struct.pack("<IBBHHH4I", flags, fmt, 0, w, h, 1, *sizes)
    return head + body


def _encode_level(a, fmt):
    h, w = a.shape[:2]
    if fmt == F_BGRA:
        return a[..., [2, 1, 0, 3]].tobytes()
    if fmt == F_BGR:
        return a[..., [2, 1, 0]].tobytes()
    if fmt == F_LA:
        return np.dstack([a[..., 0], a[..., 3]]).tobytes()
    if fmt in (F_L, F_A):
        return a[..., 0].tobytes()
    bw, bh = (w + 3) // 4, (h + 3) // 4
    p = np.pad(a, ((0, bh * 4 - h), (0, bw * 4 - w), (0, 0)), mode="edge")
    blocks = p.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4).astype(np.int32)
    n = bh * bw
    ar = np.arange(n)
    luma = blocks[..., :3] @ np.array([299, 587, 114])
    hi, lo = blocks[ar, luma.argmax(1), :3], blocks[ar, luma.argmin(1), :3]

    def enc(c):
        return ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)

    e0, e1 = enc(hi), enc(lo)
    e0, e1 = np.maximum(e0, e1), np.minimum(e0, e1)
    p0, p1 = _expand565(e0), _expand565(e1)
    pal = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], axis=1)       # (n,4,3)
    dist = ((blocks[:, :, None, :3] - pal[:, None, :, :]) ** 2).sum(-1)              # (n,16,4)
    idx = dist.argmin(2)
    bits = (idx.astype(np.uint64) << (2 * np.arange(16, dtype=np.uint64))).sum(1)
    colour = b"".join(struct.pack("<HHI", int(x), int(y), int(z)) for x, y, z in zip(e0, e1, bits))
    if fmt == F_DXT1:
        return colour
    al = blocks[:, :, 3]
    out = []
    if fmt == F_DXT3:
        nib = al >> 4
        for i in range(n):
            out.append(bytes(int(nib[i, 2 * j] | (nib[i, 2 * j + 1] << 4)) for j in range(8)))
    else:
        for i in range(n):
            a0, a1 = int(al[i].max()), int(al[i].min())
            if a0 == a1:
                out.append(bytes([a0, a1]) + bytes(6))
                continue
            tab = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
            bits = 0
            for k in range(16):
                j = min(range(8), key=lambda t: abs(tab[t] - int(al[i, k])))
                bits |= j << (3 * k)
            out.append(bytes([a0, a1]) + bits.to_bytes(6, "little"))
    return b"".join(o + colour[8 * i:8 * i + 8] for i, o in enumerate(out))


def _synthetic(kind, seed, w, h):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = x / w, y / h
    if kind == 0:                                           # smooth colour noise
        small = rng.random((6, 6, 3)).astype(np.float32)
        im = np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), np.float32)
    elif kind == 1:                                         # stripes at an angle
        ang = rng.uniform(0, np.pi)
        t = np.sin((u * np.cos(ang) + v * np.sin(ang)) * rng.uniform(8, 20))
        col = rng.random((2, 3)) * 255
        im = np.where((t > 0)[..., None], col[0], col[1])
    elif kind == 2:                                         # blobs
        im = np.zeros((h, w, 3), np.float32)
        for _ in range(5):
            cx, cy, r = rng.random(), rng.random(), rng.uniform(0.1, 0.3)
            m = ((u - cx) ** 2 + (v - cy) ** 2) < r * r
            im[m] = rng.random(3) * 255
    elif kind == 3:                                         # gradient plus grid
        im = np.stack([u * 255, v * 255, (u * v) * 255], -1) * rng.uniform(0.6, 1.0)
        im[(x.astype(int) // 8 + y.astype(int) // 8) % 2 == 0] *= 0.7
    else:                                                   # rings
        r = np.hypot(u - rng.random(), v - rng.random()) * rng.uniform(10, 25)
        im = np.stack([np.sin(r) * 127 + 128, np.cos(r * 1.3) * 127 + 128, np.sin(r * 0.7) * 127 + 128], -1)
    alpha = np.full((h, w), 255, np.float32)
    return np.dstack([np.clip(im, 0, 255), alpha]).astype(np.uint8)


def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise AssertionError(what)
        checks += 1

    # --- DXT decoding against hand-built blocks -------------------------------
    red, blue = 0xF800, 0x001F
    idx = sum(((k % 4) << (2 * k)) for k in range(16))               # 0,1,2,3 per row
    blk = struct.pack("<HHI", red, blue, idx)
    px = decode_dxt(blk, 4, 4, F_DXT1)
    check(tuple(px[0, 0]) == (255, 0, 0, 255) and tuple(px[0, 1]) == (0, 0, 255, 255), "dxt1 endpoints")
    check(tuple(px[0, 2]) == (170, 0, 85, 255) and tuple(px[0, 3]) == (85, 0, 170, 255), "dxt1 4-colour palette")
    px = decode_dxt(struct.pack("<HHI", blue, red, idx), 4, 4, F_DXT1)   # c0 <= c1: 3 colours + clear
    check(tuple(px[0, 2]) == (127, 0, 127, 255) and tuple(px[0, 3]) == (0, 0, 0, 0), "dxt1 transparent mode")
    alpha3 = bytes((2 * j) % 16 | (((2 * j + 1) % 16) << 4) for j in range(8))
    px = decode_dxt(alpha3 + struct.pack("<HHI", red, red, 0), 4, 4, F_DXT3)
    check([int(px[0, k, 3]) for k in range(4)] == [0, 17, 34, 51] and px[3, 3, 3] == 255, "dxt3 alpha")
    ai = sum(((k % 8) << (3 * k)) for k in range(16)).to_bytes(6, "little")
    px = decode_dxt(bytes([200, 40]) + ai + struct.pack("<HHI", red, red, 0), 4, 4, F_DXT5)
    check([int(px[0, k, 3]) for k in range(4)] == [200, 40, 177, 154], "dxt5 8-alpha mode")
    px = decode_dxt(bytes([40, 200]) + ai + struct.pack("<HHI", red, red, 0), 4, 4, F_DXT5)
    check([int(px[1, k, 3]) for k in range(4)] == [136, 168, 0, 255], "dxt5 6-alpha mode")
    px = decode_dxt(blk * 6, 12, 8, F_DXT1)                           # block placement and cropping
    check(px.shape == (8, 12, 4) and decode_dxt(blk, 3, 2, F_DXT1).shape == (2, 3, 4), "dxt block layout")

    # --- a synthetic game and its dumps ----------------------------------------
    sizes = [(64, 64), (128, 128), (128, 64), (64, 128), (32, 32), (96, 96), (128, 32), (128, 128),
             (64, 64), (128, 64), (64, 64), (48, 48), (128, 128), (64, 128)]
    formats = [F_DXT1, F_DXT5, F_BGRA, F_DXT3, F_BGR, F_DXT1, F_DXT5, F_LA, F_DXT1, F_DXT5, F_BGRA, F_L, F_DXT1, F_BGR]
    originals = {}
    for n, ((w, h), fmt) in enumerate(zip(sizes, formats)):
        im = _synthetic(n % 5, 1000 + n, w, h)
        if fmt in (F_L, F_LA):
            im[..., :3] = im[..., 1:2]
        if fmt in (F_DXT5, F_DXT3, F_BGRA, F_LA):
            im[..., 3] = np.clip(255 - np.hypot(*np.mgrid[0:h, 0:w]) * 255 / np.hypot(h, w) * (n % 2), 0, 255)
        originals["img_%02d_col" % n] = (im, fmt)

    with tempfile.TemporaryDirectory() as tmp:
        stock_dir, nested, mod_dir = (os.path.join(tmp, d) for d in ("stock", "stock/images", "mods"))
        for d in (stock_dir, nested, mod_dir, os.path.join(tmp, "dumps")):
            os.makedirs(d, exist_ok=True)
        # Format and layout round trips; half the stock in a loose folder, half in an .iwd.
        iwd = zipfile.ZipFile(os.path.join(stock_dir, "iw_00.iwd"), "w")
        for n, (name, (im, fmt)) in enumerate(originals.items()):
            data = _make_iwi(im, fmt, mips=(n % 3 != 0))
            parsed = parse_iwi(data)
            check(len(parsed.levels) == (1 if n % 3 == 0 else len(parsed.levels)), "levels")
            dec = decode_level(parsed, len(parsed.levels) - 1)
            exact = fmt in (F_BGRA, F_BGR, F_L, F_LA)
            if exact:
                want = im.copy()
                if fmt == F_BGR:
                    want[..., 3] = 255
                if fmt == F_L:
                    want[..., 3] = 255
                check(np.array_equal(dec, want), "%s lossless round trip" % name)
            else:
                err = np.abs(dec.astype(int) - im.astype(int)).mean()
                check(err < 14, "%s dxt round trip error %.1f" % (name, err))
            if n % 2:
                iwd.writestr("images/%s.iwi" % name, data)
            else:
                with open(os.path.join(nested if n % 4 == 0 else stock_dir, name + ".iwi"), "wb") as f:
                    f.write(data)
        iwd.close()
        check(parse_iwi(_make_iwi(originals["img_00_col"][0], F_DXT1, mips=True)).levels[-1][:2] == (64, 64),
              "largest level is last")
        for bad in (b"junk", b"IWi\x06" + bytes(60), b"IWi\x08" + struct.pack("<IBBHHH4I", 0, 8, 0, 4, 4, 1, 0, 0, 0, 0)):
            try:
                parse_iwi(bad)
                check(False, "bad file accepted")
            except IwiError:
                check(True, "bad file refused")

        # Mods: different pictures, same names, other formats and sizes; img_02 left identical to stock.
        mod_names = ["img_00_col", "img_01_col", "img_03_col", "img_05_col", "img_07_col", "img_11_col", "img_02_col"]
        mod_rgba = {}
        for n, name in enumerate(mod_names):
            w, h = originals[name][0].shape[1] * 2, originals[name][0].shape[0] * 2
            if name == "img_02_col":
                data = open(os.path.join(stock_dir, name + ".iwi"), "rb").read() if os.path.exists(
                    os.path.join(stock_dir, name + ".iwi")) else None
                if data is None:
                    z = zipfile.ZipFile(os.path.join(stock_dir, "iw_00.iwd"))
                    data = z.read("images/%s.iwi" % name) if "images/%s.iwi" % name in z.namelist() else None
                    if data is None:
                        data = open(os.path.join(nested, name + ".iwi"), "rb").read()
                with open(os.path.join(mod_dir, name + ".iwi"), "wb") as f:
                    f.write(data)
                continue
            im = _synthetic((n + 2) % 5, 5000 + n, w, h)
            mfmt = [F_DXT5, F_BGRA, F_DXT1, F_BGR, F_LA, F_DXT5][n % 6]
            if mfmt in (F_L, F_LA):
                im[..., :3] = im[..., 1:2]
            data = _make_iwi(im, mfmt)
            parsed = parse_iwi(data)
            mod_rgba[name] = decode_level(parsed, len(parsed.levels) - 1)
            with open(os.path.join(mod_dir, name + ".iwi"), "wb") as f:
                f.write(data)

        # Dumps: the console's copy of each original, streamed at half size for some,
        # lightly noisy, with fake keys; plus decoys no stock file matches.
        rng = np.random.default_rng(7)
        dumps_dir = os.path.join(tmp, "dumps")
        keys = {}
        listing = []
        for n, (name, (im, fmt)) in enumerate(originals.items()):
            d = im.astype(np.int32)
            if n % 3 == 1 and min(im.shape[:2]) >= 64:
                d = np.asarray(Image.fromarray(im).resize((im.shape[1] // 2, im.shape[0] // 2), Image.BOX), np.int32)
            d = np.clip(d + rng.integers(-4, 5, d.shape), 0, 255).astype(np.uint8)
            key = "%016x" % int(rng.integers(1, 2 ** 62))
            keys[name] = key
            Image.fromarray(d).save(os.path.join(dumps_dir, key + ".png"))
            listing.append("%s DXT1 %dx%d, 7 levels" % (key, d.shape[1], d.shape[0]))
        decoys = []
        for n in range(4):
            key = "%016x" % int(rng.integers(1, 2 ** 62))
            decoys.append(key)
            Image.fromarray(_synthetic(n, 90000 + n * 7, 64, 64) // 2 + 64 * (n % 2)).save(os.path.join(dumps_dir, key + ".png"))
            listing.append("%s DXT5 64x64, 7 levels" % key)
        cube = "%016x" % 12345
        Image.fromarray(_synthetic(0, 3, 32, 192)).save(os.path.join(dumps_dir, cube + ".png"))
        listing.append("%s DXT1 32x32, 1 levels, a cube, volume or array: not replaced" % cube)
        with open(os.path.join(dumps_dir, "textures.txt"), "w") as f:
            f.write("\n".join(listing) + "\n")

        out = os.path.join(tmp, "replace")
        ns = argparse.Namespace(stock=[stock_dir], dumps=dumps_dir, mod=[mod_dir], out=out, apply=True,
                                min_score=0.90, min_margin=0.02, allow_ambiguous=False,
                                report=os.path.join(tmp, "report.txt"), verbose=True)
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            run(ns)
        report = buf.getvalue()

        rows = {}
        for line in report.splitlines():
            m = re.match(r"([0-9a-f]{16})  (\S+)  (\S+)  (\S+)\s+(\S+)", line)
            if m:
                rows[m.group(1)] = (float(m.group(2)), m.group(4), m.group(5))
        for name, key in keys.items():
            score, status, matched = rows[key]
            check(matched == name and score >= 0.9, "%s matched %s (%.3f)" % (name, matched, score))
            if name in mod_rgba:
                check(status == "written", "%s status %s" % (name, status))
            else:
                check(status == "unchanged", "%s status %s" % (name, status))
        for key in decoys:
            check(rows[key][1] == "weak", "decoy %s refused (%s %.3f)" % (key, rows[key][1], rows[key][0]))
        check(cube not in rows, "cube dump not matched")

        files = sorted(os.listdir(out))
        check(files == sorted(keys[n] + ".png" for n in mod_rgba), "only modded textures written: %s" % files)
        for name, rgba in mod_rgba.items():
            got = np.asarray(Image.open(os.path.join(out, keys[name] + ".png")).convert("RGBA"))
            check(np.array_equal(got, rgba), "%s written from the modded image" % name)
            check(open(os.path.join(out, keys[name] + ".png"), "rb").read() !=
                  open(os.path.join(dumps_dir, keys[name] + ".png"), "rb").read(), "dump not copied")
        check(os.path.getsize(os.path.join(tmp, "report.txt")) > 0, "report written")
        worst = min(rows[k][0] for k in keys.values())
        best_decoy = max(rows[k][0] for k in decoys)
        print("self-test: %d checks passed (%d images matched, worst true score %.3f, best decoy %.3f, %d written)"
              % (checks, len(keys), worst, best_decoy, len(files)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Match the port's texture dumps (<key>.png + textures.txt) to the user's own IW5 .iwi "
                    "files and write the user's modded image under each key.",
        epilog="Matching: each picture becomes a 32x32 luma thumbnail and 8x8 colour and alpha thumbnails; "
               "a dump and a stock image score the weighted normalised correlation (0.55 luma, 0.30 colour, "
               "0.15 alpha; alpha only when either picture is not opaque), after an aspect-ratio gate. Flat "
               "pictures are compared by mean colour. A match needs --min-score and a lead over the next "
               "candidate of --min-margin unless the near candidates have the same modded image. "
               "Formats: BGRA, BGR, LA, L, A, DXT1/3/5. Wavelet and DXN images, cubes and volumes are skipped.")
    ap.add_argument("--stock", nargs="+", metavar="PATH", help="the user's stock .iwi folders and/or .iwd files")
    ap.add_argument("--dumps", metavar="DIR", help="MW2_DUMP_TEXTURES folder (<key>.png + textures.txt)")
    ap.add_argument("--mod", nargs="+", metavar="PATH", help="the user's modded .iwi/.iwd, same names as stock")
    ap.add_argument("--out", default="replace", metavar="DIR", help="where --apply writes <key>.png (default: replace)")
    ap.add_argument("--apply", action="store_true", help="write the replacements (default is a dry run)")
    ap.add_argument("--min-score", type=float, default=0.90, help="lowest score accepted (default 0.90)")
    ap.add_argument("--min-margin", type=float, default=0.02, help="lead over the next candidate (default 0.02)")
    ap.add_argument("--allow-ambiguous", action="store_true", help="take the best candidate even when close")
    ap.add_argument("--report", metavar="FILE", help="also write every match line to FILE")
    ap.add_argument("-v", "--verbose", action="store_true", help="list weak matches too")
    ap.add_argument("--self-test", action="store_true", help="run on synthetic data and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.stock or not args.dumps:
        ap.error("--stock and --dumps are required")
    if args.apply and not args.mod:
        ap.error("--apply needs --mod: only the user's modded images are written, never the dumps")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

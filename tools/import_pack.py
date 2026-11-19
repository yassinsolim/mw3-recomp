#!/usr/bin/env python3
"""Turn a PC camouflage mod into a camouflage pack for this port
(docs/textures.md, "Camouflage packs").

A pack is what Modern Warfare 3's multiplayer lists below Gold in Create a
Class: a folder `camos/<pack-id>/` in the game folder with `pack.json` and the
pictures it names. This reads a mod as its author published it -- a folder, a
.zip or an .iwd of .iwi images, or of PNG / DDS / TGA / JPEG pictures -- and
writes that folder.

ONLY IMPORT WHAT YOU MAY. A pack carries its author's name, link and the note
of the permission given, and the port shows them in its credits and in the
camouflage's description in the game. --author, --url and --permission are
therefore required, and nothing is fetched here: the mod is a download you
made from where its author published it. Packs are never committed to this
repository.

What becomes a camouflage
  Every picture is one camouflage, named after its file, except:
  - a picture named `ui_camoskin_<x>`, `<x>_swatch`, `<x>_icon` or
    `<x>_preview` is the menu's preview of camouflage <x>, not one of its own;
  - normal, specular and other technical maps (`*_n`, `*_nml`, `*_s`, `*_spc`,
    `*_cos`, `*_g`, `*_o`, and IW names whose usage is not colour) are skipped;
  - pictures `<x>_0`, `<x>_1`, ... (or `<x>.0`, or numbered pictures in a folder
    <x>) are the frames of one animated camouflage <x>, shown at --fps.
  IW names are tidied: `~weapon_camo_carbon-rgb&detail_~1a2b3c4d` is `carbon`.
  --only keeps the camouflages whose id matches a pattern; --rename gives one
  a name of your choosing.

    python tools/import_pack.py mods/carbon_pack.zip --out game/camos/carbon-pack \
        --name "Carbon Pack" --author "Their Name" --url https://example.org/carbon-pack \
        --permission "Used with the author's permission, 2026-11-19."
    python tools/import_pack.py --self-test

Requires Python 3, Pillow and numpy (the IWI reader is tools/iwi_match.py's).
"""
import argparse
import fnmatch
import io
import json
import os
import re
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iwi_match  # noqa: E402

PICTURES = (".png", ".dds", ".tga", ".jpg", ".jpeg", ".bmp")
TECHNICAL = re.compile(r"(_n|_nml|_nrm|_normal|_s|_spc|_spec|_cos|_g|_gloss|_o|_occ)$")
PREVIEW = (re.compile(r"^ui_camoskin_(.+?)(?:_sm)?$"), re.compile(r"^(.+)_(?:swatch|icon|preview)$"))
FRAME = re.compile(r"^(.+?)[._-](\d{1,4})$")


def read_iwi(data):
    iwi = iwi_match.parse_iwi(data)
    if iwi.flags & (iwi_match.FLAG_CUBE | iwi_match.FLAG_VOLUME):
        raise iwi_match.IwiError("a cube or a volume")
    if iwi.fmt == iwi_match.F_DXN:
        raise iwi_match.IwiError("a normal map")
    return Image.fromarray(iwi_match.decode_level(iwi, len(iwi.levels) - 1), "RGBA")


def sources(path):
    """Yield (relative name without extension, lower case, forward slashes; a
    function that reads the picture) for every picture in a folder, zip or iwd."""
    def one(name, data):
        stem, ext = os.path.splitext(name.replace("\\", "/").lower())
        if ext == ".iwi":
            return stem, lambda: read_iwi(data())
        if ext in PICTURES:
            return stem, lambda: Image.open(io.BytesIO(data())).convert("RGBA")
        return None

    def from_zip(z, prefix=""):
        for member in sorted(z.namelist()):
            if member.lower().endswith((".iwd", ".zip")):
                try:
                    yield from from_zip(zipfile.ZipFile(io.BytesIO(z.read(member))), prefix)
                except zipfile.BadZipFile:
                    print("warning: %s is not a zip" % member, file=sys.stderr)
                continue
            found = one(prefix + member, lambda m=member: z.read(m))
            if found:
                yield found

    if os.path.isfile(path):
        if path.lower().endswith((".zip", ".iwd")):
            yield from from_zip(zipfile.ZipFile(path))
        else:
            found = one(os.path.basename(path), lambda: open(path, "rb").read())
            if found:
                yield found
        return
    for folder, _dirs, files in sorted(os.walk(path)):
        for f in sorted(files):
            full = os.path.join(folder, f)
            if f.lower().endswith((".zip", ".iwd")):
                yield from from_zip(zipfile.ZipFile(full))
                continue
            found = one(os.path.relpath(full, path), lambda p=full: open(p, "rb").read())
            if found:
                yield found


def tidy(stem):
    """An IW image name as the camouflage it is, or None for a technical map:
    `~weapon_camo_carbon-rgb&detail_~1a2b3c4d` -> `carbon`."""
    name = stem.rsplit("/", 1)[-1]
    if name.startswith("~"):
        name = name[1:]
        name = re.sub(r"_?~[0-9a-f]{6,8}$", "", name)       # the hash IW appends
        usage = re.search(r"-([a-z0-9&_]+)$", name)
        if usage:
            # The channels the image was built from: colour first is a colour map.
            if not usage.group(1).startswith(("rgb", "c", "col")):
                return None
            name = name[:usage.start()]
    if TECHNICAL.search(name):
        return None
    name = re.sub(r"^(weapon_camo_|weapon_|camo_)", "", name)
    return name or None


def ident(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def title(name):
    return " ".join(w.capitalize() for w in re.split(r"[^a-z0-9]+", name.lower()) if w)


def collect(path):
    """-> ({camo id: {"name", "frames": [(order, read)]}}, {camo id: read preview})"""
    camos, previews = {}, {}
    for stem, read in sources(path):
        base = stem.rsplit("/", 1)[-1]
        for pattern in PREVIEW:
            m = pattern.match(base)
            if m:
                previews[ident(tidy(m.group(1)) or m.group(1))] = read
                break
        else:
            name = tidy(stem)
            if name is None:
                continue
            folder = stem.rsplit("/", 1)[0].rsplit("/", 1)[-1] if "/" in stem else ""
            order = 0
            frame = FRAME.match(name)
            if name.isdigit() and folder:               # numbered pictures in a folder named for the camo
                name, order = tidy(folder) or folder, int(name)
            elif frame:
                name, order = frame.group(1), int(frame.group(2))
            key = ident(name)
            if not key:
                continue
            camos.setdefault(key, {"name": title(name), "frames": []})["frames"].append((order, read))
    return camos, previews


def fit(im, size):
    if max(im.size) <= size:
        return im
    s = size / max(im.size)
    return im.resize((max(4, round(im.width * s)), max(4, round(im.height * s))), Image.LANCZOS)


def import_pack(source, out, name, author, url, permission, only=None, rename=None, fps=12.0, size=256,
                log=print):
    camos, previews = collect(source)
    rename = rename or {}
    os.makedirs(out, exist_ok=True)
    entries = []
    for key in sorted(camos):
        if only and not any(fnmatch.fnmatch(key, p) for p in only):
            continue
        frames = []
        for _order, read in sorted(camos[key]["frames"], key=lambda f: f[0]):
            try:
                frames.append(fit(read(), size))
            except (iwi_match.IwiError, OSError, ValueError) as e:
                log("  %s: a picture is left out (%s)" % (key, e))
        if not frames:
            continue
        frames = [f for f in frames if f.size == frames[0].size]      # an animation is one size
        entry = {"id": key, "name": rename.get(key, camos[key]["name"])}
        if len(frames) > 1:
            os.makedirs(os.path.join(out, key), exist_ok=True)
            entry["frames"] = []
            for n, frame in enumerate(frames):
                rel = "%s/%d.png" % (key, n)
                frame.save(os.path.join(out, rel))
                entry["frames"].append(rel)
            entry["fps"] = fps
        else:
            entry["image"] = key + ".png"
            frames[0].save(os.path.join(out, entry["image"]))
        if key in previews:
            try:
                fit(previews[key](), 128).save(os.path.join(out, key + "_swatch.png"))
                entry["swatch"] = key + "_swatch.png"
            except (iwi_match.IwiError, OSError, ValueError) as e:
                log("  %s: its preview is left out (%s)" % (key, e))
        entries.append(entry)
        log("  %-24s %s, %d frame%s%s" % (key, entry["name"], len(frames), "" if len(frames) == 1 else "s",
                                         ", with a preview" if "swatch" in entry else ""))
    pack = {"name": name, "author": author, "url": url, "permission": permission, "camos": entries}
    with open(os.path.join(out, "pack.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(pack, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return entries


def main(argv=None):
    ap = argparse.ArgumentParser(description="Turn a PC camouflage mod (a folder, .zip or .iwd of .iwi or "
                                             "PNG/DDS pictures) into a camouflage pack for the port.")
    ap.add_argument("source", nargs="?", help="the mod: a folder, a .zip, an .iwd or one picture")
    ap.add_argument("--out", metavar="DIR", help="the pack's folder: camos/<pack-id> in the game folder")
    ap.add_argument("--name", help="the pack's name")
    ap.add_argument("--author", help="who made it, as they want to be credited")
    ap.add_argument("--url", help="where the author published it")
    ap.add_argument("--permission", help="the note of the permission you have, or the licence it is under")
    ap.add_argument("--only", action="append", metavar="PATTERN", help="keep the camos whose id matches (repeatable)")
    ap.add_argument("--rename", action="append", metavar="ID=NAME", default=[], help="name a camo yourself (repeatable)")
    ap.add_argument("--fps", type=float, default=12.0, help="speed of animated camos (default 12)")
    ap.add_argument("--size", type=int, default=256, help="longest side of a picture kept (default 256)")
    ap.add_argument("--self-test", action="store_true", help="import a synthetic mod and check the pack")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    missing = [o for o in ("source", "out", "name", "author", "url", "permission") if not getattr(args, o)]
    if missing:
        ap.error("needed: " + ", ".join(("--" + m) if m != "source" else "the mod" for m in missing) +
                 " (a pack always says whose it is and by what permission it is used)")
    rename = dict(r.split("=", 1) for r in args.rename if "=" in r)
    entries = import_pack(args.source, args.out, args.name, args.author, args.url, args.permission,
                          args.only, rename, args.fps, args.size)
    if not entries:
        print("no camouflage found in %s" % args.source, file=sys.stderr)
        return 1
    print("%s: %d camos" % (args.out, len(entries)))
    return 0


def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise AssertionError(what)
        checks += 1

    rng = np.random.default_rng(1)

    def picture(w, h):
        return rng.integers(0, 255, (h, w, 4), dtype=np.uint8)

    with tempfile.TemporaryDirectory() as tmp:
        mod = os.path.join(tmp, "mod")
        os.makedirs(os.path.join(mod, "images", "lava"))
        carbon = picture(64, 64)
        carbon[..., 3] = 255
        with open(os.path.join(mod, "images", "~weapon_camo_carbon-rgb&detail_~1a2b3c4d.iwi"), "wb") as f:
            f.write(iwi_match._make_iwi(carbon, iwi_match.F_BGRA))
        with open(os.path.join(mod, "images", "~weapon_camo_carbon-g_~1a2b3c4d.iwi"), "wb") as f:
            f.write(iwi_match._make_iwi(picture(64, 64), iwi_match.F_BGRA))        # a gloss map
        Image.fromarray(picture(512, 256)).save(os.path.join(mod, "images", "Tiger Stripe.png"))
        Image.fromarray(picture(32, 32)).save(os.path.join(mod, "images", "tiger_stripe_n.png"))
        Image.fromarray(picture(48, 48)).save(os.path.join(mod, "images", "ui_camoskin_carbon.png"))
        for n in range(3):
            Image.fromarray(picture(32, 32)).save(os.path.join(mod, "images", "lava", "%d.png" % n))
        with zipfile.ZipFile(os.path.join(mod, "extra.iwd"), "w") as z:
            buf = io.BytesIO()
            Image.fromarray(picture(16, 16)).save(buf, "PNG")
            z.writestr("images/weapon_camo_zebra.png", buf.getvalue())

        out = os.path.join(tmp, "camos", "test-pack")
        entries = import_pack(mod, out, "Test", "Someone", "https://example.org", "permitted", log=lambda *_: None)
        by = {e["id"]: e for e in entries}
        check(set(by) == {"carbon", "tiger-stripe", "lava", "zebra"}, "the camos found: %s" % sorted(by))
        check(by["tiger-stripe"]["name"] == "Tiger Stripe", "a name from the file's")
        check(Image.open(os.path.join(out, "tiger-stripe.png")).size == (256, 128), "the longest side capped")
        check(len(by["lava"]["frames"]) == 3 and by["lava"]["fps"] == 12.0, "numbered pictures are frames")
        check(by["carbon"].get("swatch") == "carbon_swatch.png", "ui_camoskin_<x> is the preview")
        got = np.asarray(Image.open(os.path.join(out, "carbon.png")).convert("RGBA"))
        check(np.array_equal(got, carbon), "an IWI's picture, texel for texel")
        pack = json.load(open(os.path.join(out, "pack.json"), encoding="utf-8"))
        check(pack["author"] == "Someone" and pack["url"] == "https://example.org" and
              pack["permission"] == "permitted" and pack["name"] == "Test", "the credit is in pack.json")
        check(all(os.path.isfile(os.path.join(out, p)) for e in pack["camos"]
                  for p in e.get("frames", []) + [e[k] for k in ("image", "swatch") if k in e]),
              "every picture named is in the pack")
        only = import_pack(mod, os.path.join(tmp, "camos", "one"), "T", "S", "u", "p", only=["zeb*"],
                           rename={"zebra": "Stripes"}, log=lambda *_: None)
        check([e["name"] for e in only] == ["Stripes"], "--only and --rename")
        import contextlib
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                main([mod, "--out", out, "--name", "x"])
            check(False, "a pack without credit is refused")
        except SystemExit:
            check(True, "a pack without credit is refused")
    print("import_pack self-test: %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())

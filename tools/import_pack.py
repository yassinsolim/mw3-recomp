#!/usr/bin/env python3
"""Turn PC camouflage mods into camouflage packs for this port
(docs/textures.md, "Camouflage packs").

A pack is what Modern Warfare 3's multiplayer lists below Gold in Create a
Class: a folder `camos/<pack-id>/` in the game folder with `pack.json` and the
pictures it names. This reads a mod as its author published it -- a folder, a
.zip, .iwd, .rar or .7z, or loose pictures, in any layout -- and writes that
folder. Nothing is fetched here: the mod is a download you made from where its
author published it.

NOTHING IS REPLACED. A PC mod is a set of images named after the game's image
each one replaces. Here every such image becomes a camouflage of its own:

  detail_gold_col, ~detail_gold_spc-..., weapon_camo_menu_gold[_sm]
      one camouflage made from a mod's Gold (colour map, specular map, the
      menu's banner and its small preview);
  weapon_camo_<x>, weapon_camo_menu_<x>[_sm]
      one camouflage made from a mod's replacement of the game's <x>
      (classic, snow, multi, d_urban, hex, choco, snake, blue, red, autumn,
      marine, winter);
  weapon_<gun>_col, ~weapon_<gun>_spc-...
      whole-weapon skins: all of them together are one camouflage that has a
      tile of each weapon's own skin, and the richest of them for every other
      weapon. Normal maps and scopes are skipped.

So two mods that both replace Gold are two entries, and the game's own
camouflages stay as they are. A mod that offers the same image in several
folders (Sloth's packs have `All` and one folder a camouflage) lists it once,
named after the folder that says what it is ("autumn - Orange" is Orange); the
same image with different content in several folders is one entry a folder.
A missing preview is made from the camouflage.

Pictures that are not named after an image of the game's (PNG, DDS, TGA, JPEG)
are each a camouflage named after its file when the mod has none of the
above: `<x>_swatch`, `<x>_icon`, `<x>_preview` and `ui_camoskin_<x>` are
previews, `<x>_0`, `<x>_1`, ... (or numbered pictures in a folder <x>) the
frames of an animation, and technical maps (`*_n`, `*_nml`, `*_s`, `*_spc`,
...) are skipped.

WHOSE IT IS. setup/camo-packs.json knows the packs the project has looked at:
a mod whose file name matches one gets its name, author, link and the names
of its camouflages from there. For any other, give --name, --author and --url;
without them the pack is named after its file and has no author.

    python tools/import_pack.py "Interstellar.zip" --camos game/camos
    python tools/import_pack.py mods/carbon_pack.zip --camos game/camos \
        --name "Carbon Pack" --author "Their Name" --url https://example.org/carbon-pack
    python tools/import_pack.py --folder camo-packs --camos game/camos
    python tools/import_pack.py --self-test

--folder takes a folder of downloads: every archive and every folder in it is
a pack, and loose pictures in it are one more, named after the folder.

Requires Python 3, Pillow and numpy (the IWI reader is tools/iwi_match.py's);
.rar and .7z need 7-Zip, or the `tar` of Windows 10 and later.
"""
import argparse
import fnmatch
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iwi_match  # noqa: E402

PICTURES = (".png", ".dds", ".tga", ".jpg", ".jpeg", ".bmp")
ZIPS = (".zip", ".iwd")
ARCHIVES = (".rar", ".7z")
TECHNICAL = re.compile(r"(_n|_nml|_nrml|_nrm|_normal|_s|_spc|_spec|_cos|_g|_gloss|_o|_occ)$")
PREVIEW = (re.compile(r"^ui_camoskin_(.+?)(?:_sm)?$"), re.compile(r"^(.+)_(?:swatch|icon|preview)$"))
FRAME = re.compile(r"^(.+?)[._-](\d{1,4})$")
KNOWN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "setup", "camo-packs.json")

# The game's camouflages by the name its images carry, as the menu calls them.
STOCK = {
    "classic": "Classic", "snow": "Snow", "multi": "Multicam", "d_urban": "Digital Urban", "hex": "Hex",
    "choco": "Choco", "snake": "Snake", "blue": "Blue", "red": "Red", "autumn": "Autumn", "gold": "Gold",
    "marine": "Marine", "winter": "Winter",
}
# A weapon's skin image by the weapon a class names (iw5_<x>): the images'
# names are older than the weapons' and differ where listed.
SKIN_WEAPON = {
    "m4_spr": "m4", "remington_acr": "acr", "remington_msr": "msr", "l_96_a1": "l96a1", "m14_ebr": "mk14",
    "uzi_m9": "m9", "g36": "g36c", "desert_eagle": "deserteagle", "44_magnum": "44magnum", "spas_12": "spas12",
    "m82": "barrett", "fmg": "fmg9", "scar_l": "scar", "striker": "striker", "pecheneg": "pecheneg",
    "sa80": "sa80", "mp5_new": "mp5",
}
SIZE, SWATCH, BANNER = 256, 128, (408, 152)


def read_picture(name, data):
    if name.lower().endswith(".iwi"):
        iwi = iwi_match.parse_iwi(data)
        if iwi.flags & (iwi_match.FLAG_CUBE | iwi_match.FLAG_VOLUME):
            raise iwi_match.IwiError("a cube or a volume")
        if iwi.fmt == iwi_match.F_DXN:
            raise iwi_match.IwiError("a normal map")
        return Image.fromarray(iwi_match.decode_level(iwi, len(iwi.levels) - 1), "RGBA")
    return Image.open(io.BytesIO(data)).convert("RGBA")


def unpack(archive, into):
    """A .rar or .7z into a folder, by 7-Zip or the system's tar (libarchive reads both)."""
    seven = shutil.which("7z") or shutil.which("7za")
    for candidate in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if not seven and os.path.isfile(candidate):
            seven = candidate
    if seven:
        command = [seven, "x", "-y", "-o" + into, archive]
    elif shutil.which("tar"):
        command = ["tar", "-xf", archive, "-C", into]
    else:
        raise OSError("%s needs 7-Zip or tar to be opened" % os.path.basename(archive))
    done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if done.returncode:
        raise OSError("%s does not open: %s" % (os.path.basename(archive),
                                                done.stderr.decode("utf-8", "replace").strip()[-200:]))


def sources(path, scratch):
    """Yield (name with forward slashes, lower case; a function that returns the
    file's bytes) for every picture of a mod: a folder, an archive or one file."""
    def wanted(name):
        return name.lower().endswith(PICTURES + (".iwi",))

    def from_zip(z, prefix):
        for member in sorted(z.namelist()):
            if member.lower().endswith(ZIPS):
                try:
                    yield from from_zip(zipfile.ZipFile(io.BytesIO(z.read(member))), prefix + member + "/")
                except zipfile.BadZipFile:
                    print("warning: %s is not a zip" % member, file=sys.stderr)
            elif wanted(member):
                yield (prefix + member).replace("\\", "/").lower(), lambda m=member: z.read(m)

    def from_folder(folder, prefix):
        for at, _dirs, files in sorted(os.walk(folder)):
            for f in sorted(files):
                full = os.path.join(at, f)
                yield from from_file(full, prefix + os.path.relpath(full, folder))

    def from_file(full, name):
        if full.lower().endswith(ZIPS):
            yield from from_zip(zipfile.ZipFile(full), name + "/")
        elif full.lower().endswith(ARCHIVES):
            into = tempfile.mkdtemp(dir=scratch)
            unpack(full, into)
            yield from from_folder(into, name + "/")
        elif wanted(full):
            yield name.replace("\\", "/").lower(), lambda p=full: open(p, "rb").read()

    if os.path.isdir(path):
        yield from from_folder(path, "")
    else:
        yield from from_file(path, os.path.basename(path))


def classify(stem):
    """What an image named after one of the game's is: (kind, target, role), the
    kind "camo" or "skin", or None for any other name. Roles: colour, specular,
    banner, swatch; "skip" for a map that is not a picture."""
    if re.match(r"^~?detail_gold_(col|c)\b", stem):
        return "camo", "gold", "colour"
    if re.match(r"^~?detail_gold_(spc|spec|s)\b", stem):
        return "camo", "gold", "specular"
    m = re.match(r"^weapon_camo_menu_(.+?)(_sm)?$", stem)
    if m:
        return "camo", m.group(1), "swatch" if m.group(2) else "banner"
    m = re.match(r"^(~?)weapon_camo_([a-z0-9_]+?)(?:-[a-z0-9]*rgb.*)?$", stem)
    if m:
        return "camo", m.group(2), "specular" if m.group(1) else "colour"
    m = re.match(r"^(~?)(?:weapon_)?([a-z0-9_]+?)_(col|spc|spec|nml|nrml|cos)(?:-.*|~.*)?$", stem)
    if m:
        derived, gun, usage = m.group(1) == "~", m.group(2), m.group(3)
        if usage in ("nml", "nrml", "cos") or "scope" in gun:
            return "skin", gun, "skip"
        if derived == (usage in ("spc", "spec")):
            gun = re.sub(r"_iw5$", "", gun)
            return "skin", "iw5_" + SKIN_WEAPON.get(gun, gun.replace("_", "")), "specular" if derived else "colour"
    return None


def tidy(stem):
    """A free picture's name as the camouflage it is, or None for a technical map."""
    name = stem.rsplit("/", 1)[-1]
    if name.startswith("~"):
        name = re.sub(r"_?~[0-9a-f]{6,8}$", "", name[1:])
        usage = re.search(r"-([a-z0-9&_]+)$", name)
        if usage:
            if not usage.group(1).startswith(("rgb", "c", "col")):
                return None
            name = name[:usage.start()]
    if TECHNICAL.search(name):
        return None
    return re.sub(r"^(weapon_camo_|weapon_|camo_)", "", name) or None


def ident(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def title(name):
    return " ".join(w.capitalize() for w in re.split(r"[^a-z0-9]+", name.lower()) if w)


def fit(im, size):
    if max(im.size) <= size:
        return im
    s = size / max(im.size)
    return im.resize((max(4, round(im.width * s)), max(4, round(im.height * s))), Image.LANCZOS)


def swatch_of(im):
    """The menu's small preview, made from the camouflage when a mod has none."""
    side = min(im.size)
    return im.crop((0, 0, side, side)).resize((SWATCH, SWATCH), Image.LANCZOS)


def tile_of(sheet, work=512, window=64):
    """A camouflage tile out of a whole-weapon skin: the window of the sheet
    with the most fine detail in every quarter of it -- the painted pattern,
    not the flat or smooth parts around it -- mirrored into a square that
    repeats without a seam. Returns (tile, how much detail it has)."""
    im = sheet.convert("RGB")
    s = work / max(im.size)
    im = im.resize((max(window, round(im.width * s)), max(window, round(im.height * s))), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32)
    g = a.mean(2)
    lap = np.zeros_like(g)
    lap[1:-1, 1:-1] = np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])
    total = np.pad(lap.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    q = window // 2
    best = (-1.0, 0, 0)
    for y in range(0, g.shape[0] - window + 1, 8):
        for x in range(0, g.shape[1] - window + 1, 8):
            score = min(total[y + dy + q, x + dx + q] - total[y + dy, x + dx + q] - total[y + dy + q, x + dx]
                        + total[y + dy, x + dx] for dy in (0, q) for dx in (0, q))
            if score > best[0]:
                best = (float(score), x, y)
    score, x, y = best
    t = a[y:y + window, x:x + window].astype(np.uint8)
    top = np.concatenate([t, t[:, ::-1]], 1)
    return Image.fromarray(np.concatenate([top, top[::-1]], 0)), score


def known_pack(source, path=KNOWN):
    """The entry of setup/camo-packs.json whose `match` the mod's file name fits."""
    try:
        with open(path, encoding="utf-8") as f:
            packs = json.load(f)["packs"]
    except (OSError, ValueError, KeyError):
        return {}
    base = os.path.splitext(os.path.basename(os.path.normpath(source)))[0].lower()
    for pack in packs:
        if any(fnmatch.fnmatch(base, pattern) for pattern in pack.get("match", [])):
            return pack
    return {}


def collect(files, pack_name, names):
    """The camouflages of a mod, from its pictures [(name, read bytes)]:
    [{"id", "name", "target", roles: read picture, "weapons": {weapon: read}, "frames": [read]}]."""
    def reader(name, data):
        return lambda: read_picture(name, data())

    slots, skins, free = {}, {}, []
    for name, data in files:
        stem = os.path.splitext(name)[0]
        folder, base = stem.rsplit("/", 1) if "/" in stem else ("", stem)
        what = classify(base)
        if not what:
            free.append((stem, reader(name, data)))
        elif what[0] == "camo":
            slots.setdefault((folder, what[1]), {}).setdefault(what[2], (data, reader(name, data)))
        elif what[2] != "skip":
            skins.setdefault(what[1], {}).setdefault(what[2], reader(name, data))

    out = []
    # One entry for each distinct content of a target; of the folders that
    # have the same content, the one that names it (not `All`) is kept.
    by_content = {}
    for (folder, target), roles in sorted(slots.items()):
        main = roles.get("specular") or roles.get("colour")
        if not main:
            continue   # a preview with no camouflage to go with it
        digest = hashlib.sha256(b"".join(roles[r][0]() for r in ("colour", "specular") if r in roles)).hexdigest()
        leaf = folder.rsplit("/", 1)[-1]
        naming = (" - " in leaf, leaf not in ("all", "images", ""))
        have = by_content.get((target, digest))
        if not have or naming > have[0]:
            merged = dict(have[3]) if have else {}
            merged.update(roles)
            by_content[(target, digest)] = (naming, folder, leaf, merged)
        else:
            for role, value in roles.items():
                have[3].setdefault(role, value)
    variants = {}
    for (target, _digest) in by_content:
        variants[target] = variants.get(target, 0) + 1
    single = len(by_content) + bool(skins) == 1
    for (target, _digest), (_naming, _folder, leaf, roles) in sorted(by_content.items(), key=lambda i: i[1][1]):
        said = re.match(r"^.+?\s+-\s+(.+)$", leaf)
        if target in names:
            name = names[target]
        elif said:
            name = title(said.group(1))
        elif single:
            name = pack_name
        elif variants[target] > 1 and leaf:
            name = title(leaf)
        else:
            name = STOCK.get(target, title(target))
        key = ident(target) if variants[target] == 1 else ident(leaf + "-" + target)
        entry = {"id": key, "name": name, "target": "gold" if target == "gold" else "weapon_camo_" + target}
        entry.update({role: value[1] for role, value in roles.items()})
        out.append(entry)

    if skins:
        out.append({"id": "skin" if out else ident(pack_name) or "skin", "name": names.get("skin", pack_name),
                    "target": "weapons",
                    "weapons": {weapon: maps.get("specular") or maps["colour"] for weapon, maps in sorted(skins.items())
                                if maps.get("specular") or maps.get("colour")}})
    if out:
        return out   # what else the mod holds is its screenshots

    camos, previews = {}, {}
    for stem, read in free:
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
            if key:
                camos.setdefault(key, {"name": title(name), "frames": []})["frames"].append((order, read))
    for key in sorted(camos):
        entry = {"id": key, "name": names.get(key, camos[key]["name"]),
                 "frames": [read for _order, read in sorted(camos[key]["frames"], key=lambda f: f[0])]}
        if key in previews:
            entry["swatch"] = previews[key]
        out.append(entry)
    return out


def import_pack(source, out, name=None, author=None, url=None, only=None, rename=None, fps=12.0, label=None,
                known=KNOWN, log=print):
    """Writes the mod `source` as the pack folder `out` (or, with `out` None,
    returns what it would be called). -> the pack's entries."""
    listed = known_pack(source, known)
    name = name or listed.get("name") or title(os.path.splitext(os.path.basename(os.path.normpath(source)))[0])
    names = dict(listed.get("names", {}))
    names.update(rename or {})
    with tempfile.TemporaryDirectory() as scratch:
        found = collect(list(sources(source, scratch)), name, names)
        if only:
            found = [e for e in found if any(fnmatch.fnmatch(e["id"], p) for p in only)]
        entries = []
        for camo in found:
            key = camo["id"]
            try:
                entry = write_camo(camo, out, fps)
            except (iwi_match.IwiError, OSError, ValueError) as e:
                log("  %s is left out (%s)" % (key, e))
                continue
            entries.append(entry)
            log("  %-24s %s%s" % (key, entry["name"], ", %d weapons" % len(entry["weapons"]) if "weapons" in entry else ""))
        if not entries:
            return []
        pack = {"name": name, "author": author or listed.get("author", ""), "url": url or listed.get("url", "")}
        if label or listed.get("label"):
            pack["label"] = label or listed["label"]
        pack["camos"] = entries
        with open(os.path.join(out, "pack.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(pack, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return entries


def write_camo(camo, out, fps):
    key = camo["id"]
    os.makedirs(out, exist_ok=True)
    entry = {"id": key, "name": camo["name"]}
    if "target" in camo:
        entry["target"] = camo["target"]

    def save(im, rel):
        os.makedirs(os.path.dirname(os.path.join(out, rel)), exist_ok=True)
        im.save(os.path.join(out, rel))
        return rel

    drawn = None
    if "weapons" in camo:
        entry["weapons"] = {}
        best = -1.0
        for weapon, read in camo["weapons"].items():
            try:
                tile, detail = tile_of(read())
            except (iwi_match.IwiError, OSError, ValueError):
                continue
            entry["weapons"][weapon] = save(tile, "%s/%s.png" % (key, weapon))
            if detail > best:
                best, drawn = detail, tile
        if drawn is None:
            raise ValueError("none of its weapon skins decodes")
        entry["image"] = save(drawn, key + ".png")
    elif "frames" in camo:
        frames = [fit(read(), SIZE) for read in camo["frames"]]
        frames = [f for f in frames if f.size == frames[0].size]      # an animation is one size
        drawn = frames[0]
        if len(frames) > 1:
            entry["frames"] = [save(frame, "%s/%d.png" % (key, n)) for n, frame in enumerate(frames)]
            entry["fps"] = fps
        else:
            entry["image"] = save(drawn, key + ".png")
    else:
        # Gold's image on the console is its specular map, so a mod's specular
        # map is what is drawn; its colour map is kept beside it.
        for role in ("specular", "colour"):
            if role in camo:
                picture = fit(camo[role](), SIZE)
                entry[role] = save(picture, "%s_%s.png" % (key, "spc" if role == "specular" else "col"))
                drawn = drawn or picture
    try:
        small = fit(camo["swatch"](), SWATCH) if "swatch" in camo else swatch_of(drawn)
    except (iwi_match.IwiError, OSError, ValueError):
        small = swatch_of(drawn)
    entry["swatch"] = save(small, key + "_swatch.png")
    if "banner" in camo:
        try:
            entry["banner"] = save(camo["banner"]().resize(BANNER, Image.LANCZOS), key + "_banner.png")
        except (iwi_match.IwiError, OSError, ValueError):
            pass
    return entry


def import_folder(folder, camos, known=KNOWN, log=print):
    """Every mod in a folder of downloads: each archive and each folder is a
    pack, and loose pictures are one more. -> {pack id: its entries}."""
    done = {}
    loose = False
    mods = []
    for item in sorted(os.listdir(folder)):
        full = os.path.join(folder, item)
        if os.path.isdir(full) or item.lower().endswith(ZIPS + ARCHIVES):
            mods.append(full)
        elif item.lower().endswith(PICTURES + (".iwi",)):
            loose = True
    if loose:
        # The folder itself, without the mods beside the loose pictures.
        with tempfile.TemporaryDirectory() as only_loose:
            target = os.path.join(only_loose, os.path.basename(os.path.normpath(folder)))
            os.makedirs(target)
            for item in os.listdir(folder):
                if item.lower().endswith(PICTURES + (".iwi",)):
                    shutil.copy2(os.path.join(folder, item), target)
            done.update(import_one(target, camos, known, log))
    for mod in mods:
        done.update(import_one(mod, camos, known, log))
    return done


def import_one(source, camos, known=KNOWN, log=print, **credit):
    listed = known_pack(source, known)
    name = credit.get("name") or listed.get("name") or \
        title(os.path.splitext(os.path.basename(os.path.normpath(source)))[0])
    pack_id = listed.get("id") or ident(name) or "pack"
    out = os.path.join(camos, pack_id)
    log("%s -> %s" % (source, out))
    try:
        if os.path.isdir(out):
            shutil.rmtree(out)
        entries = import_pack(source, out, known=known, log=log, **credit)
    except (OSError, zipfile.BadZipFile) as e:
        log("  not imported: %s" % e)
        return {}
    if not entries:
        fastfile = any(f.lower().endswith(".ff") for f in names_in(source))
        log("  no camouflage found in it" + ("; its .ff is a fastfile, which is not read (a mod that animates Gold"
                                             " is the camo packs' own Animate option here)" if fastfile else ""))
        return {}
    return {pack_id: entries}


def names_in(source):
    """The file names of a mod that is a folder or a zip, for saying what it holds."""
    if os.path.isdir(source):
        return [f for _at, _dirs, files in os.walk(source) for f in files]
    try:
        return zipfile.ZipFile(source).namelist()
    except (OSError, zipfile.BadZipFile):
        return []


def main(argv=None):
    ap = argparse.ArgumentParser(description="Turn PC camouflage mods (folders, .zip, .iwd, .rar, .7z or loose "
                                             ".iwi/PNG/DDS pictures) into camouflage packs for the port.")
    ap.add_argument("source", nargs="*", help="a mod: a folder, an archive or one picture")
    ap.add_argument("--folder", metavar="DIR", help="a folder of downloads, each archive or folder in it a mod")
    ap.add_argument("--camos", metavar="DIR", help="the game folder's `camos`: each pack goes in a folder of its own there")
    ap.add_argument("--name", help="the pack's name (one mod only)")
    ap.add_argument("--author", help="who made it, as they want to be credited")
    ap.add_argument("--url", help="where the author published it")
    ap.add_argument("--label", help="a short name for the menu's list, when the pack's is long")
    ap.add_argument("--only", action="append", metavar="PATTERN", help="keep the camos whose id matches (repeatable)")
    ap.add_argument("--rename", action="append", metavar="ID=NAME", default=[], help="name a camo yourself (repeatable)")
    ap.add_argument("--fps", type=float, default=12.0, help="speed of a mod's own frame animations (default 12)")
    ap.add_argument("--known", default=KNOWN, help=argparse.SUPPRESS)
    ap.add_argument("--self-test", action="store_true", help="import synthetic mods and check the packs")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.camos or not (args.source or args.folder):
        ap.error("needed: --camos and a mod (or --folder)")
    if len(args.source) > 1 and (args.name or args.author or args.url):
        ap.error("--name, --author and --url describe one mod")
    done = {}
    if args.folder:
        done.update(import_folder(args.folder, args.camos, args.known))
    credit = {k: getattr(args, k) for k in ("name", "author", "url", "label", "only", "fps") if getattr(args, k)}
    if args.rename:
        credit["rename"] = dict(r.split("=", 1) for r in args.rename if "=" in r)
    for source in args.source:
        done.update(import_one(source, args.camos, args.known, **credit))
    for pack_id, entries in done.items():
        print("%s: %d camos" % (pack_id, len(entries)))
    return 0 if done else 1


def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise AssertionError(what)
        checks += 1

    rng = np.random.default_rng(1)

    def picture(w, h, opaque=True):
        p = rng.integers(0, 255, (h, w, 4), dtype=np.uint8)
        if opaque:
            p[..., 3] = 255
        return p

    def iwi(path, rgba):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(iwi_match._make_iwi(rgba, iwi_match.F_BGRA))

    quiet = {"log": lambda *_: None}
    with tempfile.TemporaryDirectory() as tmp:
        known = os.path.join(tmp, "known.json")
        with open(known, "w", encoding="utf-8") as f:
            json.dump({"packs": [{"id": "colours", "name": "Colours", "label": "Col", "author": "Painter",
                                  "url": "https://example.org/colours", "match": ["colo*"],
                                  "names": {"hex": "Lime"}}]}, f)
        camos = os.path.join(tmp, "camos")

        # A mod in Sloth's layout: `All`, one folder a camouflage, and Gold.
        mod = os.path.join(tmp, "Colours v2")
        red, hexa, spec = picture(64, 64), picture(64, 64), picture(128, 128)
        for folder in ("All", "red - Crimson"):
            iwi(os.path.join(mod, "x", folder, "weapon_camo_red.iwi"), red)
            iwi(os.path.join(mod, "x", folder, "weapon_camo_menu_red.iwi"), picture(408, 152))
        iwi(os.path.join(mod, "x", "All", "weapon_camo_hex.iwi"), hexa)
        iwi(os.path.join(mod, "x", "All", "weapon_camo_menu_hex_sm.iwi"), picture(128, 128))
        iwi(os.path.join(mod, "x", "All", "detail_gold_col.iwi"), picture(16, 16))
        iwi(os.path.join(mod, "x", "All", "~detail_gold_spc-r-20g-20b-20~cf35b1a3.iwi"), spec)
        iwi(os.path.join(mod, "x", "All", "~detail_gold_spc-r-42g-42b-42~cf35b1a3.iwi"), spec)
        iwi(os.path.join(mod, "x", "Gold - rainbow", "detail_gold_col.iwi"), picture(16, 16))
        Image.fromarray(picture(320, 200)).save(os.path.join(mod, "screenshot.png"))
        done = import_one(mod, camos, known, **quiet)
        check(list(done) == ["colours"], "the pack's id is the known one: %s" % list(done))
        pack = json.load(open(os.path.join(camos, "colours", "pack.json"), encoding="utf-8"))
        by = {e["id"]: e for e in pack["camos"]}
        check((pack["name"], pack["author"], pack["url"], pack["label"]) ==
              ("Colours", "Painter", "https://example.org/colours", "Col"), "the credit comes from the known list")
        check("permission" not in pack, "a pack has no permission line")
        check(set(by) == {"red", "hex", "all-gold", "gold-rainbow-gold"}, "the camos found: %s" % sorted(by))
        check(by["red"]["name"] == "Crimson", "a folder `<x> - <name>` names the camouflage")
        check(by["hex"]["name"] == "Lime", "the known list names a camouflage")
        check(by["red"]["target"] == "weapon_camo_red" and by["all-gold"]["target"] == "gold", "the targets")
        check(by["all-gold"]["specular"] == "all-gold_spc.png" and by["all-gold"]["colour"] == "all-gold_col.png",
              "Gold carries its specular and colour maps")
        got = np.asarray(Image.open(os.path.join(camos, "colours", "all-gold_spc.png")).convert("RGBA"))
        check(np.array_equal(got, spec), "an IWI's picture, texel for texel")
        check(by["red"]["banner"] == "red_banner.png" and "banner" not in by["hex"], "the banner when the mod has one")
        check(Image.open(os.path.join(camos, "colours", "red_swatch.png")).size == (128, 128),
              "a missing preview is made from the camouflage")
        check(all(os.path.isfile(os.path.join(camos, "colours", e[k])) for e in pack["camos"]
                  for k in ("colour", "specular", "swatch", "banner") if k in e), "every picture named is in the pack")

        # Two mods that both replace Gold are two packs of one entry each.
        for n, mod_name in enumerate(("Nebula", "Lava")):
            with zipfile.ZipFile(os.path.join(tmp, mod_name + ".zip"), "w") as z:
                z.writestr("%s/iw5/images/detail_gold_col.iwi" % mod_name,
                           iwi_match._make_iwi(picture(32, 32), iwi_match.F_BGRA))
                z.writestr("%s/iw5/images/weapon_camo_menu_gold_sm.iwi" % mod_name,
                           iwi_match._make_iwi(picture(128, 128), iwi_match.F_BGRA))
            done = import_one(os.path.join(tmp, mod_name + ".zip"), camos, known, author="A%d" % n, **quiet)
            entry = done[mod_name.lower()][0]
            check(entry["name"] == mod_name and entry["id"] == "gold" and entry["target"] == "gold",
                  "a mod of Gold alone is one camouflage named after the mod")
        check(json.load(open(os.path.join(camos, "lava", "pack.json")))["author"] == "A1", "--author")

        # Whole-weapon skins: one camouflage with a tile a weapon.
        skins = os.path.join(tmp, "Jewel")
        for gun in ("ak47_iw5", "remington_msr"):
            sheet = np.full((256, 256, 4), 90, np.uint8)
            sheet[..., 3] = 255
            sheet[64:192, 64:192, :3] = picture(128, 128)[..., :3]
            iwi(os.path.join(skins, gun, "iw_06", "weapon_%s_col.iwi" % gun), sheet)
            iwi(os.path.join(skins, gun, "iw_06", "weapon_%s_nml.iwi" % gun), picture(64, 64))
        iwi(os.path.join(skins, "msr", "weapon_remington_msr_scope_col.iwi"), picture(64, 64))
        Image.fromarray(picture(64, 64)).save(os.path.join(skins, "ak47_iw5", "AK-47.png"))
        entry = import_one(skins, camos, known, **quiet)["jewel"]
        check(len(entry) == 1 and entry[0]["name"] == "Jewel" and entry[0]["target"] == "weapons",
              "weapon skins are one camouflage")
        check(set(entry[0]["weapons"]) == {"iw5_ak47", "iw5_msr"}, "its weapons: %s" % sorted(entry[0]["weapons"]))
        tile = np.asarray(Image.open(os.path.join(camos, "jewel", entry[0]["weapons"]["iw5_ak47"])).convert("RGB"))
        check(tile.shape == (128, 128, 3) and tile.std() > 40, "a weapon's tile is cut from the painted part")
        check(np.array_equal(tile, tile[:, ::-1]) and np.array_equal(tile, tile[::-1]), "and repeats without a seam")

        # Free pictures: a camouflage each, previews, frames, technical maps.
        free = os.path.join(tmp, "free")
        os.makedirs(os.path.join(free, "images", "lava"))
        Image.fromarray(picture(512, 256)).save(os.path.join(free, "images", "Tiger Stripe.png"))
        Image.fromarray(picture(32, 32)).save(os.path.join(free, "images", "tiger_stripe_n.png"))
        Image.fromarray(picture(48, 48)).save(os.path.join(free, "images", "ui_camoskin_tiger_stripe.png"))
        for n in range(3):
            Image.fromarray(picture(32, 32)).save(os.path.join(free, "images", "lava", "%d.png" % n))
        entries = import_one(free, camos, known, rename={"lava": "Molten"}, **quiet)["free"]
        by = {e["id"]: e for e in entries}
        check(set(by) == {"tiger-stripe", "lava"}, "free pictures: %s" % sorted(by))
        check(Image.open(os.path.join(camos, "free", "tiger-stripe.png")).size == (256, 128), "the longest side capped")
        check(len(by["lava"]["frames"]) == 3 and by["lava"]["fps"] == 12.0 and by["lava"]["name"] == "Molten",
              "numbered pictures are frames; --rename")
        check(by["tiger-stripe"]["swatch"] == "tiger-stripe_swatch.png", "ui_camoskin_<x> is the preview")

        # A folder of downloads.
        downloads = os.path.join(tmp, "downloads")
        os.makedirs(downloads)
        shutil.copy2(os.path.join(tmp, "Nebula.zip"), downloads)
        shutil.copytree(skins, os.path.join(downloads, "Jewel"))
        iwi(os.path.join(downloads, "weapon_camo_snow.iwi"), picture(32, 32))
        done = import_folder(downloads, os.path.join(tmp, "camos2"), known, **quiet)
        check(set(done) == {"nebula", "jewel", "downloads"}, "a folder of downloads: %s" % sorted(done))
        check(done["downloads"][0]["name"] == "Downloads", "loose pictures are a pack named after the folder")
    print("import_pack self-test: %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())

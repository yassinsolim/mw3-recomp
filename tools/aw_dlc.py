#!/usr/bin/env python3
"""A player's own downloadable content, as copied off the console's hard
disk: what is in a folder, and Advanced Warfare's update's patch out of it.

    tools/aw_dlc.py <folder>                 # the packages there, one a line
    tools/aw_dlc.py <folder> --title <id>    # another game's: 415608CB, 41560817, 415607E6
    tools/aw_dlc.py <folder> --patch <file>  # and the title update's default_mp.xexp, written to <file>

The folder is the console's Content\\0000000000000000\\<title id> (41560914,
Advanced Warfare's, unless --title names another), or its 00000002 (the map
packs; the title update is then looked for in 000B0000 beside it), or any
folder the packages were put in. Every package is read once, whole, and
checked against the hashes it keeps of itself; nothing is written but <file>.

Lines: "root <folder>" (what holds them all), "update <path>" (a title
update: content type 000B0000), "pack <path>\\t<name>" (downloadable
content: type 00000002), and "bad <path>\\t<sentence>" for a package that
is not used and why: damaged, incomplete, or another game's.
"""
import hashlib, os, struct, sys
import stfs

AW_TITLE = 0x41560914
UPDATE, CONTENT = 0x000B0000, 0x00000002
HEADER = 0xB000         # where the header ends and the first hash table begins


def header(path, wanted):
    """(content type, display name, what is wrong with it or None) of a LIVE
    or PIRS package, or None for any other file. `wanted` is the game's
    title id.

    The game's own word for a package it cannot read is "An installed
    Content Pack is corrupt", with nothing to say which, so what a copy can
    do to one is looked for here (as the app's Add DLC does, PackageCheck.kt):
    the package keeps a SHA-1 of its header (0x32C, from the content type
    on), the number of its blocks (0x395), and a SHA-1 of every block in the
    hash table before the block's group."""
    try:
        with open(path, "rb") as file:
            head = file.read(HEADER)
            if len(head) < 4 or head[:4] not in (b"LIVE", b"PIRS"):
                return None
            if len(head) < HEADER:
                return 0, os.path.basename(path), "is incomplete: copy it again from your console"
            if ((struct.unpack_from(">I", head, 0x340)[0] + 0xFFF) & 0xF000) >> 12 != 0xB:
                return None
            kind = struct.unpack_from(">I", head, 0x344)[0]
            title = struct.unpack_from(">I", head, 0x360)[0]
            name = head[0x411:0x511].decode("utf-16-be", "replace").split("\0")[0] or os.path.basename(path)
            if hashlib.sha1(head[0x344:]).digest() != head[0x32C:0x340]:
                return kind, name, "is damaged: copy it again from your console"
            if title != wanted:
                return kind, name, f"is for another game ({title:08X}), not this one"
            blocks = struct.unpack_from(">I", head, 0x395)[0]
            table = b""
            for block in range(blocks):
                at = stfs.Package.offset(None, block)
                if block % stfs.PER_TABLE == 0:
                    file.seek(at - stfs.BLOCK)
                    table = file.read(stfs.BLOCK)
                file.seek(at)
                data = file.read(stfs.BLOCK)
                if len(data) < stfs.BLOCK:
                    return kind, name, "is incomplete: copy it again from your console"
                entry = (block % stfs.PER_TABLE) * 0x18
                if hashlib.sha1(data).digest() != table[entry:entry + 0x14]:
                    return kind, name, "is damaged: copy it again from your console"
    except OSError:
        return None
    return kind, name, None


def main():
    arguments = sys.argv[1:]
    options = {"--title": f"{AW_TITLE:08X}", "--patch": None}
    while len(arguments) >= 3 and arguments[-2] in options:
        options[arguments[-2]] = arguments[-1]
        del arguments[-2:]
    if len(arguments) != 1:
        raise SystemExit(__doc__)
    try:
        wanted = int(options["--title"], 16)
    except ValueError:
        raise SystemExit(f"--title {options['--title']} is not a title id (eight hexadecimal digits)")
    folder = os.path.abspath(arguments[0])
    if not os.path.isdir(folder):
        raise SystemExit(f"{folder} is not a folder")
    root = folder
    if os.path.basename(folder).upper() == "00000002" and os.path.isdir(os.path.join(os.path.dirname(folder), "000B0000")):
        root = os.path.dirname(folder)
    print(f"root {root}")
    updates = []
    for at, _, names in os.walk(root):
        for name in sorted(names):
            path = os.path.join(at, name)
            found = header(path, wanted)
            if not found:
                continue
            if found[2]:
                print(f"bad {path}\t{found[1]} {found[2]}")
            elif found[0] == UPDATE:
                updates.append(path)
                print(f"update {path}")
            elif found[0] == CONTENT:
                print(f"pack {path}\t{found[1]}")
    if options["--patch"]:
        if not updates:
            raise SystemExit(f"no title update's package is there (the console keeps it in Content\\0000000000000000\\{wanted:08X}\\000B0000)")
        package = stfs.Package(updates[0])
        patch = next((entry for entry in package.entries if package.path(entry).lower() == "default_mp.xexp"), None)
        if patch is None:
            raise SystemExit(f"{updates[0]} has no default_mp.xexp")
        with open(options["--patch"], "wb") as out:
            out.write(package.read(patch))


if __name__ == "__main__":
    main()

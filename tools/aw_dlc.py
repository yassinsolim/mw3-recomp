#!/usr/bin/env python3
"""A player's own Advanced Warfare downloadable content, as copied off the
console's hard disk: what is in a folder, and the update's patch out of it.

    tools/aw_dlc.py <folder>                 # the packages there, one a line
    tools/aw_dlc.py <folder> --patch <file>  # and the title update's default_mp.xexp, written to <file>

The folder is the console's Content\\0000000000000000\\41560914, or its
00000002 (the map packs; the title update is then looked for in 000B0000
beside it), or any folder the packages were put in. Only the first bytes of
each file are read, and the update's package: nothing is written but <file>.

Lines: "root <folder>" (what holds them all), "update <path>" (a title
update: content type 000B0000), "pack <path>\\t<name>" (downloadable
content: type 00000002).
"""
import os, struct, sys
import stfs

TITLE = 0x41560914
UPDATE, CONTENT = 0x000B0000, 0x00000002


def header(path):
    """(content type, title id, display name) of a LIVE or PIRS package, or None."""
    try:
        with open(path, "rb") as file:
            head = file.read(0x511)
    except OSError:
        return None
    if len(head) < 0x511 or head[:4] not in (b"LIVE", b"PIRS"):
        return None
    kind = struct.unpack_from(">I", head, 0x344)[0]
    title = struct.unpack_from(">I", head, 0x360)[0]
    return kind, title, head[0x411:0x511].decode("utf-16-be").split("\0")[0]


def main():
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and sys.argv[2] != "--patch"):
        raise SystemExit(__doc__)
    folder = os.path.abspath(sys.argv[1])
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
            found = header(path)
            if not found or found[1] != TITLE:
                continue
            if found[0] == UPDATE:
                updates.append(path)
                print(f"update {path}")
            elif found[0] == CONTENT:
                print(f"pack {path}\t{found[2]}")
    if len(sys.argv) == 4:
        if not updates:
            raise SystemExit("no title update's package is there (the console keeps it in Content\\0000000000000000\\41560914\\000B0000)")
        package = stfs.Package(updates[0])
        patch = next((entry for entry in package.entries if package.path(entry).lower() == "default_mp.xexp"), None)
        if patch is None:
            raise SystemExit(f"{updates[0]} has no default_mp.xexp")
        with open(sys.argv[3], "wb") as out:
            out.write(package.read(patch))


if __name__ == "__main__":
    main()

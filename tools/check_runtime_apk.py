#!/usr/bin/env python3
"""Refuse to publish an Android app that carries the game's code or files.

    python tools/check_runtime_apk.py <app.apk>

The published app is the runtime alone (docs/game-library.md): the recompiled
code is each player's own, made from their disc, and must never be in a
release. This reads every native library in the APK, symbol by symbol, and
fails if one of them defines what only the recompiled code defines:

  * __imp__sub_XXXXXXXX, the body of a recompiled function;
  * PPCFuncMappings, the table of them;
  * more than a few hundred sub_XXXXXXXX. The runtime defines some itself:
    its hooks, named for the function each replaces (runtime/title.h). The
    recompiled code defines tens of thousands.

It also fails on a game library by name (lib*game.so) and on any game file
(the disc's executables, fastfiles, videos). Standard library only, so it
runs anywhere the release workflow does.
"""
import re
import struct
import sys
import zipfile

GAME_FILES = (".xex", ".xexp", ".ff", ".ffm", ".pak", ".bik", ".iso", ".god", ".pe")
HOOK_LIMIT = 500
FUNCTION = re.compile(r"^sub_[0-9A-F]{8}$")
BODY = re.compile(r"^__imp__sub_[0-9A-F]{8}$")


def defined_symbols(elf: bytes):
    """Names of the symbols an ELF64 little-endian library defines, from both its symbol tables."""
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise ValueError("not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]
    for _, kind, _, _, offset, size, link, _, _, entsize in sections:
        if kind not in (2, 11) or not entsize:      # SHT_SYMTAB, SHT_DYNSYM
            continue
        strings = sections[link]
        table = elf[strings[4]:strings[4] + strings[5]]
        for at in range(offset, offset + size, entsize):
            name, _, _, shndx = struct.unpack_from("<IBBH", elf, at)
            if shndx == 0:                          # undefined: a reference, not code
                continue
            end = table.find(b"\0", name)
            yield table[name:end].decode("utf-8", "replace")


def problems(apk_path: str):
    with zipfile.ZipFile(apk_path) as apk:
        libraries = 0
        for entry in apk.namelist():
            lower = entry.lower()
            if lower.endswith(GAME_FILES):
                yield f"{entry}: a game file"
            if not lower.endswith(".so"):
                continue
            libraries += 1
            if lower.endswith("game.so"):
                yield f"{entry}: a game library"
            names = set(defined_symbols(apk.read(entry)))
            bodies = sum(1 for n in names if BODY.match(n))
            functions = sum(1 for n in names if FUNCTION.match(n))
            if bodies:
                yield f"{entry}: defines {bodies} recompiled function bodies (__imp__sub_*)"
            if "PPCFuncMappings" in names:
                yield f"{entry}: defines the recompiled function table (PPCFuncMappings)"
            if functions > HOOK_LIMIT:
                yield f"{entry}: defines {functions} sub_* functions; the runtime's hooks are far fewer"
            print(f"{entry}: {len(names)} symbols defined, {functions} hooks, {bodies} recompiled bodies")
        if not libraries:
            yield "no native libraries at all: not the app"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    found = list(problems(sys.argv[1]))
    for problem in found:
        print(f"REFUSED: {problem}")
    if not found:
        print("No recompiled code and no game files in this APK.")
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main())

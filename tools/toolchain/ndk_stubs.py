#!/usr/bin/env python3
"""The library a link names in place of the phone's libc, libm or libdl.

    ndk_stubs.py <library>.map.txt <api level> <out.c> <out.map>

Android's C library says what it exports, and since which version of Android,
in a text file beside its source (bionic's libc.map.txt). A link needs no more
of the library than that: the names, and the version each is exported under.
This writes them as a C file of empty definitions and a linker version script,
which build_sysroot.sh compiles to a libc.so with nothing in it, as the NDK's
own are made from the same files. So the toolchain app carries Android's list
of names and none of the NDK.

The rules are those of AOSP's build/soong/cc/symbolfile: a version or a symbol
is left out when it is for another architecture, for a later Android than the
level asked for, or for the platform's own use.
"""
import re
import sys

ARCH = "arm64"
ARCHES = {"arm", "arm64", "riscv64", "x86", "x86_64"}
# Exported to parts of the platform only, never to an app.
PLATFORM_TAGS = {"platform-only", "apex", "llndk", "systemapi"}


def tags_of(line):
    comment = line.partition("#")[2]
    return comment.split()


def included(tags, api):
    if PLATFORM_TAGS & set(tags):
        return False
    arches = ARCHES & set(tags)
    if arches and ARCH not in arches:
        return False
    introduced = None
    for tag in tags:
        name, _, value = tag.partition("=")
        if name == "introduced-" + ARCH:
            introduced = value
            break
        if name == "introduced":
            introduced = value
    # A level that is a name is a release not out yet.
    return introduced is None or (introduced.isdigit() and int(introduced) <= api)


def versioned(tags, api):
    """Whether the symbol has its version at this level: a few had none at first."""
    for tag in tags:
        name, _, value = tag.partition("=")
        if name == "versioned":
            return int(value) <= api
    return True


def parse(text, api):
    """[(version, base version or None, [(symbol, is a variable, has the version)])], in the file's order."""
    versions = []
    current = None
    for line in text.splitlines():
        code = line.partition("#")[0].strip()
        if current is None:
            opened = re.match(r"(\S+)\s*\{", code)
            if opened:
                name = opened.group(1)
                keep = included(tags_of(line), api) and not name.endswith(("_PRIVATE", "_PLATFORM"))
                current = (name, keep, [])
            continue
        closed = re.match(r"\}\s*(\S*)\s*;", code)
        if closed:
            name, keep, symbols = current
            if keep:
                versions.append((name, closed.group(1) or None, symbols))
            current = None
        elif code.endswith(";") and ":" not in code and code != "*;":
            tags = tags_of(line)
            if included(tags, api):
                current[2].append((code[:-1].strip(), "var" in tags, versioned(tags, api)))
    return versions


def main():
    source, api, out_c, out_map = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    versions = parse(open(source, encoding="utf-8").read(), api)
    kept = {name for name, _, symbols in versions if symbols}
    bases = {name: base for name, base, _ in versions}
    with open(out_c, "w", newline="\n") as c, open(out_map, "w", newline="\n") as script:
        for name, base, symbols in versions:
            if not symbols:
                continue
            # A version with nothing left at this level is not in the library,
            # so what inherited from it inherits from the one before.
            while base is not None and base not in kept:
                base = bases.get(base)
            script.write(name + " {\n    global:\n")
            for symbol, variable, with_version in symbols:
                c.write(f"int {symbol} = 0;\n" if variable else f"void {symbol}() {{}}\n")
                # Left out of the script, a symbol is exported without a version.
                if with_version:
                    script.write(f"        {symbol};\n")
            script.write("}" + (" " + base if base else "") + ";\n")


if __name__ == "__main__":
    main()

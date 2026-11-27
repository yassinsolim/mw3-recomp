#!/usr/bin/env python3
"""Fail if proprietary game files, pointers to them, or build output are tracked by git.

Game files stay on the developer's own machine. They are never committed,
never put in Git LFS, never uploaded to a private asset repository and never
fetched by CI. Nor is anything that was built: an APK, or a file too large to
be a source (a compiler in a Gradle output folder was committed once). This
check runs in CI and can be used as a pre-commit hook:

    python3 tools/check_no_game_files.py            # check tracked files
    python3 tools/check_no_game_files.py --staged   # check what is staged
    python3 tools/check_no_game_files.py --self-test
"""
import os
import subprocess
import sys
from pathlib import PurePosixPath

FORBIDDEN_SUFFIXES = {
    ".xex", ".xexp", ".pe", ".iso", ".god", ".xbe",
    ".ff", ".ffm", ".pak", ".bik", ".xma", ".xwb", ".xsb", ".xpr",
    ".ppc_dump",
}
FORBIDDEN_DIRS = {"mw2", "mw3", "cod4", "ppc", "ppc_mw3", "ppc_mw3_mp", "ppc_mp", "ppc_tu0", "ppc_mp_tu0",
                  "ppc_cod4", "ppc_cod4_mp", "aw", "ppc_aw", "ppc_aw_mp", "bo3", "ppc_bo3", "ppc_bo3_zm"}
LFS_MAGIC = b"version https://git-lfs.github.com/spec/"
# What is built is not committed, whatever it is called.
BUILT_SUFFIXES = {".apk", ".aab"}
# The largest tracked source is a quarter of a megabyte; nothing of twenty is one.
LARGEST_FILE = 20 * 1024 * 1024
# Directories where a name like "mw3" is a legitimate source directory.
ALLOWED_PREFIXES = ("docs/", "tools/", "tests/", "config/", "runtime/", ".github/")


def violations(paths, read_head, size_of):
    bad = []
    for p in paths:
        pp = PurePosixPath(p)
        if pp.suffix.lower() in BUILT_SUFFIXES:
            bad.append((p, "a built package"))
            continue
        if size_of(p) > LARGEST_FILE:
            bad.append((p, f"{size_of(p) // (1024 * 1024)} MB: build output, not a source"))
            continue
        if pp.suffix.lower() in FORBIDDEN_SUFFIXES:
            bad.append((p, "game-file extension"))
            continue
        if pp.parts and pp.parts[0] in FORBIDDEN_DIRS and not p.startswith(ALLOWED_PREFIXES):
            bad.append((p, "game-data or generated directory"))
            continue
        if pp.name == ".gitattributes":
            continue
        if read_head(p).startswith(LFS_MAGIC):
            bad.append((p, "Git LFS pointer (game files must not go through LFS)"))
    return bad


def lfs_attributes(read_text):
    try:
        text = read_text(".gitattributes")
    except OSError:
        return []
    return [ln for ln in text.splitlines() if "filter=lfs" in ln]


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def main(argv):
    if "--self-test" in argv:
        return self_test()
    staged = "--staged" in argv
    names = git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR") if staged \
        else git("ls-files", "-z")
    paths = [n.decode() for n in names.split(b"\0") if n]

    def read_head(p):
        try:
            with open(p, "rb") as f:
                return f.read(64)
        except OSError:
            return b""

    def size_of(p):
        try:
            return os.path.getsize(p)
        except OSError:
            return 0

    bad = violations(paths, read_head, size_of)
    for ln in lfs_attributes(lambda f: open(f, encoding="utf-8").read()):
        bad.append((".gitattributes", f"LFS rule: {ln.strip()}"))
    if bad:
        print("Proprietary game data and build output must never be committed or uploaded:")
        for p, why in bad:
            print(f"  {p}: {why}")
        return 1
    print(f"ok: {len(paths)} tracked files, no game data, no build output, no LFS")
    return 0


def self_test():
    heads = {"x/lfs.bin": LFS_MAGIC + b"sha256:..."}
    rh = lambda p: heads.get(p, b"")
    sizes = {"android/toolchain/build/out/lib/arm64-v8a/libllvm.so": 89788000}
    sz = lambda p: sizes.get(p, 1000)
    assert violations(["mw3/tu0/default.xex"], rh, sz)
    assert violations(["assets/code_post_gfx.ff"], rh, sz)
    assert violations(["assets/imagefile1.pak"], rh, sz)
    assert violations(["x/lfs.bin"], rh, sz)
    assert violations(["mw3/anything.txt"], rh, sz)
    assert violations(["ppc_mw3/ppc_recomp.0.cpp"], rh, sz)
    assert violations(["cod4/notes.txt"], rh, sz)
    assert violations(["ppc_cod4_mp/ppc_recomp.0.cpp"], rh, sz)
    assert violations(["aw/tu0/notes.txt"], rh, sz)
    assert violations(["ppc_aw/ppc_recomp.0.cpp"], rh, sz)
    assert violations(["ppc_aw_mp/ppc_config.h"], rh, sz)
    assert violations(["bo3/default.xex"], rh, sz)
    assert violations(["ppc_bo3_zm/ppc_recomp.0.cpp"], rh, sz)
    assert violations(["android/toolchain/build/out/lib/arm64-v8a/libllvm.so"], rh, sz)
    assert violations(["out/toolchain/recomp-toolchain.apk"], rh, sz)
    assert not violations(["docs/MW3-BUILD.md", "tools/title.py", "runtime/main.cpp",
                           "tests/xex_header_test.cpp", "config/MW3.toml.in"], rh, sz)
    assert lfs_attributes(lambda f: "*.ff filter=lfs diff=lfs merge=lfs -text\n")
    assert not lfs_attributes(lambda f: "*.sh text eol=lf\n")
    print("self-test ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

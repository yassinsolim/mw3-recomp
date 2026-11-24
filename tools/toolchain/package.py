#!/usr/bin/env python3
"""What android/toolchain packages, put together from the builds beside this script.

    package.py <work directory> <XenonRecomp checkout> <out folder>

Writes, for -PtoolchainFiles=<out folder> (android/toolchain/build.gradle.kts):

    jniLibs/arm64-v8a/libllvm.so          clang and lld (build_llvm.sh)
    jniLibs/arm64-v8a/libXenonRecomp.so   the recompiler and its analyser
    jniLibs/arm64-v8a/libXenonAnalyse.so  (build_recompiler.sh)
    assets/toolchain/data.zip             what clang reads: build_sysroot.sh's
                                          data/ and the recompiler's headers
    assets/toolchain/toolchain.json       what is in the package and how it is
                                          run (docs/ondevice-build.md)

The programs are named lib*.so because Android unpacks nothing else into the
folder a program may be run from.
"""
import hashlib
import json
import os
import re
import shutil
import sys
import zipfile


def versions(root):
    text = open(os.path.join(root, "tools", "toolchain", "versions.sh"), encoding="utf-8").read()
    return dict(re.findall(r"^([A-Z_0-9]+)=(\S+)$", text, re.M))


def abi_hash(path):
    """A file's SHA-256 as cmake/game_abi.cmake takes it: without carriage returns."""
    return hashlib.sha256(open(path, "rb").read().replace(b"\r", b"")).hexdigest()


def add_tree(archive, folder, prefix):
    for directory, names, files in os.walk(folder):
        names.sort()
        for name in sorted(files):
            path = os.path.join(directory, name)
            inside = prefix + "/" + os.path.relpath(path, folder).replace(os.sep, "/")
            # One date for every file: the archive is the same for the same contents.
            info = zipfile.ZipInfo(inside, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, open(path, "rb").read())


def main():
    work, xenon, out = (os.path.abspath(p) for p in sys.argv[1:4])
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    version = versions(root)
    api = version["ANDROID_API"]
    clang_major = version["LLVM_VERSION"].split(".")[0]

    shutil.rmtree(out, ignore_errors=True)
    libraries = os.path.join(out, "jniLibs", "arm64-v8a")
    assets = os.path.join(out, "assets", "toolchain")
    os.makedirs(libraries)
    os.makedirs(assets)
    tools = {
        "llvm": ("libllvm.so", os.path.join(work, "llvm-android", "bin", "llvm.stripped")),
        "recomp": ("libXenonRecomp.so", os.path.join(work, "xenon-android", "XenonRecomp", "XenonRecomp.stripped")),
        "analyse": ("libXenonAnalyse.so", os.path.join(work, "xenon-android", "XenonAnalyse", "XenonAnalyse.stripped")),
    }
    for name, source in tools.values():
        shutil.copyfile(source, os.path.join(libraries, name))

    data_zip = os.path.join(assets, "data.zip")
    with zipfile.ZipFile(data_zip, "w") as archive:
        data = os.path.join(work, "data")
        for part in sorted(os.listdir(data)):
            add_tree(archive, os.path.join(data, part), part)
        # What every recompiled file includes: the register context and the
        # portable SIMD headers, where the PC's tree has them.
        add_tree(archive, os.path.join(xenon, "thirdparty", "simde"), "XenonRecomp/thirdparty/simde")
        archive.write(os.path.join(xenon, "XenonUtils", "ppc_context.h"), "XenonRecomp/XenonUtils/ppc_context.h",
                      zipfile.ZIP_DEFLATED)
        archive.write(os.path.join(xenon, "LICENSE.md"), "licences/XenonRecomp-LICENSE.md", zipfile.ZIP_DEFLATED)
        archive.write(os.path.join(xenon, "thirdparty", "simde", "COPYING"), "licences/simde-COPYING",
                      zipfile.ZIP_DEFLATED)

    setup = open(os.path.join(root, "setup", "setup.ps1"), encoding="utf-8").read()
    commit = re.search(r"XenonCommit\s*=\s*'([0-9a-f]+)'", setup).group(1)
    sysroot = "{data}/sysroot"
    library = f"{sysroot}/usr/lib/aarch64-linux-android"
    resource = f"{{data}}/lib/clang/{clang_major}"
    builtins = f"{resource}/lib/linux/libclang_rt.builtins-aarch64-android.a"
    target = f"aarch64-none-linux-android{api}"
    manifest = {
        "schema": 1,
        "version": version["TOOLCHAIN_VERSION"],
        "clang": {"version": version["LLVM_VERSION"], "target": target},
        "recompiler": {
            "commit": commit,
            "patchSha256": abi_hash(os.path.join(root, "patches", "xenonrecomp-mw2.patch")),
        },
        "data": {"asset": "toolchain/data.zip", "sha256": hashlib.sha256(open(data_zip, "rb").read()).hexdigest()},
        "tools": {key: name for key, (name, _) in tools.items()},
        "paths": {
            "simde": "XenonRecomp/thirdparty/simde",
            "context": "XenonRecomp/XenonUtils/ppc_context.h",
            "licences": "licences",
        },
        # A compile is these, then the build's own flags, -c, the source and -o.
        # They are what the NDK's CMake toolchain file gives every compile of
        # the PC build, less -g: the phone has not the memory for the debug
        # information, and the game package would be stripped of it anyway.
        "compile": [
            "{llvm}", "clang", "--driver-mode=g++", f"--target={target}", f"--sysroot={sysroot}",
            "-resource-dir", resource,
            "-DANDROID", "-fdata-sections", "-ffunction-sections", "-funwind-tables",
            "-fstack-protector-strong", "-no-canonical-prefixes", "-D_FORTIFY_SOURCE=2",
            "-Wformat", "-Werror=format-security",
        ],
        # A link is linkPrefix, the build's own options and objects, linkSuffix:
        # the command the NDK's clang++ runs for -shared -static-libstdc++ with
        # the toolchain file's linker flags, lld being run directly because
        # clang cannot find it under the name Android lets it have.
        "linkPrefix": [
            "{llvm}", "ld.lld", f"--sysroot={sysroot}", "-EL", "--fix-cortex-a53-843419",
            "-z", "now", "-z", "relro", "-z", "max-page-size=16384", "--hash-style=gnu", "--eh-frame-hdr",
            "-m", "aarch64linux", "-shared", f"{library}/{api}/crtbegin_so.o",
            f"-L{resource}/lib/linux/aarch64", f"-L{library}/{api}", f"-L{library}", f"-L{sysroot}/usr/lib",
            "--no-rosegment", "--no-undefined-version", "--fatal-warnings", "--no-undefined",
        ],
        "linkSuffix": [
            "-latomic", "-lm", "-Bstatic", "-lc++", "-Bdynamic", "-lm",
            builtins, "-l:libunwind.a", "-ldl", "-lc", builtins, "-l:libunwind.a", "-ldl",
            f"{library}/{api}/crtend_so.o",
        ],
    }
    with open(os.path.join(assets, "toolchain.json"), "w", newline="\n") as file:
        json.dump(manifest, file, indent=2)
        file.write("\n")


if __name__ == "__main__":
    main()

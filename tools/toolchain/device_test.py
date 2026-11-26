#!/usr/bin/env python3
"""Builds a title's game library on a connected phone and checks what it states.

    device_test.py <title> [--serial S] [--suffix .tc] [--jobs 8] [--opt -O2] [--package <folder>] [--pc <file>]

    title     mw2, mw2mp, mw3 or mw3mp: its executable must be in the checkout
              (mw2/tu0/default.xex and so on, as setup leaves it)
    --suffix  of the installed toolchain and probe packages (-PappSuffix)
    --package tools/toolchain/package.py's output: its toolchain.json is
              compared with the commands tools/toolchain/device_build.sh runs

The phone needs the toolchain package and the probe app (android/toolchain,
android/toolchain-probe) installed. This pushes the title's executable and the
few files of the checkout the build reads to the probe's folder, has the probe
run tools/toolchain/device_build.sh as an app, loads the library in the app,
and compares its statement (mw2_game_abi) with what an app built from this
checkout expects (cmake/game_abi.cmake, config/abi/) and, with --pc <the PC
build's game_abi.cpp>, with the PC's. Prints the build's times and memory.
Exit status 0 when the statements are the same.
"""
import argparse
import ast
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# What differs between the titles: setup/setup.ps1's table and the definitions
# CMakeLists.txt gives each.
TITLES = {
    "mw2": dict(TOML="MW2.tu0.toml", TABLES="mw2_switch_tables.tu0.toml", PPC="ppc_tu0",
                XEX="mw2/tu0/default.xex", DEFINES="-DMW2_TRACE_INDIRECT=1 -DMW2_VERSION_TU0=1"),
    "mw2mp": dict(TOML="MW2MP.tu0.toml", TABLES="mw2mp_switch_tables.tu0.toml", PPC="ppc_mp_tu0",
                  XEX="mw2/tu0/default_mp.xex",
                  DEFINES="-DMW2_TITLE_MP=1 -DMW2_TRACE_INDIRECT=1 -DMW2_VERSION_TU0=1"),
    "mw3": dict(TOML="MW3.toml", TABLES="mw3_switch_tables.toml", PPC="ppc_mw3",
                XEX="mw3/tu0/default.xex", DEFINES="-DMW2_TRACE_INDIRECT=1 -DMW3_TITLE=1"),
    "mw3mp": dict(TOML="MW3MP.toml", TABLES="mw3mp_switch_tables.toml", PPC="ppc_mw3_mp",
                  XEX="mw3/tu0/default_mp.xex",
                  DEFINES="-DMW2_TRACE_INDIRECT=1 -DMW3_TITLE=1 -DMW3_TITLE_MP=1"),
}


def text(path):
    """A file as cmake/game_abi.cmake reads it: without carriage returns."""
    return open(os.path.join(ROOT, path), "rb").read().replace(b"\r", b"")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def expected_statement(library):
    """What the app's library is built against without the disc (MW2_SPLIT=runtime)."""
    recorded = f"config/abi/{library}/"
    context = b'#pragma once\n#include "ppc_config.h"\n\n' + text("XenonRecomp/XenonUtils/ppc_context.h")
    hook = f"memory-hook {sha(text('runtime/mmio_hook.h'))}\n"
    statement = (f"title {library}\n"
                 f"generated-code {text(recorded + 'generated_code.sha256').decode().strip()}\n"
                 f"code-extent {sha(text(recorded + 'ppc_config.h'))}\n"
                 f"register-context {sha(context)}\n"
                 f"system-calls {sha(text(recorded + 'kernel_imports.list'))}\n" + hook)
    return statement, hook


def check_commands(package):
    """The script's command lines are toolchain.json's, with its own names for the folders."""
    manifest = json.load(open(os.path.join(package, "assets", "toolchain", "toolchain.json")))
    script = open(os.path.join(ROOT, "tools", "toolchain", "device_build.sh"), encoding="utf-8").read()
    script = " ".join(script.replace("\\\n", " ").replace('"', "").split())
    names = {"{llvm}": "$LLVM", "{data}/sysroot/usr/lib/aarch64-linux-android": "$LIB",
             "{data}/lib/clang/19/lib/linux": "$RT", "{data}": "$DATA"}
    for key in ("compile", "linkPrefix", "linkSuffix"):
        line = " ".join(manifest[key])
        for name, variable in names.items():
            line = line.replace(name, variable)
        if line not in script:
            sys.exit(f"tools/toolchain/device_build.sh no longer runs toolchain.json's {key}:\n  {line}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("title", choices=sorted(TITLES))
    parser.add_argument("--serial")
    parser.add_argument("--suffix", default="")
    parser.add_argument("--jobs", default="8")
    parser.add_argument("--opt", default="-O2")
    parser.add_argument("--package")
    parser.add_argument("--pc")
    arguments = parser.parse_args()
    if arguments.package:
        check_commands(arguments.package)
    library, title = arguments.title, TITLES[arguments.title]
    statement, hook = expected_statement(library)
    adb = ["adb"] + (["-s", arguments.serial] if arguments.serial else [])
    probe = "com.recomp.toolchain.probe" + arguments.suffix
    work = f"/sdcard/Android/data/{probe}/files"

    def shell(command):
        return subprocess.run(adb + ["shell", command], capture_output=True, text=True).stdout

    with tempfile.TemporaryDirectory() as stage:
        for path in ("config/" + title["TOML"], title["XEX"], "runtime/mmio_hook.h", "cmake/game_library.list",
                     "tools/toolchain/device_build.sh"):
            os.makedirs(os.path.dirname(os.path.join(stage, path)), exist_ok=True)
            shutil.copyfile(os.path.join(ROOT, path), os.path.join(stage, path))
        write = lambda name, data: open(os.path.join(stage, name), "w", newline="\n").write(data)
        write("title.env", f"LIBRARY={library}\n" + "".join(f"{k}='{v}'\n" for k, v in title.items()))
        write("abi_hook.txt", hook)
        # The header's fixed part, from the script that writes it on a PC.
        header = re.search(r'HEADER = (""".*?""")', text("tools/genshared.py").decode(), re.S).group(1)
        write("shared_header_prefix.h", ast.literal_eval(header))
        # adb's own file transfer may not write an app's folder on shared
        # storage, and an app may not read adb's; a shell may do both. The
        # app makes its folder when first started.
        shell(f"am start -n {probe}/.ProbeActivity")
        time.sleep(3)
        staging = "/data/local/tmp/recomp-probe-in"
        shell(f"rm -rf {staging} {work}/in {work}/probe.log")
        subprocess.run(adb + ["push", stage, staging], check=True, capture_output=True)
        shell(f"cp -r {staging} {work}/in && rm -rf {staging}")

    # The build runs in the app's own folder: shared storage is slow, and a
    # library can be loaded only from there. Its results are written back
    # for the group adb is in, which the app's own files are not.
    results = " && ".join(f"cat out/{name} >$WORK/{name} && chmod 660 $WORK/{name}"
                          for name in (f"lib{library}game.so", "game_abi.txt", "build.txt"))
    run = (f"rm -rf out && sh $WORK/in/tools/toolchain/device_build.sh $WORK/in $PWD/out {arguments.jobs} {arguments.opt}"
           f" && cp out/libtoolchainprobe.so . && {results}")
    shell(f"am force-stop {probe}")
    shell(f"am start -n {probe}/.ProbeActivity --es run '{run}' --es load libtoolchainprobe.so")
    log = ""
    while "\nexit " not in log and not log.startswith("exit "):
        time.sleep(5)
        log = shell(f"cat {work}/probe.log")
    time.sleep(2)
    log = shell(f"cat {work}/probe.log")
    print(log)
    built = shell(f"cat {work}/game_abi.txt")
    result = 0
    if built != statement:
        print("the statement differs from the app's:\n" + built + "expected:\n" + statement)
        result = 1
    if arguments.pc:
        same = f'R"abi({built})abi"' in open(arguments.pc, encoding="utf-8").read()
        print("PC build's statement:", "the same" if same else "differs")
        result = result or (0 if same else 1)
    if "loaded libtoolchainprobe.so" not in log:
        print("the app did not load a library the toolchain linked")
        result = 1
    print("statement:", sha(built.encode())[:12], "matches the app's" if built == statement else "")
    sys.exit(result)


if __name__ == "__main__":
    main()

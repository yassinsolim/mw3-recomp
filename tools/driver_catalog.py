#!/usr/bin/env python3
"""Checks and refreshes the graphics driver catalog (android/drivers/catalog.json,
docs/android.md "The driver catalog").

A driver is native code that runs inside the app, so the catalog pins every
package by its SHA-256 and the app downloads nothing whose hash is not in it.
This is what puts a hash there, and what says a package is what its entry
claims:

    python tools/driver_catalog.py verify            every entry: download, hash, look inside
    python tools/driver_catalog.py verify --offline  only what is in the cache already
    python tools/driver_catalog.py inspect <url|zip> what an entry for that package would say
    python tools/driver_catalog.py check             the catalog's own shape, no network

`verify` fails when a download's hash is not the entry's, when the zip has no
meta.json naming a library inside it, when the library is not an AArch64
shared object with a Vulkan entry point (vk_icdGetInstanceProcAddr,
vkGetInstanceProcAddr or the Android HAL's HMI), or when the entry's minApi is
below what the library's libc symbol versions need (every community Turnip
asks for LIBC_R, Android 11, whatever its meta.json says). Downloads are kept
in the cache folder (--cache, default <temp>/driver-catalog-cache) under their
hash.
"""
import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOG = os.path.join(ROOT, "android", "drivers", "catalog.json")

SCHEMA = 1
KINDS = {"turnip", "qualcomm", "panvk"}
VENDORS = {"adreno", "mali", "xclipse", "powervr"}
RESULTS = {"good", "broken"}
GAMES = {"*", "mw3", "mw2", "cod4", "aw", "bo3"}
# What the app takes a URL from (DriverCatalog.kt, allowedUrl): release assets
# of GitHub repositories, over HTTPS.
URL = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[^?#\s]+$")
ENTRY_POINTS = ("vk_icdGetInstanceProcAddr", "vkGetInstanceProcAddr", "HMI")
# bionic's symbol versions and the Android API that brought each.
LIBC_API = {"LIBC": 21, "LIBC_N": 24, "LIBC_O": 26, "LIBC_P": 28, "LIBC_Q": 29, "LIBC_R": 30,
            "LIBC_S": 31, "LIBC_T": 33, "LIBC_U": 34, "LIBC_V": 35, "LIBC_W": 36}


class Problem(Exception):
    pass


# ---- ELF ---------------------------------------------------------------------

def read_elf(data):
    """An AArch64 shared object's exports, NEEDED libraries and libc versions."""
    if data[:4] != b"\x7fELF":
        raise Problem("the library is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise Problem("the library is not 64-bit little-endian")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_machine != 0xB7:
        raise Problem("the library is not AArch64 (machine 0x%x)" % e_machine)
    if e_type != 3:
        raise Problem("the library is not a shared object")
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, _ = struct.unpack_from("<HHH", data, 0x3A)
    sections = []
    for index in range(e_shnum):
        name, kind, _flags, _addr, offset, size, link, info, _align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, e_shoff + index * e_shentsize)
        sections.append({"type": kind, "offset": offset, "size": size, "link": link, "info": info,
                         "entsize": entsize})

    def string(table, offset):
        start = table["offset"] + offset
        return data[start:data.index(b"\0", start)].decode("utf-8", "replace")

    exports, needed, versions = set(), [], set()
    for section in sections:
        if section["type"] == 11:  # SHT_DYNSYM
            strings = sections[section["link"]]
            for at in range(section["offset"], section["offset"] + section["size"], 24):
                name, info, _other, shndx, _value, _size = struct.unpack_from("<IBBHQQ", data, at)
                if shndx != 0 and (info >> 4) in (1, 2):   # defined, global or weak
                    exports.add(string(strings, name))
        elif section["type"] == 6:  # SHT_DYNAMIC
            strings = sections[section["link"]]
            for at in range(section["offset"], section["offset"] + section["size"], 16):
                tag, value = struct.unpack_from("<qQ", data, at)
                if tag == 0:
                    break
                if tag == 1:
                    needed.append(string(strings, value))
        elif section["type"] == 0x6FFFFFFE:  # SHT_GNU_verneed
            strings = sections[section["link"]]
            at = section["offset"]
            for _ in range(section["info"]):
                _version, count, _file, aux, following = struct.unpack_from("<HHIII", data, at)
                aux_at = at + aux
                for _ in range(count):
                    _hash, _flags, _other, name, aux_next = struct.unpack_from("<IHHII", data, aux_at)
                    versions.add(string(strings, name))
                    if not aux_next:
                        break
                    aux_at += aux_next
                if not following:
                    break
                at += following
    return {"exports": exports, "needed": needed, "versions": versions}


def needed_api(versions):
    """The Android API the library's libc symbol versions ask for."""
    return max([LIBC_API.get(version, 0) for version in versions] + [0])


# ---- a package ---------------------------------------------------------------

def inspect_package(data):
    """What a driver zip is, as DriverStore.import reads it, and what its library needs."""
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        raise Problem("not a zip archive")
    names = archive.namelist()
    for name in names:
        if name.startswith("/") or ".." in name.split("/"):
            raise Problem("the archive has an entry outside itself: %s" % name)
    metas = [name for name in names if name == "meta.json" or (name.count("/") == 1 and name.endswith("/meta.json"))]
    if not metas:
        raise Problem("no meta.json")
    folder = metas[0][:-len("meta.json")]
    try:
        meta = json.loads(archive.read(metas[0]).decode("utf-8-sig"))
    except ValueError as error:
        raise Problem("meta.json is not JSON: %s" % error)
    library = meta.get("libraryName") or ""
    if not library or folder + library not in names:
        raise Problem("meta.json names no library that is in the archive (%r)" % library)
    elf = read_elf(archive.read(folder + library))
    entry = next((name for name in ENTRY_POINTS if name in elf["exports"]), None)
    if not entry:
        raise Problem("%s exports no Vulkan entry point (%s)" % (library, ", ".join(ENTRY_POINTS)))
    return {
        "library": library,
        "metaName": meta.get("name", ""),
        "metaVersion": meta.get("driverVersion", meta.get("packageVersion", "")),
        "metaMinApi": int(meta.get("minApi", 0) or 0),
        "entryPoint": entry,
        "needed": elf["needed"],
        "libcVersions": sorted(elf["versions"] & set(LIBC_API)),
        "neededApi": needed_api(elf["versions"]),
        "files": len(names),
    }


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url, cache, expected=None, offline=False):
    """The file at [url], from the cache when it is there under [expected]."""
    os.makedirs(cache, exist_ok=True)
    if expected:
        kept = os.path.join(cache, expected + ".zip")
        if os.path.isfile(kept):
            with open(kept, "rb") as file:
                data = file.read()
            if sha256(data) == expected:
                return data
    if offline:
        return None
    request = urllib.request.Request(url, headers={"User-Agent": "driver-catalog"})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read()
    with open(os.path.join(cache, sha256(data) + ".zip"), "wb") as file:
        file.write(data)
    return data


# ---- the catalog -------------------------------------------------------------

def check_gpus(where, gpus, problems):
    if not isinstance(gpus, list) or not gpus:
        problems.append("%s: no gpus" % where)
        return
    for gpu in gpus:
        if gpu.get("vendor") not in VENDORS:
            problems.append("%s: gpu vendor %r" % (where, gpu.get("vendor")))
        low, high = gpu.get("from", 0), gpu.get("to", 0)
        if not (isinstance(low, int) and isinstance(high, int) and 0 <= low <= high):
            problems.append("%s: gpu range %r..%r" % (where, low, high))


def check_reports(where, reports, problems):
    for report in reports or []:
        if report.get("result") not in RESULTS:
            problems.append("%s: report result %r" % (where, report.get("result")))
        if report.get("game", "*") not in GAMES:
            problems.append("%s: report game %r" % (where, report.get("game")))
        if "gpus" in report:
            check_gpus(where + " report", report["gpus"], problems)
        if not report.get("source"):
            problems.append("%s: a report without a source" % where)


def check_shape(catalog):
    """The catalog's own rules, as DriverCatalog.kt reads it. Returns the problems found."""
    problems = []
    if catalog.get("schema") != SCHEMA:
        problems.append("schema is not %d" % SCHEMA)
    if not isinstance(catalog.get("version"), int) or catalog["version"] <= 0:
        problems.append("version is not a positive whole number")
    seen = set()
    for driver in catalog.get("drivers", []):
        ident = driver.get("id", "")
        where = ident or "(a driver without an id)"
        if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{2,63}", ident):
            problems.append("%s: the id is not lower-case letters, digits, '.', '_' and '-'" % where)
        if ident in seen:
            problems.append("%s: twice in the catalog" % where)
        seen.add(ident)
        for key in ("name", "version", "source", "licence", "library"):
            if not driver.get(key):
                problems.append("%s: no %s" % (where, key))
        if driver.get("kind") not in KINDS:
            problems.append("%s: kind %r" % (where, driver.get("kind")))
        if not re.fullmatch(r"[0-9a-f]{64}", driver.get("sha256", "")):
            problems.append("%s: no SHA-256" % where)
        url, asset = driver.get("url", ""), driver.get("asset", "")
        if not url and not asset:
            problems.append("%s: neither a url nor an asset the app carries" % where)
        if url and not URL.match(url):
            problems.append("%s: the url is not a GitHub release asset over HTTPS" % where)
        if not isinstance(driver.get("size", 0), int) or (url and driver.get("size", 0) <= 0):
            problems.append("%s: no size" % where)
        if not isinstance(driver.get("rank"), int):
            problems.append("%s: no rank" % where)
        if not isinstance(driver.get("minApi"), int) or driver["minApi"] < 26:
            problems.append("%s: minApi" % where)
        if not isinstance(driver.get("auto"), bool):
            problems.append("%s: auto is not true or false" % where)
        # Qualcomm's own driver is theirs: linked to, chosen by the player, never by the app.
        if driver.get("kind") == "qualcomm" and (driver.get("auto") or asset):
            problems.append("%s: a proprietary driver is neither chosen automatically nor carried" % where)
        check_gpus(where, driver.get("gpus"), problems)
        check_reports(where, driver.get("reports"), problems)
    for index, rule in enumerate(catalog.get("system", [])):
        where = "system rule %d" % index
        check_gpus(where, rule.get("gpus"), problems)
        check_reports(where, rule.get("reports"), problems)
        if not isinstance(rule.get("rank", 0), int):
            problems.append("%s: rank" % where)
    return problems


def verify_driver(driver, cache, offline, assets):
    """Returns (problems, notes) for one entry, after looking at the package itself."""
    problems, notes = [], []
    data = None
    if driver.get("asset"):
        for folder in assets:
            path = os.path.join(folder, driver["asset"])
            if os.path.isfile(path):
                with open(path, "rb") as file:
                    data = file.read()
                break
    if data is None and driver.get("url"):
        try:
            data = fetch(driver["url"], cache, driver.get("sha256"), offline)
        except Exception as error:   # the network's own failures, named
            return ["the download failed: %s" % error], notes
    if data is None:
        notes.append("not looked at (%s)" % ("not in the cache" if driver.get("url") else "the asset is not built here"))
        return problems, notes
    if sha256(data) != driver["sha256"]:
        problems.append("SHA-256 is %s, the catalog says %s" % (sha256(data), driver["sha256"]))
        return problems, notes
    if driver.get("url") and len(data) != driver.get("size"):
        problems.append("the size is %d, the catalog says %s" % (len(data), driver.get("size")))
    try:
        package = inspect_package(data)
    except Problem as error:
        problems.append(str(error))
        return problems, notes
    if package["library"] != driver["library"]:
        problems.append("the library is %s, the catalog says %s" % (package["library"], driver["library"]))
    if driver["minApi"] < package["neededApi"]:
        problems.append("minApi %d, but the library asks for %s (API %d)" % (
            driver["minApi"], package["libcVersions"][-1] if package["libcVersions"] else "?", package["neededApi"]))
    if driver["minApi"] < package["metaMinApi"]:
        problems.append("minApi %d, but meta.json says %d and the app refuses it below that" % (
            driver["minApi"], package["metaMinApi"]))
    # The runtime refuses a library named for Turnip where the chip is no Adreno (driver.cpp).
    turnip = "freedreno" in package["library"] or "turnip" in package["library"].lower()
    if turnip and any(gpu["vendor"] != "adreno" for gpu in driver["gpus"]):
        problems.append("the library's name says Turnip, which the runtime refuses off an Adreno")
    notes.append("%s, %s, needs API %d (%s), %d bytes" % (
        package["library"], package["entryPoint"], package["neededApi"],
        "/".join(package["libcVersions"]) or "no libc versions", len(data)))
    return problems, notes


def load():
    with open(CATALOG, "r", encoding="utf-8") as file:
        return json.load(file)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["verify", "check", "inspect"])
    parser.add_argument("target", nargs="?")
    parser.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "driver-catalog-cache"))
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--assets", action="append", default=[],
                        help="a folder the app's carried packages are in (drivers/<name>.zip under it)")
    arguments = parser.parse_args()

    if arguments.command == "inspect":
        if not arguments.target:
            parser.error("inspect needs a URL or a zip")
        if os.path.isfile(arguments.target):
            with open(arguments.target, "rb") as file:
                data = file.read()
        else:
            data = fetch(arguments.target, arguments.cache)
        package = inspect_package(data)
        print(json.dumps({
            "url": arguments.target if not os.path.isfile(arguments.target) else "",
            "sha256": sha256(data), "size": len(data), "library": package["library"],
            "minApi": max(package["neededApi"], package["metaMinApi"], 26),
            "_meta": {key: package[key] for key in ("metaName", "metaVersion", "metaMinApi", "entryPoint",
                                                    "needed", "libcVersions")},
        }, indent=2))
        return 0

    catalog = load()
    problems = check_shape(catalog)
    for problem in problems:
        print("catalog: " + problem)
    failed = bool(problems)
    if arguments.command == "verify" and not problems:
        assets = arguments.assets + [os.path.join(ROOT, "android", "app", "src", "mw3", "assets")]
        for driver in catalog["drivers"]:
            found, notes = verify_driver(driver, arguments.cache, arguments.offline, assets)
            for note in notes:
                print("%-34s %s" % (driver["id"], note))
            for problem in found:
                print("%-34s PROBLEM: %s" % (driver["id"], problem))
            failed = failed or bool(found)
    print("catalog version %s: %d drivers, %s" % (
        catalog.get("version"), len(catalog.get("drivers", [])), "PROBLEMS" if failed else "ok"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

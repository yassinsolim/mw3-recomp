#!/usr/bin/env python3
"""Reject unsupported MW3 inputs before generating address-dependent code."""
import argparse
import hashlib
from pathlib import Path

# Retail packagings of the same iw5sp_ship.exe build. Each was recompiled with
# config/MW3.toml and gave the same 128 jump tables and no boundary errors;
# only the second (USA/Europe disc, media id 30775D59, executable version 2)
# also ran the bounded headless start-up and is the one the published app
# installs (android-release.yml, CMakeLists.txt's default, the app's kit
# table; tests/xex_hashes_test.cmake). The first (executable version 22, the
# bring-up's) is installed only by a build that has it in mw3/tu0.
SUPPORTED_SHA256 = (
    "d90bc8d93b7b8f6001ead8330271196a6ef7a5c2e4a9e9b63118f47d58a05912",
    "e94de720373d668a2bd592dfc74339bfbbc98cca993176fcd055b1449633bcd1",
)

# The multiplayer executable, iw5mp_ship.exe (default_mp.xex), of the same
# disc as the second entry above (media id 30775D59): title id 415608CB,
# version 0x00000002, entry 0x824DBAE0, image size 0x1FA0000. Recompiled with
# config/MW3MP.toml: 156 jump tables, no boundary errors
# (docs/MW3-MP-BRINGUP.md). Checked by file name, so neither executable
# passes for the other.
SUPPORTED_SHA256_MP = (
    "0d7669844122b8f94e29ec87baf6bf0a7b84ab4abad8a82b43aa444682f90f7c",
)


def supported(path: Path) -> tuple[str, ...]:
    return SUPPORTED_SHA256_MP if path.name.lower() == "default_mp.xex" else SUPPORTED_SHA256


def verify(path: Path, expected: tuple[str, ...] | None = None) -> None:
    if expected is None:
        expected = supported(path)
    with path.open("rb") as stream:
        prefix = stream.read(128)
        if prefix.startswith(b"version https://git-lfs.github.com/spec/v1"):
            raise ValueError("MW3 XEX is an LFS pointer; supply the actual executable")
        if prefix[:4] != b"XEX2":
            raise ValueError("MW3 executable does not have an XEX2 header")
        digest = hashlib.sha256(prefix)
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() not in expected:
        raise ValueError("Unsupported MW3 executable; refusing to use fixed title addresses")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path, nargs="?", default=Path("mw3/tu0/default.xex"))
    args = parser.parse_args()
    try:
        verify(args.xex)
    except (OSError, ValueError) as error:
        # Do not print input bytes, derived keys or the input's actual digest.
        if isinstance(error, OSError):
            print("MW3 executable is missing or unreadable")
        else:
            print(error)
        return 1
    print("Supported MW3 executable verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

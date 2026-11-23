#!/usr/bin/env python3
"""Reject unsupported Call of Duty 4 inputs before generating address-dependent code."""
import argparse
import hashlib
from pathlib import Path

# The campaign, CoD3.exe (default.xex), of the USA/Europe disc: title id
# 415607E6, version 0x00000001 (the disc's own build: base version the same),
# entry 0x820E4600, image size 0x1AE0000. Recompiled with config/COD4.toml: 178
# jump tables, no boundary errors (docs/COD4-BRINGUP.md).
SUPPORTED_SHA256 = (
    "f6b848ac4ecf2b12881223e4c3cd63c49ae36e1fadc6f2179428033274602739",
)

# The multiplayer, CoD3MP.exe (default_mp.xex), of the same disc: entry
# 0x82103AC8, image size 0x33D0000. Recompiled with config/COD4MP.toml: 177
# jump tables, no boundary errors. Checked by file name, so neither executable
# passes for the other.
SUPPORTED_SHA256_MP = (
    "9aae8db5b38f0b394f9052d1ee9c8b1f18038ff86fc5f4110f173f66043a0cc6",
)


def supported(path: Path) -> tuple[str, ...]:
    return SUPPORTED_SHA256_MP if path.name.lower() == "default_mp.xex" else SUPPORTED_SHA256


def verify(path: Path, expected: tuple[str, ...] | None = None) -> None:
    if expected is None:
        expected = supported(path)
    with path.open("rb") as stream:
        prefix = stream.read(128)
        if prefix[:4] != b"XEX2":
            raise ValueError("CoD4 executable does not have an XEX2 header")
        digest = hashlib.sha256(prefix)
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() not in expected:
        raise ValueError("Unsupported CoD4 executable; refusing to use fixed title addresses")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path, nargs="?", default=Path("cod4/tu0/default.xex"))
    args = parser.parse_args()
    try:
        verify(args.xex)
    except (OSError, ValueError) as error:
        # Do not print input bytes, derived keys or the input's actual digest.
        if isinstance(error, OSError):
            print("CoD4 executable is missing or unreadable")
        else:
            print(error)
        return 1
    print("Supported CoD4 executable verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

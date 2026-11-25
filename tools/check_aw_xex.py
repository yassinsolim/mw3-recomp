#!/usr/bin/env python3
"""Reject unsupported Advanced Warfare inputs before generating address-dependent code."""
import argparse
import hashlib
from pathlib import Path

# The campaign, default.pe (default.xex), of the USA/Europe game disc: title id
# 41560914, version 0x0000000A (the disc's own build: base version the same),
# media id 4FA17C64, entry 0x8263ED28, image size 0x2D90000. Recompiled with
# config/AW.toml: 210 jump tables, no boundary errors (docs/AW-BRINGUP.md).
# The install disc's default.xex is another build of it (version 7, SHA-256
# 740982a8...1eb2): the installer, not recompiled and not accepted here.
SUPPORTED_SHA256 = (
    "a8b1e99c27d6d03a0b0700e6eada1e7d24ee4b194e3c9fa844f16c25f8376a73",
)

# The multiplayer, default_mp.pe (default_mp.xex), of the same disc: entry
# 0x8213A6D0, image size 0x2CB0000. Recompiled with config/AWMP.toml: 230 jump
# tables, no boundary errors. Checked by file name, so neither executable
# passes for the other.
SUPPORTED_SHA256_MP = (
    "3ef96359f96ee7994e43de70f56a9c2a019d5b841f11318ca2dffdb717b265f8",
)

# The multiplayer as title update 17 makes it (aw/tu17/default_mp.xex, the
# update's default_mp.xexp applied to the disc's): entry 0x82145960, image
# size 0x2F60000, recompiled with config/AWMP.tu17.toml. This is the SHA-256
# of its image, the file from the header's end on, and not of the file: the
# updates of two pressings of the disc (base versions 0x10 and 0x15) make the
# same image under headers that differ.
UPDATE_IMAGE_SHA256_MP = (
    "9112629a870d860e17f4f30abe3c1c60ae2919d9697e58ddf12552ab74efaf55",
)


def is_update(path: Path) -> bool:
    return path.resolve().parent.name.lower() == "tu17"


def supported(path: Path) -> tuple[str, ...]:
    if is_update(path):
        return UPDATE_IMAGE_SHA256_MP
    return SUPPORTED_SHA256_MP if path.name.lower() == "default_mp.xex" else SUPPORTED_SHA256


def verify(path: Path, expected: tuple[str, ...] | None = None) -> None:
    if expected is None:
        expected = supported(path)
    with path.open("rb") as stream:
        prefix = stream.read(128)
        if prefix[:4] != b"XEX2":
            raise ValueError("Advanced Warfare executable does not have an XEX2 header")
        digest = hashlib.sha256(prefix)
        if is_update(path):
            # The image alone: from the offset the header gives (its third word).
            stream.seek(int.from_bytes(prefix[8:12], "big"))
            digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() not in expected:
        raise ValueError("Unsupported Advanced Warfare executable; refusing to use fixed title addresses")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path, nargs="?", default=Path("aw/tu0/default.xex"))
    args = parser.parse_args()
    try:
        verify(args.xex)
    except (OSError, ValueError) as error:
        # Do not print input bytes, derived keys or the input's actual digest.
        if isinstance(error, OSError):
            print("Advanced Warfare executable is missing or unreadable")
        else:
            print(error)
        return 1
    print("Supported Advanced Warfare executable verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

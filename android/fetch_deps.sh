#!/usr/bin/env bash
# The dependencies the Android build has that are not fetched by CMake:
# libadrenotools, which is what lets a driver the player imported be loaded
# on a phone that has not been rooted, and the Turnip driver the apps bundle
# (last step below).
#
# Neither is vendored here -- they are somebody else's code, under their own
# licences, and libadrenotools carries submodules. The app's Gradle build
# stops without them (android/app/build.gradle.kts, bundledDriverCheck): an
# app built without them runs on the driver the phone shipped with, which is
# only built on purpose, with -PwithoutBundledDriver=true.
set -euo pipefail

REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${REPOSITORY_ROOT}/third_party/libadrenotools"
REMOTE="${ADRENOTOOLS_REMOTE:-https://github.com/bylaws/libadrenotools.git}"
# The commit every build so far was made with. The one named here before is
# not in that repository: a first run's clone of it failed quietly and took
# the default branch instead, and a second run's fetch of it failed outright
# ("not our ref"), which stopped setup.sh whenever it was run again.
REVISION="${ADRENOTOOLS_REVISION:-8fae8ce254dfc1344527e05301e43f37dea2df80}"

# A clone cannot name a commit, so the commit is fetched into the clone: a
# first run and a later one end on the same code.
if [ -d "${TARGET}/.git" ]; then
    echo "libadrenotools is already there; updating it."
else
    echo "Cloning libadrenotools into ${TARGET}"
    git clone --depth 1 "${REMOTE}" "${TARGET}"
fi
git -C "${TARGET}" fetch --depth 1 origin "${REVISION}"
git -C "${TARGET}" checkout --force FETCH_HEAD

# Its own dependency: the linker shim it loads the driver through.
git -C "${TARGET}" submodule update --init --recursive --depth 1

# XenonRecomp's sources, at the commit and with the patch the recompiled code
# is made with (build.sh, setup/setup.ps1). The app's library is compiled
# against its register context and carries its XEX patcher, and Modern
# Warfare 3's app is built without the recompiled code at all
# (cmake/game_abi.cmake), so a machine with no disc -- the release workflow's
# -- needs the checkout and none of its tools built. A checkout already there
# is whoever's build made it, and is left alone.
XENON="${REPOSITORY_ROOT}/XenonRecomp"
XENON_COMMIT="$(sed -n 's/^XENON_COMMIT=//p' "${REPOSITORY_ROOT}/build.sh")"
if [ -e "${XENON}/XenonUtils/ppc_context.h" ]; then
    echo "XenonRecomp is already there."
else
    echo "Cloning XenonRecomp at ${XENON_COMMIT}"
    git clone -c core.autocrlf=false https://github.com/hedge-dev/XenonRecomp.git "${XENON}"
    git -C "${XENON}" checkout -q "${XENON_COMMIT}"
    git -C "${XENON}" submodule update --init --recursive
    git -C "${XENON}" apply "${REPOSITORY_ROOT}/patches/xenonrecomp-mw2.patch"
fi

# The Turnip (Mesa, MIT) Vulkan driver the apps unpack and use by default on
# Qualcomm phones, where the stock driver is slow and draws corrupted textures
# (and, in Modern Warfare 2, leaves surfaces out for minutes while it compiles
# their pipelines). Downloaded, never committed (THIRD_PARTY.md); it is kept
# under the mw3 flavour's assets, the MW2 flavours package a copy of it
# (android/app/build.gradle.kts, bundledDriverAssets), and a build without it
# has no bundled driver, which Gradle refuses unless it is asked for.
TURNIP_URL="${TURNIP_URL:-https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v39/Turnip_v27.0.0-R2.zip}"
TURNIP_SHA256="8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781"
TURNIP_ASSET="${REPOSITORY_ROOT}/android/app/src/mw3/assets/drivers/turnip_v27_r2.zip"

# The file goes in on standard input: given a name with a backslash in it (a
# Windows shell whose pwd is D:\...), sha256sum puts a backslash before the
# hash, and the good download was thrown away as a bad one.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum < "$1" | cut -d' ' -f1
    else shasum -a 256 < "$1" | cut -d' ' -f1
    fi
}

if [ -f "${TURNIP_ASSET}" ] && [ "$(sha256_of "${TURNIP_ASSET}")" = "${TURNIP_SHA256}" ]; then
    echo "The bundled Turnip driver is already there."
else
    echo "Downloading the bundled Turnip driver"
    mkdir -p "$(dirname "${TURNIP_ASSET}")"
    # Into a temporary name first: a download that fails the check must not
    # be left where Gradle would package it.
    curl -fsSL -o "${TURNIP_ASSET}.part" "${TURNIP_URL}"
    if [ "$(sha256_of "${TURNIP_ASSET}.part")" != "${TURNIP_SHA256}" ]; then
        rm -f "${TURNIP_ASSET}.part"
        echo "The Turnip download does not match the expected SHA-256; not using it." >&2
        exit 1
    fi
    mv "${TURNIP_ASSET}.part" "${TURNIP_ASSET}"
fi

# The Turnip the apps carry for Android 10, where the one above cannot load
# (it needs Android 11's libc), and for the Adreno 8xx, where the one above
# predates Mesa's a8xx fixes: the project's own build of Mesa, made here by
# android/build_turnip.sh, never committed (THIRD_PARTY.md). It needs Linux
# or WSL with Meson and the NDK; without them the apps are built without it
# and an Android 10 Snapdragon or an Adreno 8xx has no bundled driver
# (DriverStore.kt). The release workflows set REQUIRE_TURNIP_API29=1, which
# makes that an error.
TURNIP29_ASSET="${REPOSITORY_ROOT}/android/app/src/mw3/assets/drivers/turnip_api29.zip"
TURNIP29_COMMIT="$(sed -n 's/^MESA_COMMIT="\(.*\)"$/\1/p' "${REPOSITORY_ROOT}/android/build_turnip.sh")"
turnip29_current() {
    [ -f "${TURNIP29_ASSET}" ] && python3 - "${TURNIP29_ASSET}" "${TURNIP29_COMMIT:0:10}" <<'EOF'
import json, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    sys.exit(0 if sys.argv[2] in json.loads(z.read("meta.json"))["driverVersion"] else 1)
EOF
}
if turnip29_current; then
    echo "The project's Turnip driver (Android 10, Adreno 8xx) is already there."
else
    echo "Building the project's Turnip driver for Android 10 and the Adreno 8xx (Mesa ${TURNIP29_COMMIT:0:10}; a few minutes)"
    if ! bash "${REPOSITORY_ROOT}/android/build_turnip.sh" "${TURNIP29_ASSET}"; then
        rm -f "${TURNIP29_ASSET}"
        if [ "${REQUIRE_TURNIP_API29:-0}" = 1 ]; then
            echo "The project's Turnip driver could not be built." >&2
            exit 1
        fi
        echo "The project's Turnip driver could not be built: the apps will have no bundled driver on Android 10 or an Adreno 8xx." >&2
    fi
fi

# The camouflage pack the multiplayer app carries (docs/textures.md,
# "Camouflage packs"): the project's own, drawn here by tools/camo_gen.py and
# never committed. It needs Python with Pillow and numpy; without them the
# app is built without the pack and lists the game's camouflages alone. Other
# authors' packs are not in the app: it fetches or converts them itself
# (CamoPacksActivity.kt), by the list of known packs copied beside.
CAMO_ASSETS="${REPOSITORY_ROOT}/android/app/src/mw3/assets"
CAMO_PACK="${CAMO_ASSETS}/camos/mw3-recomp-originals"
if python3 -c 'import PIL, numpy' >/dev/null 2>&1; then
    echo "Drawing the bundled camouflage pack"
    python3 "${REPOSITORY_ROOT}/tools/camo_gen.py" --pack "${CAMO_PACK}"
else
    echo "No Python with Pillow and numpy: the multiplayer app is built without its camouflage pack." >&2
fi
mkdir -p "${CAMO_ASSETS}"
cp "${REPOSITORY_ROOT}/setup/camo-packs.json" "${CAMO_ASSETS}/camo-packs.json"

echo
echo "Done. The next Gradle build will find libadrenotools (cmake/android.cmake)"
echo "and the apps will carry the Turnip driver."

#!/usr/bin/env bash
# The Turnip (Mesa, MIT) driver the apps carry for Android 10: Mesa at a
# pinned commit, only the freedreno Vulkan driver on KGSL, compiled with the
# NDK for API 29 and packaged in the layout DriverStore and libadrenotools
# read (meta.json + the library). android/fetch_deps.sh runs it.
#
# Why our own build: every community Turnip package is compiled for API 30 or
# later and imports C11 <threads.h> from bionic as LIBC_R, which Android 10's
# linker refuses, whatever minApi their meta.json claims. Compiled for API 29
# the NDK's own inline versions are used and the newest libc version asked
# for is LIBC_P (docs/drafts/older-hardware.md, "Building it ourselves").
# Nothing is patched, so a bug found with it can be reported upstream.
#
#   android/build_turnip.sh <output.zip>
#
# Needs Linux (or WSL) with git, python3 with meson >= 1.4, mako and PyYAML,
# ninja, bison, flex and glslangValidator, and the NDK the app is built with
# (ANDROID_HOME/ndk/28.2.13676358, or TURNIP_NDK). The same commit, NDK
# and Meson give the same zip, byte for byte: paths are mapped out of the
# objects (-ffile-prefix-map) and the zip's dates and modes are fixed.
set -euo pipefail

OUTPUT="${1:?usage: build_turnip.sh <output.zip>}"
MESA_REMOTE="${MESA_REMOTE:-https://gitlab.freedesktop.org/mesa/mesa.git}"
MESA_COMMIT="483aff363be4d6913017b834123c8eeaada171d9"
NDK_VERSION="28.2.13676358"
# The pinned NDK under the SDK first: a CI image's ANDROID_NDK_HOME is
# whichever NDK it shipped with.
NDK="${TURNIP_NDK:-${ANDROID_HOME:-/opt/android-sdk}/ndk/${NDK_VERSION}}"
[ -d "${NDK}" ] || NDK="${ANDROID_NDK_HOME:-${NDK}}"
# TURNIP_WORK keeps the Mesa tree and the build for another run; without it
# both go in a temporary folder that is removed at the end.
if [ -n "${TURNIP_WORK:-}" ]; then
    WORK="${TURNIP_WORK}"
else
    WORK="$(mktemp -d)"
    trap 'rm -rf "${WORK}"' EXIT
fi

case "$(uname -s)" in
    Linux) host=linux-x86_64 ;;
    Darwin) host=darwin-x86_64 ;;
    *) echo "build_turnip.sh needs Linux or macOS (WSL on Windows)" >&2; exit 1 ;;
esac
BIN="${NDK}/toolchains/llvm/prebuilt/${host}/bin"
if [ ! -x "${BIN}/aarch64-linux-android29-clang" ]; then
    echo "No NDK ${NDK_VERSION} at ${NDK} (set ANDROID_NDK_HOME)" >&2
    exit 1
fi
for tool in git python3 meson ninja bison flex glslangValidator; do
    command -v "${tool}" >/dev/null || { echo "build_turnip.sh needs ${tool}" >&2; exit 1; }
done
python3 -c 'import mako, yaml' 2>/dev/null || { echo "build_turnip.sh needs python3's mako and PyYAML" >&2; exit 1; }

# One commit, fetched by its name, into a fresh tree.
SRC="${WORK}/mesa"
if [ ! -d "${SRC}/.git" ]; then
    git init -q "${SRC}"
    git -C "${SRC}" remote add origin "${MESA_REMOTE}"
fi
git -C "${SRC}" fetch -q --depth 1 origin "${MESA_COMMIT}"
git -C "${SRC}" checkout -q --force FETCH_HEAD
VERSION="$(cat "${SRC}/VERSION")"
SHORT="${MESA_COMMIT:0:10}"

# The NDK's API 29 compilers. armv8-a, the NDK's default: Adreno 610's
# Snapdragon 662/665 have Cortex-A73/A53 cores, which have no 8.2 extensions.
mkdir -p "${WORK}/empty-pkgconfig"
map="-ffile-prefix-map=${SRC}=mesa -ffile-prefix-map=${WORK}/build=build"
cat > "${WORK}/android29-aarch64.txt" <<EOF
[binaries]
ar = '${BIN}/llvm-ar'
c = ['${BIN}/aarch64-linux-android29-clang']
cpp = ['${BIN}/aarch64-linux-android29-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '${BIN}/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=${WORK}/empty-pkgconfig', 'pkg-config']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

rm -rf "${WORK}/build"
meson setup "${WORK}/build" "${SRC}" --cross-file "${WORK}/android29-aarch64.txt" \
    -Dbuildtype=release -Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dvulkan-beta=true -Dfreedreno-kmds=kgsl \
    -Dstrip=true -Degl=disabled -Dandroid-libbacktrace=disabled \
    "-Dc_args=${map}" "-Dcpp_args=${map}" > "${WORK}/setup.log"
ninja -C "${WORK}/build" src/freedreno/vulkan/libvulkan_freedreno.so > "${WORK}/ninja.log"
LIB="${WORK}/build/src/freedreno/vulkan/libvulkan_freedreno.so"

# Android 10's bionic or older only: a LIBC_Q or later version would not load there.
if "${BIN}/llvm-readelf" -V "${LIB}" | grep -qE 'Name: LIBC_[Q-Z]'; then
    echo "libvulkan_freedreno.so asks for a libc newer than Android 9's:" >&2
    "${BIN}/llvm-readelf" -V "${LIB}" | grep -oE 'Name: LIBC_[A-Z]+' | sort -u >&2
    exit 1
fi

# Packaged as DriverStore.import reads it, with dates and modes fixed.
python3 - "${OUTPUT}" "${LIB}" "${VERSION}" "${SHORT}" <<'EOF'
import json, sys, zipfile
output, library, version, short = sys.argv[1:]
meta = {
    "schemaVersion": 1,
    "name": f"Turnip {version} ({short}) for Android 10",
    "description": f"Mesa {version} at {short}, freedreno Vulkan on KGSL, built by mw3-android "
                   "(android/build_turnip.sh) with the NDK for API 29. MIT licence.",
    "author": "Mesa contributors; built by mw3-android",
    "packageVersion": "1",
    "vendor": "Mesa",
    "driverVersion": f"Mesa {version} git {short}",
    "minApi": 29,
    "libraryName": "libvulkan_freedreno.so",
}
def add(archive, name, data):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.external_attr = 0o644 << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data, compresslevel=9)
with zipfile.ZipFile(output + ".part", "w") as archive:
    add(archive, "meta.json", (json.dumps(meta, indent=2) + "\n").encode())
    with open(library, "rb") as f:
        add(archive, "libvulkan_freedreno.so", f.read())
EOF
mv "${OUTPUT}.part" "${OUTPUT}"
sha256sum "${OUTPUT}" 2>/dev/null || shasum -a 256 "${OUTPUT}"

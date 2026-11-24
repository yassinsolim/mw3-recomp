#!/bin/bash
# XenonRecomp and XenonAnalyse for the phone: the pinned commit with this
# repository's patch (android/fetch_deps.sh clones it), built with the NDK.
# They are the programs the PC runs, so the tree the phone recompiles is the
# PC's, byte for byte.
#
#   tools/toolchain/build_recompiler.sh <work directory>
#   Output: <work>/xenon-android/XenonRecomp/XenonRecomp.stripped
#           <work>/xenon-android/XenonAnalyse/XenonAnalyse.stripped
#   Environment: NDK, CMAKE, NINJA as build_llvm.sh; XENON, the checkout
#                (XenonRecomp/ of this repository).
set -euo pipefail
WORK=${1:?usage: $0 <work directory>}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/toolchain/versions.sh"
: "${NDK:?NDK must name an Android NDK}"
CMAKE=${CMAKE:-cmake}
NINJA=${NINJA:-ninja}
XENON=${XENON:-$ROOT/XenonRecomp}
[ -f "$XENON/XenonRecomp/register_flow.cpp" ] || {
    echo "$XENON is not the patched recompiler; run android/fetch_deps.sh" >&2
    exit 1
}

BUILD=$WORK/xenon-android
"$CMAKE" -S "$XENON" -B "$BUILD" -G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$ANDROID_API" -DANDROID_STL=c++_static \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384" >"$WORK/xenon-android-configure.log"
"$CMAKE" --build "$BUILD" --target XenonAnalyse XenonRecomp >"$WORK/xenon-android-build.log"
for program in XenonRecomp XenonAnalyse; do
    "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-all \
        -o "$BUILD/$program/$program.stripped" "$BUILD/$program/$program"
    ls -l "$BUILD/$program/$program.stripped"
done

#!/bin/bash
# The build tools' package for building the game code on the phone
# (docs/ondevice-build.md): com.recomp.toolchain, from sources alone.
#
#   tools/toolchain/build.sh <work directory> [gradle arguments, e.g. -PappSuffix=.tc]
#   Output: out/toolchain/recomp-toolchain.apk
#   Environment: NDK (r28), CMAKE, NINJA, PYTHON, HOST_CC, HOST_CXX; for the
#                release's signature MW2_KEYSTORE and the rest, as the app's build.
#   Prereqs: cmake ninja python3 curl tar, a host C++ compiler, a JDK 17, the
#            Android SDK android/local.properties names, XenonRecomp/
#            (android/fetch_deps.sh).
#   Time: about 15 minutes on 32 threads, nearly all of it LLVM; the work
#         directory keeps what is built, and a second run is a minute.
set -euo pipefail
WORK=${1:?usage: $0 <work directory> [gradle arguments]}
shift
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PYTHON=${PYTHON:-python3}
export XENON=${XENON:-$ROOT/XenonRecomp}

"$ROOT/tools/toolchain/build_llvm.sh" "$WORK"
"$ROOT/tools/toolchain/build_sysroot.sh" "$WORK"
"$ROOT/tools/toolchain/build_recompiler.sh" "$WORK"
WORK=$(cd "$WORK" && pwd)
"$PYTHON" "$ROOT/tools/toolchain/package.py" "$WORK" "$XENON" "$WORK/package"

cd "$ROOT/android"
./gradlew --no-daemon "-PtoolchainFiles=$WORK/package" "$@" :toolchain:assembleRelease
mkdir -p "$ROOT/out/toolchain"
cp toolchain/build/outputs/apk/release/toolchain-release.apk "$ROOT/out/toolchain/recomp-toolchain.apk"
ls -l "$ROOT/out/toolchain/recomp-toolchain.apk"

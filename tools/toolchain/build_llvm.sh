#!/bin/bash
# The compiler and linker of the on-device toolchain (docs/ondevice-build.md):
# clang and lld of an LLVM release, cross-built for arm64 Android as one
# executable, and the compiler's own headers.
#
#   tools/toolchain/build_llvm.sh <work directory>
#   Output: <work>/llvm-android/bin/llvm            clang and lld in one file
#           <work>/llvm-android/lib/clang/19/include the compiler's headers
#   Environment: NDK (an Android NDK r28), CMAKE, NINJA, HOST_CC, HOST_CXX
#                (the compiler of the machine this runs on; cc and c++).
#   Prereqs: cmake, ninja, a host C++ compiler, curl, tar with xz.
#
# Why one executable: Android runs a program an app brings only from the
# app's library folder, where every megabyte is on the phone twice (in the
# package and unpacked); clang and lld share most of LLVM, and linked apart
# they would carry it twice.
set -euo pipefail
WORK=${1:?usage: $0 <work directory>}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/toolchain/versions.sh"
: "${NDK:?NDK must name an Android NDK}"
CMAKE=${CMAKE:-cmake}
NINJA=${NINJA:-ninja}
HOST_CC=${HOST_CC:-cc}
HOST_CXX=${HOST_CXX:-c++}

SRC=$WORK/llvm-project-$LLVM_VERSION.src
if [ ! -d "$SRC/llvm" ]; then
    ARCHIVE=$WORK/llvm-project-$LLVM_VERSION.src.tar.xz
    [ -f "$ARCHIVE" ] || curl -fsSL --retry 3 -o "$ARCHIVE" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-project-$LLVM_VERSION.src.tar.xz"
    echo "$LLVM_SHA256  $ARCHIVE" | sha256sum -c - >/dev/null
    # Only what is built; the rest is most of the archive, and libclc has
    # links a Windows checkout cannot make.
    tar -xf "$ARCHIVE" -C "$WORK" \
        "llvm-project-$LLVM_VERSION.src/llvm" "llvm-project-$LLVM_VERSION.src/clang" \
        "llvm-project-$LLVM_VERSION.src/lld" "llvm-project-$LLVM_VERSION.src/cmake" \
        "llvm-project-$LLVM_VERSION.src/third-party" "llvm-project-$LLVM_VERSION.src/libunwind" \
        "llvm-project-$LLVM_VERSION.src/libcxx" "llvm-project-$LLVM_VERSION.src/libcxxabi" \
        "llvm-project-$LLVM_VERSION.src/compiler-rt" "llvm-project-$LLVM_VERSION.src/runtimes" \
        "llvm-project-$LLVM_VERSION.src/LICENSE.TXT"
fi

# What is the same for the host's tools and the phone's: clang and lld for
# arm64 alone, without the optional libraries a phone has not got.
COMMON=(-G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA" -DCMAKE_BUILD_TYPE=Release
    "-DLLVM_ENABLE_PROJECTS=clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_PLUGINS=OFF
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF)

# The table generators run on this machine while the phone's compiler is built.
HOST=$WORK/llvm-host
"$CMAKE" -S "$SRC/llvm" -B "$HOST" "${COMMON[@]}" \
    "-DCMAKE_C_COMPILER=$HOST_CC" "-DCMAKE_CXX_COMPILER=$HOST_CXX" >"$WORK/llvm-host-configure.log"
"$CMAKE" --build "$HOST" --target llvm-min-tblgen llvm-tblgen clang-tblgen >"$WORK/llvm-host-build.log"

# -O2 and no assertions, as LLVM's own releases: the compiler's speed is the
# player's wait. The linker and the C++ library are the defaults the NDK's
# clang has, so the same command line means the same thing.
TARGET=$WORK/llvm-android
"$CMAKE" -S "$SRC/llvm" -B "$TARGET" "${COMMON[@]}" \
    "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$ANDROID_API" -DANDROID_STL=c++_static \
    "-DLLVM_NATIVE_TOOL_DIR=$HOST/bin" \
    -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-android "-DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-none-linux-android$ANDROID_API" \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON "-DLLVM_DISTRIBUTION_COMPONENTS=clang;lld;clang-resource-headers" \
    -DCLANG_DEFAULT_LINKER=lld -DCLANG_DEFAULT_CXX_STDLIB=libc++ -DCLANG_DEFAULT_RTLIB=compiler-rt \
    -DCLANG_DEFAULT_UNWINDLIB=libunwind \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384" >"$WORK/llvm-android-configure.log"
"$CMAKE" --build "$TARGET" --target llvm-driver clang-resource-headers >"$WORK/llvm-android-build.log"
"$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-all -o "$TARGET/bin/llvm.stripped" "$TARGET/bin/llvm"
ls -l "$TARGET/bin/llvm.stripped"

#!/bin/bash
# What the on-device toolchain compiles and links against, made from sources
# whose licences let it be given away (docs/ondevice-build.md, THIRD_PARTY.md):
# nothing in it is copied out of the Android NDK, which is only the compiler
# this script runs.
#
#   tools/toolchain/build_sysroot.sh <work directory>      (after build_llvm.sh)
#   Output: <work>/data/                 laid out as the NDK's toolchain is, so
#                                        the PC build's command lines apply
#             sysroot/usr/include        Android's C library headers (AOSP bionic)
#             sysroot/usr/include/c++/v1 libc++'s headers (LLVM)
#             sysroot/usr/lib/aarch64-linux-android/[26/]
#                                        libc++, and for the link: empty libc.so,
#                                        libm.so and libdl.so with bionic's
#                                        symbol lists, and its start-up objects
#             lib/clang/19               the compiler's headers and runtime (LLVM)
#             licences/                  the notices of all of the above
#   Environment: NDK, CMAKE, NINJA, PYTHON as build_llvm.sh.
set -euo pipefail
WORK=${1:?usage: $0 <work directory>}
WORK=$(cd "$WORK" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/toolchain/versions.sh"
: "${NDK:?NDK must name an Android NDK}"
CMAKE=${CMAKE:-cmake}
NINJA=${NINJA:-ninja}
PYTHON=${PYTHON:-python3}
BIN=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)
TRIPLE=aarch64-linux-android
TARGET=--target=$TRIPLE$ANDROID_API
SRC=$WORK/llvm-project-$LLVM_VERSION.src
CLANG_MAJOR=${LLVM_VERSION%%.*}

DATA=$WORK/data
rm -rf "$DATA"
SYSROOT=$DATA/sysroot
INCLUDE=$SYSROOT/usr/include
LIB=$SYSROOT/usr/lib/$TRIPLE
RESOURCE=$DATA/lib/clang/$CLANG_MAJOR
mkdir -p "$INCLUDE" "$LIB/$ANDROID_API" "$RESOURCE/lib/linux/aarch64" "$DATA/licences"

# ---- Android's C library: headers ------------------------------------------
BIONIC=$WORK/bionic-$BIONIC_COMMIT
for part in libc libm libdl; do
    if [ ! -d "$BIONIC/$part" ]; then
        mkdir -p "$BIONIC/$part"
        curl -fsSL --retry 3 -o "$BIONIC/$part.tar.gz" \
            "https://android.googlesource.com/platform/bionic/+archive/$BIONIC_COMMIT/$part.tar.gz"
        tar -xzf "$BIONIC/$part.tar.gz" -C "$BIONIC/$part" --warning=no-timestamp
    fi
done
# As bionic's own build lays its headers out for the NDK (libc/Android.bp,
# ndk_headers): the C library's, the kernel's interface as bionic keeps it,
# and arm64's part of that under the target's name.
cp -R "$BIONIC/libc/include/." "$INCLUDE/"
for part in asm-generic drm linux misc mtd rdma scsi sound video xen; do
    mkdir -p "$INCLUDE/$part"
    cp -R "$BIONIC/libc/kernel/uapi/$part/." "$INCLUDE/$part/"
done
cp -R "$BIONIC/libc/kernel/android/uapi/linux/." "$INCLUDE/linux/"
cp -R "$BIONIC/libc/kernel/android/scsi/scsi/." "$INCLUDE/scsi/"
mkdir -p "$INCLUDE/$TRIPLE"
cp -R "$BIONIC/libc/kernel/uapi/asm-arm64/." "$INCLUDE/$TRIPLE/"
find "$INCLUDE" -type f ! -name '*.h' -delete

# ---- Android's C library: what a link needs of it ---------------------------
# libc, libm and libdl are on the phone; a link wants their names and versions
# (tools/toolchain/ndk_stubs.py).
STUBS=$WORK/stubs
mkdir -p "$STUBS"
for lib in c m dl; do
    "$PYTHON" "$ROOT/tools/toolchain/ndk_stubs.py" "$BIONIC/lib$lib/lib$lib.map.txt" "$ANDROID_API" \
        "$STUBS/lib$lib.c" "$STUBS/lib$lib.map"
    "$BIN/clang" "$TARGET" -shared -nostdlib -fPIC -w -fno-builtin \
        "-Wl,--version-script=$STUBS/lib$lib.map" "-Wl,-soname,lib$lib.so" \
        -o "$LIB/$ANDROID_API/lib$lib.so" "$STUBS/lib$lib.c"
done
# The objects every shared library begins and ends with: they register its
# destructors and say which Android it was built for. Against the headers
# just laid out, with the branch protection bionic builds arm64 with.
CRT=("$BIN/clang" "$TARGET" -nostdinc -isystem "$BIN/../lib/clang/$CLANG_MAJOR/include"
    "-I$INCLUDE" "-I$INCLUDE/$TRIPLE" "-I$BIONIC/libc" "-I$BIONIC/libc/private"
    -O2 -fPIC -mbranch-protection=standard -Wall -Wno-gcc-compat -c)
COMMON=$BIONIC/libc/arch-common/bionic
"${CRT[@]}" "$COMMON/crtbegin_so.c" -o "$STUBS/crtbegin_so.o"
"${CRT[@]}" "-DPLATFORM_SDK_VERSION=$ANDROID_API" "$COMMON/crtbrand.S" -o "$STUBS/crtbrand.o"
"$BIN/ld.lld" -r -o "$LIB/$ANDROID_API/crtbegin_so.o" "$STUBS/crtbegin_so.o" "$STUBS/crtbrand.o"
"${CRT[@]}" "$COMMON/crtend_so.S" -o "$LIB/$ANDROID_API/crtend_so.o"

# ---- LLVM's runtime libraries -----------------------------------------------
# libc++ with the NDK's names and namespace (std::__ndk1), so code compiled
# here is the code the NDK compiles; its ABI library and the unwinder static
# and apart, as the NDK has them.
ANDROID=("-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a
    "-DANDROID_PLATFORM=android-$ANDROID_API" -DANDROID_STL=none
    -G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA" -DCMAKE_BUILD_TYPE=Release)
"$CMAKE" -S "$SRC/runtimes" -B "$WORK/runtimes" "${ANDROID[@]}" \
    -C "$SRC/libcxx/cmake/caches/AndroidNDK.cmake" \
    "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind" \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBUNWIND_HIDE_SYMBOLS=ON \
    -DLIBCXX_HARDENING_MODE=none -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
    "-DCMAKE_INSTALL_PREFIX=$WORK/runtimes-install" >"$WORK/runtimes-configure.log"
"$CMAKE" --build "$WORK/runtimes" --target install-cxx install-cxxabi install-unwind >"$WORK/runtimes-build.log"
mkdir -p "$INCLUDE/c++"
cp -R "$WORK/runtimes-install/include/c++/v1" "$INCLUDE/c++/v1"
cp "$WORK/runtimes-install/lib/libc++_static.a" "$WORK/runtimes-install/lib/libc++abi.a" "$LIB/"
cp "$WORK/runtimes-install/lib/libunwind.a" "$RESOURCE/lib/linux/aarch64/"
# What -lc++ finds, as the NDK has it: a link with -static-libstdc++ takes both.
echo 'INPUT(-lc++_static -lc++abi)' >"$LIB/$ANDROID_API/libc++.a"

# The compiler's own runtime: what a division or an atomic of sixteen bytes
# calls. libatomic is its atomic.c alone, which the link names (-latomic).
"$CMAKE" -S "$SRC/compiler-rt/lib/builtins" -B "$WORK/builtins" "${ANDROID[@]}" \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILTINS_HIDE_SYMBOLS=ON \
    -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=ON \
    "-DCMAKE_ASM_COMPILER_TARGET=$TRIPLE$ANDROID_API" >"$WORK/builtins-configure.log"
"$CMAKE" --build "$WORK/builtins" >"$WORK/builtins-build.log"
cp "$(find "$WORK/builtins" -name 'libclang_rt.builtins-aarch64-android.a')" "$RESOURCE/lib/linux/"
"$BIN/clang" "$TARGET" -O2 -fPIC -fvisibility=hidden -c "$SRC/compiler-rt/lib/builtins/atomic.c" -o "$STUBS/atomic.o"
"$BIN/llvm-ar" rcs "$RESOURCE/lib/linux/aarch64/libatomic.a" "$STUBS/atomic.o"

# The compiler's headers (stddef.h, arm_neon.h), from the build of the compiler.
cp -R "$WORK/llvm-android/lib/clang/$CLANG_MAJOR/include" "$RESOURCE/include"

# ---- the notices --------------------------------------------------------------
cp "$SRC/LICENSE.TXT" "$DATA/licences/LLVM-LICENSE.TXT"
cp "$BIONIC/libc/NOTICE" "$DATA/licences/bionic-libc-NOTICE"
cp "$BIONIC/libm/NOTICE" "$DATA/licences/bionic-libm-NOTICE"
cp "$BIONIC/libdl/NOTICE" "$DATA/licences/bionic-libdl-NOTICE"
du -sh "$DATA"

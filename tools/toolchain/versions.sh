# What the on-device toolchain is made from (docs/ondevice-build.md); sourced
# by the scripts beside it.

# The toolchain app's own version: raised with any change to what it holds.
# 2: the recompiler's patch keeps XER's summary overflow in the context.
# 3: the recompiler packs D3D 2_10_10_10 as it unpacks it (vpkd3d128).
TOOLCHAIN_VERSION=3
# The NDK the PC's game library is built with (setup/setup.ps1) has clang
# 19.0.1; this is the release of the same line.
LLVM_VERSION=19.1.7
LLVM_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
# The app's minSdk, and the level the PC build names (android-26).
ANDROID_API=26
# Android's C library: its headers, the start-up objects and the lists of
# what libc, libm and libdl export, at the head of the branch the NDK above
# was cut from (ndk-r28-release).
BIONIC_COMMIT=94f46d493251dc776d0da6b914824a21aac1bde4

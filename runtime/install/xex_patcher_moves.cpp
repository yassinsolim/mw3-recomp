// XenonRecomp's patcher (XenonUtils/xex_patcher.cpp), compiled with its
// copies as moves.
//
// A delta patch moves parts of the image over themselves, and the patcher
// does it with memcpy, which need not copy overlapping bytes in order:
// Windows' and a Snapdragon 865's do, a Snapdragon 8 Gen 2's does not, and
// Advanced Warfare's title update came out as another executable on the AYN
// Thor (27,793 bytes of it differed). The patcher is the recompiler's source
// and stays as it is: a change to this repository's patch to it would have
// every game's code built again (cmake/game_abi.cmake).
//
// Everything the patcher includes is included first, so the word is replaced
// in its own lines alone. A build without the recompiler's source has no
// patcher (CMakeLists.txt) and this file is empty in it.
#ifdef MW2_HAVE_XEX_PATCHER
#include <xex_patcher.h>
#include <xex.h>
#include <memory_mapped_file.h>

#include <bit>
#include <cassert>
#include <climits>
#include <cstring>
#include <fstream>

#include <aes.hpp>
#include <lzx.h>
#include <mspack.h>
#include <TinySHA1.hpp>

#define memcpy memmove
#include <xex_patcher.cpp>
#endif

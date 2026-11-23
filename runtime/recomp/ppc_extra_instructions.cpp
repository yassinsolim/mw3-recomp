// Two AltiVec instructions Call of Duty 4's executables use and the pinned
// recompiler has no case for: it knows only their VMX128 forms, which is all
// Modern Warfare 2 and 3 have. The recompiler's configuration replaces each
// one by a call here and steps over it (config/COD4.toml, [[midasm_hook]]).
// Adding the two cases to patches/xenonrecomp-mw2.patch instead would change
// what every title's game library says it was built against
// (cmake/game_abi.cmake), and the published apps would refuse the game code
// players have already built.
//
// The recompiler declares a hook with its registers as the parameters' names,
// so an instruction whose result goes where an operand came from has a form
// of its own, with that register once.
//
// Compiled into the recompiled code's own target (CMakeLists.txt), so that
// the calls are bound inside the game library where that is a file of its
// own; in a folder of its own for that, since every source at the top of
// runtime/ is the runtime's.
#include <ppc_context.h>

// vandc vD,vA,vB: vA & ~vB, as the recompiler writes vandc128.
void PpcVandc(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b)
{
    simde_mm_store_si128((simde__m128i*)d.u8,
                         simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)b.u8),
                                               simde_mm_load_si128((simde__m128i*)a.u8)));
}

// vandc vD,vD,vB
void PpcVandcInPlace(PPCVRegister& d, PPCVRegister& b) { PpcVandc(d, d, b); }

// vctuxs vD,vD,0: each float to an unsigned word, saturated, with no scale
// (every use in the two executables is in place with a zero shift), as the
// recompiler writes vcfpuxws128.
void PpcVctuxsInPlace(PPCVRegister& d)
{
    simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_vctuxs(simde_mm_load_ps(d.f32)));
}

// Instructions a title's executables use and the pinned recompiler has no
// case for: it knows what Modern Warfare 2 and 3 have. The recompiler's
// configuration replaces each one by a call here and steps over it
// (config/COD4.toml, config/AW.toml and config/BO3.toml, [[midasm_hook]]). Adding the cases to
// patches/xenonrecomp-mw2.patch instead would change what every title's game
// library says it was built against (cmake/game_abi.cmake), and the published
// apps would refuse the game code players have already built.
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

#if defined(COD4_TITLE)
// Call of Duty 4: two AltiVec instructions of which the recompiler has only
// the VMX128 forms.


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

#elif defined(AW_TITLE) || defined(BO3_TITLE)
// Advanced Warfare, and Black Ops III with six more (at the end): twelve instructions, most of them integer AltiVec
// (config/AW.toml lists how many of each; tools/extra_instructions.py wrote
// the hooks). A vector register holds its sixteen bytes in reverse, as the
// recompiler's own code keeps them, so what works element by element is the
// host's instruction of the same width, and what packs two vectors takes them
// in the other order.

// Where guest memory starts (runtime/guest_memory.cpp): a hook is given
// registers and nothing else.
extern "C" uint8_t* mw2_guest_base();

namespace
{
    simde__m128i Load(const PPCVRegister& v) { return simde_mm_load_si128((const simde__m128i*)v.u8); }
    void Store(PPCVRegister& v, simde__m128i value) { simde_mm_store_si128((simde__m128i*)v.u8, value); }
}

// vminsw vD,vA,vB: the smaller of each pair of signed words.
void PpcVminsw(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_min_epi32(Load(a), Load(b))); }
// vminsw vD,vA,vA, which is a move.
void PpcVminsw_abb(PPCVRegister& d, PPCVRegister& a) { Store(d, Load(a)); }

// vminuh and vmaxuh vD,vA,vB: of each pair of unsigned halfwords.
void PpcVminuh(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_min_epu16(Load(a), Load(b))); }
void PpcVmaxuh(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_max_epu16(Load(a), Load(b))); }

// vsububm and vsubuwm vD,vA,vB: vA - vB in bytes and in words, wrapping.
void PpcVsububm(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_sub_epi8(Load(a), Load(b))); }
void PpcVsubuwm(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_sub_epi32(Load(a), Load(b))); }

// vcmpgtsb vD,vA,vB: all ones in each byte where vA's is the greater, signed.
// None of the six sets cr6 (the tool refuses the form that does).
void PpcVcmpgtsb(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_cmpgt_epi8(Load(a), Load(b))); }

// vaddsws vD,vA,vB: signed words added, saturated. The host has no such
// instruction at this width.
void PpcVaddsws(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b)
{
    for (int i = 0; i < 4; i++)
    {
        const int64_t sum = int64_t(a.s32[i]) + int64_t(b.s32[i]);
        d.s32[i] = sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : int32_t(sum);
    }
}

// vpkuhus128 and vpkuwus128 vD,vA,vA: unsigned halfwords to bytes and
// unsigned words to halfwords, saturated, vA's before vB's (here the same
// register). The host's pack takes its input as signed, so each element is
// brought under the limit first.
void PpcVpkuhus_abb(PPCVRegister& d, PPCVRegister& a)
{
    const simde__m128i limited = simde_mm_min_epu16(Load(a), simde_mm_set1_epi16(0xFF));
    Store(d, simde_mm_packus_epi16(limited, limited));
}
void PpcVpkuwus_abb(PPCVRegister& d, PPCVRegister& a)
{
    const simde__m128i limited = simde_mm_min_epu32(Load(a), simde_mm_set1_epi32(0xFFFF));
    Store(d, simde_mm_packus_epi32(limited, limited));
}
// The two with the one register for all three, as title update 17's
// multiplayer has them (8229AA60).
void PpcVpkuhus_aaa(PPCVRegister& d) { PpcVpkuhus_abb(d, d); }
void PpcVpkuwus_aaa(PPCVRegister& d) { PpcVpkuwus_abb(d, d); }

// vupkd3d128 vD,vB,12 and 20: the packed vertex formats of two and of four
// half floats, for which the recompiler writes a trap. The halves are the last
// two (four) halfwords of vB; two give (x, y, 0, 1). As D3D's own conversion on
// this console has it, an exponent of all ones is a number like any other, not
// an infinity.
namespace
{
    float HalfToFloat(uint16_t half)
    {
        uint32_t mantissa = half & 0x3FF, exponent = (half >> 10) & 0x1F;
        if (exponent == 0)
        {
            if (mantissa == 0) return (half & 0x8000) ? -0.0f : 0.0f;
            exponent = 1;
            while (!(mantissa & 0x400)) { mantissa <<= 1; exponent--; }
            mantissa &= 0x3FF;
        }
        const uint32_t bits = (uint32_t(half & 0x8000) << 16) | ((exponent + 112) << 23) | (mantissa << 13);
        float value;
        __builtin_memcpy(&value, &bits, sizeof value);
        return value;
    }
}
void PpcVupkd3dHalf2(PPCVRegister& d, PPCVRegister& b)
{
    const float x = HalfToFloat(b.u16[1]), y = HalfToFloat(b.u16[0]);
    d.f32[3] = x; d.f32[2] = y; d.f32[1] = 0.0f; d.f32[0] = 1.0f;
}
void PpcVupkd3dHalf4(PPCVRegister& d, PPCVRegister& b)
{
    for (int i = 0; i < 4; i++) d.f32[i] = HalfToFloat(b.u16[i]);
}
// The same with one register for both (title update 17's multiplayer has one,
// 82267028): the halves are read before the first float is stored over them.
void PpcVupkd3dHalf4_aa(PPCVRegister& d)
{
    const uint16_t halves[4] = { d.u16[0], d.u16[1], d.u16[2], d.u16[3] };
    for (int i = 0; i < 4; i++) d.f32[i] = HalfToFloat(halves[i]);
}

// lvehx vD,0,rB: the halfword at rB into its place in vD, the rest undefined.
// As the recompiler does for lvewx, the whole aligned vector is loaded.
void PpcLvehx(PPCVRegister& d, PPCRegister& b)
{
    Store(d, simde_mm_shuffle_epi8(simde_mm_load_si128((const simde__m128i*)(mw2_guest_base() + (b.u32 & ~0xFu))),
                                   simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
}

// mulhd rD,rA,rB: the high 64 bits of the signed 128-bit product.
void PpcMulhd(PPCRegister& d, PPCRegister& a, PPCRegister& b)
{
    d.s64 = int64_t((__int128(a.s64) * __int128(b.s64)) >> 64);
}

// bdnzt eq,target: the count goes down by one, and the branch is taken while
// it is not zero and the comparison before it was equal.
bool PpcBdnzt_eq(PPCRegister& ctr, PPCCRRegister& cr)
{
    --ctr.u64;
    return ctr.u32 != 0 && cr.eq;
}

#if defined(BO3_TITLE)
// Black Ops III (config/BO3.toml, config/BO3ZM.toml): the instructions above
// in forms Advanced Warfare does not have, and eight instructions more.

// vaddsws, vsubuwm and vminsw with the result in vA's register, vaddsws in
// vB's, and vaddsws of a register with itself.
void PpcVaddsws_aab(PPCVRegister& d, PPCVRegister& b) { PpcVaddsws(d, d, b); }
void PpcVaddsws_aba(PPCVRegister& d, PPCVRegister& a) { PpcVaddsws(d, a, d); }
void PpcVaddsws_abb(PPCVRegister& d, PPCVRegister& a) { PpcVaddsws(d, a, a); }
void PpcVsubuwm_aab(PPCVRegister& d, PPCVRegister& b) { PpcVsubuwm(d, d, b); }
void PpcVminsw_aab(PPCVRegister& d, PPCVRegister& b) { PpcVminsw(d, d, b); }

// vcmpgtsw vD,vA,vB: all ones in each word where vA's is the greater, signed.
void PpcVcmpgtsw(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b) { Store(d, simde_mm_cmpgt_epi32(Load(a), Load(b))); }
void PpcVcmpgtsw_aab(PPCVRegister& d, PPCVRegister& b) { PpcVcmpgtsw(d, d, b); }
void PpcVcmpgtsw_aba(PPCVRegister& d, PPCVRegister& a) { PpcVcmpgtsw(d, a, d); }

// vcmpgtuw vD,vA,vB: the same, unsigned. The host compares signed words, and
// an unsigned order is the signed one with each top bit turned over.
void PpcVcmpgtuw(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b)
{
    const simde__m128i top = simde_mm_set1_epi32(int32_t(0x80000000u));
    Store(d, simde_mm_cmpgt_epi32(simde_mm_xor_si128(Load(a), top), simde_mm_xor_si128(Load(b), top)));
}
void PpcVcmpgtuw_aba(PPCVRegister& d, PPCVRegister& a) { PpcVcmpgtuw(d, a, d); }

// vcmpgtuw. vD,vA,vB: and cr6 says whether every word was greater (lt) or
// none was (eq), as the recompiler sets it for the compares it has.
void PpcVcmpgtuwRc(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b, PPCCRRegister& cr6)
{
    PpcVcmpgtuw(d, a, b);
    cr6.setFromMask(Load(d), 0xFFFF);
}
void PpcVcmpgtuwRc_aab(PPCVRegister& d, PPCVRegister& b, PPCCRRegister& cr6) { PpcVcmpgtuwRc(d, d, b, cr6); }
void PpcVcmpgtuwRc_aba(PPCVRegister& d, PPCVRegister& a, PPCCRRegister& cr6) { PpcVcmpgtuwRc(d, a, d, cr6); }

// vnor128 vD,vA,vA, which is "not": every use has one register for both
// operands, and five of them the result's too.
void PpcVnor_abb(PPCVRegister& d, PPCVRegister& a) { Store(d, simde_mm_xor_si128(Load(a), simde_mm_set1_epi32(-1))); }
void PpcVnor_aaa(PPCVRegister& d) { PpcVnor_abb(d, d); }

// vrfip128 vD,vB: each float rounded up to a whole number, as the recompiler
// writes vrfim128 with the other direction.
void PpcVrfip(PPCVRegister& d, PPCVRegister& b)
{
    simde_mm_store_ps(d.f32, simde_mm_round_ps(simde_mm_load_ps(b.f32), SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC));
}

// vsl vD,vA,vB: the whole of vA shifted left by the count in the last three
// bits of vB (0 to 7; the definition wants every byte of vB to carry the same
// count and leaves the result open otherwise, so the last byte's is used).
// The register's sixteen bytes in reverse are the vector as one number, low
// half first.
void PpcVsl(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b)
{
    const unsigned count = b.u8[0] & 7;
    const uint64_t low = a.u64[0], high = a.u64[1];
    d.u64[1] = count ? (high << count) | (low >> (64 - count)) : high;
    d.u64[0] = low << count;
}

// vavgsw vD,vA,vB: the mean of each pair of signed words, halves rounded up.
// The host has the unsigned one of bytes and halfwords only.
void PpcVavgsw(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b)
{
    for (int i = 0; i < 4; i++) d.s32[i] = int32_t((int64_t(a.s32[i]) + int64_t(b.s32[i]) + 1) >> 1);
}
void PpcVavgsw_aba(PPCVRegister& d, PPCVRegister& a) { PpcVavgsw(d, a, d); }

// stvebx vS,0,rB: the one byte of vS that lies where rB points within its
// sixteen, to that address. A register keeps the vector's bytes in reverse.
void PpcStvebx(PPCVRegister& s, PPCRegister& b)
{
    mw2_guest_base()[b.u32] = s.u8[15 - (b.u32 & 0xF)];
}
#endif

// rlwimi. and rldicl., the forms that record (title update 17's multiplayer:
// two and six places; the six are the interpreter's test of a boolean, the
// value's word moved down and compared with zero by the move itself). The
// recompiler writes the operation and no comparison, and the branch after it
// took whatever cr0 held: `if Engine.HasSpeechFeature() then` was true. The
// hook follows the instruction and sets cr0 from its result as the
// recompiler's own recording forms do: the low word, signed, against zero.
void PpcRecordCr0(PPCRegister& result, PPCCRRegister& cr, PPCXERRegister& xer)
{
    cr.compare<int32_t>(result.s32, 0, xer);
}

#endif

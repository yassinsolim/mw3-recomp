// Advanced Warfare's stepped-over instructions (runtime/recomp/
// ppc_extra_instructions.cpp, config/AW.toml) against what the instructions
// are defined to do: the AltiVec ones by the PowerPC vector definition, the
// two VMX128 packs and the half-float unpacks of vupkd3d128 as Xenia's
// implementations have them (xenia/cpu/ppc/ppc_emit_altivec.cc: the packs
// take vA's elements first; FLOAT16_2 reads halfwords 6 and 7 of vB and gives
// (x, y, 0, 1); FLOAT16_4 reads halfwords 4 to 7).
//
// The model here is written in the console's terms, with no host vector
// instruction: a vector is sixteen bytes of which byte 0 is the first, its
// elements big-endian. A register of the recompiler keeps those bytes in
// reverse, so each case goes through ToRegister and FromRegister, and a hook
// that had an element order wrong fails here whatever its arithmetic.
//
// Each instruction has cases with results written out by hand from its
// definition, then a sweep of random inputs against the model.
#include <ppc_context.h>

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

void PpcVminsw(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVminsw_abb(PPCVRegister& d, PPCVRegister& a);
void PpcVminuh(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVmaxuh(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVsububm(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVsubuwm(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVcmpgtsb(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVaddsws(PPCVRegister& d, PPCVRegister& a, PPCVRegister& b);
void PpcVpkuhus_abb(PPCVRegister& d, PPCVRegister& a);
void PpcVpkuwus_abb(PPCVRegister& d, PPCVRegister& a);
void PpcVpkuhus_aaa(PPCVRegister& d);
void PpcVpkuwus_aaa(PPCVRegister& d);
void PpcVupkd3dHalf2(PPCVRegister& d, PPCVRegister& b);
void PpcVupkd3dHalf4(PPCVRegister& d, PPCVRegister& b);
void PpcVupkd3dHalf4_aa(PPCVRegister& d);
void PpcLvehx(PPCVRegister& d, PPCRegister& b);
void PpcMulhd(PPCRegister& d, PPCRegister& a, PPCRegister& b);
bool PpcBdnzt_eq(PPCRegister& ctr, PPCCRRegister& cr);
void PpcRecordCr0(PPCRegister& result, PPCCRRegister& cr, PPCXERRegister& xer);

// The guest memory lvehx reads: four vectors, each byte its own address.
alignas(16) static uint8_t memory[64];
extern "C" uint8_t* mw2_guest_base() { return memory; }

namespace
{
    using Bytes = std::array<uint8_t, 16>; // a vector as the console has it

    PPCVRegister ToRegister(const Bytes& guest)
    {
        PPCVRegister r;
        for (int i = 0; i < 16; i++) r.u8[15 - i] = guest[i];
        return r;
    }
    Bytes FromRegister(const PPCVRegister& r)
    {
        Bytes guest;
        for (int i = 0; i < 16; i++) guest[i] = r.u8[15 - i];
        return guest;
    }

    // Big-endian elements of a guest vector.
    uint16_t Half(const Bytes& v, int i) { return uint16_t(v[2 * i] << 8 | v[2 * i + 1]); }
    uint32_t Word(const Bytes& v, int i) { return uint32_t(Half(v, 2 * i)) << 16 | Half(v, 2 * i + 1); }
    void SetHalf(Bytes& v, int i, uint16_t x) { v[2 * i] = uint8_t(x >> 8); v[2 * i + 1] = uint8_t(x); }
    void SetWord(Bytes& v, int i, uint32_t x) { SetHalf(v, 2 * i, uint16_t(x >> 16)); SetHalf(v, 2 * i + 1, uint16_t(x)); }
    Bytes Words(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        Bytes v;
        SetWord(v, 0, a); SetWord(v, 1, b); SetWord(v, 2, c); SetWord(v, 3, d);
        return v;
    }
    Bytes Halves(std::array<uint16_t, 8> h)
    {
        Bytes v;
        for (int i = 0; i < 8; i++) SetHalf(v, i, h[i]);
        return v;
    }
    Bytes Floats(float a, float b, float c, float d)
    {
        uint32_t w[4];
        const float f[4] = {a, b, c, d};
        std::memcpy(w, f, sizeof w);
        return Words(w[0], w[1], w[2], w[3]);
    }

    // The definitions, element by element.
    Bytes ModelVminsw(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 4; i++) SetWord(d, i, int32_t(Word(a, i)) < int32_t(Word(b, i)) ? Word(a, i) : Word(b, i));
        return d;
    }
    Bytes ModelVminuh(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 8; i++) SetHalf(d, i, Half(a, i) < Half(b, i) ? Half(a, i) : Half(b, i));
        return d;
    }
    Bytes ModelVmaxuh(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 8; i++) SetHalf(d, i, Half(a, i) > Half(b, i) ? Half(a, i) : Half(b, i));
        return d;
    }
    Bytes ModelVsububm(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 16; i++) d[i] = uint8_t(a[i] - b[i]);
        return d;
    }
    Bytes ModelVsubuwm(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 4; i++) SetWord(d, i, Word(a, i) - Word(b, i));
        return d;
    }
    Bytes ModelVcmpgtsb(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 16; i++) d[i] = int8_t(a[i]) > int8_t(b[i]) ? 0xFF : 0x00;
        return d;
    }
    Bytes ModelVaddsws(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 4; i++)
        {
            const int64_t sum = int64_t(int32_t(Word(a, i))) + int64_t(int32_t(Word(b, i)));
            SetWord(d, i, uint32_t(sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : int32_t(sum)));
        }
        return d;
    }
    // The packs: vA's elements narrowed into the first half, vB's the second.
    Bytes ModelVpkuhus(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 8; i++)
        {
            d[i] = Half(a, i) > 0xFF ? 0xFF : uint8_t(Half(a, i));
            d[8 + i] = Half(b, i) > 0xFF ? 0xFF : uint8_t(Half(b, i));
        }
        return d;
    }
    Bytes ModelVpkuwus(const Bytes& a, const Bytes& b)
    {
        Bytes d;
        for (int i = 0; i < 4; i++)
        {
            SetHalf(d, i, Word(a, i) > 0xFFFF ? 0xFFFF : uint16_t(Word(a, i)));
            SetHalf(d, 4 + i, Word(b, i) > 0xFFFF ? 0xFFFF : uint16_t(Word(b, i)));
        }
        return d;
    }
    // A half float as D3D's conversion on the console reads it: sign, five
    // bits of exponent biased by 15, ten of mantissa, and no infinities (an
    // exponent of 31 is 2^16 times the mantissa's 1.x).
    float ModelHalf(uint16_t h)
    {
        const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        float magnitude = 0.0f;
        const float two = 2.0f;
        auto scale = [&](float x, int power) { for (; power > 0; power--) x *= two; for (; power < 0; power++) x /= two; return x; };
        if (exponent == 0) magnitude = scale(float(mantissa), -24);              // denormal: m * 2^-10 * 2^-14
        else magnitude = scale(1.0f + float(mantissa) / 1024.0f, exponent - 15);
        return (h & 0x8000) ? -magnitude : magnitude;
    }
    Bytes ModelHalf2(const Bytes& b) { return Floats(ModelHalf(Half(b, 6)), ModelHalf(Half(b, 7)), 0.0f, 1.0f); }
    Bytes ModelHalf4(const Bytes& b)
    {
        return Floats(ModelHalf(Half(b, 4)), ModelHalf(Half(b, 5)), ModelHalf(Half(b, 6)), ModelHalf(Half(b, 7)));
    }

    using Hook3 = void (*)(PPCVRegister&, PPCVRegister&, PPCVRegister&);
    using Hook2 = void (*)(PPCVRegister&, PPCVRegister&);
    Bytes Run(Hook3 hook, const Bytes& a, const Bytes& b)
    {
        PPCVRegister d = ToRegister(Bytes{}), ra = ToRegister(a), rb = ToRegister(b);
        hook(d, ra, rb);
        // An instruction leaves its operands alone.
        assert(FromRegister(ra) == a && FromRegister(rb) == b);
        return FromRegister(d);
    }
    Bytes Run(Hook2 hook, const Bytes& a)
    {
        PPCVRegister d = ToRegister(Bytes{}), ra = ToRegister(a);
        hook(d, ra);
        assert(FromRegister(ra) == a);
        return FromRegister(d);
    }
    // The recompiler passes one register twice where the instruction names it
    // twice (vD = vA or vD = vB): the result must not depend on it.
    void CheckAliased(Hook3 hook, const Bytes& a, const Bytes& b, const Bytes& expected)
    {
        PPCVRegister x = ToRegister(a), y = ToRegister(b);
        hook(x, x, y);
        assert(FromRegister(x) == expected);
        x = ToRegister(a);
        hook(y, x, y);
        assert(FromRegister(y) == expected);
    }

    std::mt19937_64 rng(0x41560914); // the title's id: the sweep is the same every run
    Bytes Random()
    {
        Bytes v;
        // One element in four an edge value, or random bytes never meet a limit.
        static const uint8_t edges[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE};
        for (int i = 0; i < 16; i += 4)
        {
            const uint64_t r = rng();
            if ((r & 3) == 0) for (int k = 0; k < 4; k++) v[i + k] = edges[(r >> (8 + 8 * k)) % 6];
            else for (int k = 0; k < 4; k++) v[i + k] = uint8_t(r >> (8 + 8 * k));
        }
        return v;
    }

    void Sweep(Hook3 hook, Bytes (*model)(const Bytes&, const Bytes&))
    {
        for (int n = 0; n < 20000; n++)
        {
            const Bytes a = Random(), b = Random(), expected = model(a, b);
            assert(Run(hook, a, b) == expected);
            CheckAliased(hook, a, b, expected);
        }
    }
}

int main()
{
    // vminsw: signed, so 0x80000000 is the least and 0xFFFFFFFF is -1.
    assert(Run(PpcVminsw, Words(1, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF), Words(2, 0x7FFFFFFF, 0, 0x80000001))
           == Words(1, 0x80000000, 0xFFFFFFFF, 0x80000001));
    Sweep(PpcVminsw, ModelVminsw);
    // vminsw with one register for both operands is that register.
    for (int n = 0; n < 1000; n++) { const Bytes a = Random(); assert(Run(PpcVminsw_abb, a) == a); }

    // vminuh, vmaxuh: unsigned, so 0x8000 is above 0x7FFF.
    {
        const Bytes a = Halves({0x0000, 0xFFFF, 0x8000, 0x7FFF, 0x1234, 0x00FF, 0xFF00, 0x0001});
        const Bytes b = Halves({0x0001, 0x0000, 0x7FFF, 0x8000, 0x1233, 0x0100, 0x00FF, 0x0001});
        assert((Run(PpcVminuh, a, b) == Halves({0x0000, 0x0000, 0x7FFF, 0x7FFF, 0x1233, 0x00FF, 0x00FF, 0x0001})));
        assert((Run(PpcVmaxuh, a, b) == Halves({0x0001, 0xFFFF, 0x8000, 0x8000, 0x1234, 0x0100, 0xFF00, 0x0001})));
    }
    Sweep(PpcVminuh, ModelVminuh);
    Sweep(PpcVmaxuh, ModelVmaxuh);

    // vsububm, vsubuwm: vA - vB, wrapping, with no borrow between elements.
    assert((Run(PpcVsububm, Bytes{0, 1, 0x80, 0xFF, 5, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0},
                           Bytes{1, 0, 0x01, 0xFF, 7, 0, 0, 1, 0x01, 0, 0, 0, 0, 0, 0, 0xFF})
           == Bytes{0xFF, 1, 0x7F, 0, 0xFE, 0, 0, 0xFF, 0x0F, 0, 0, 0, 0, 0, 0, 0x01}));
    assert(Run(PpcVsubuwm, Words(0, 0x00010000, 0x80000000, 10), Words(1, 0x00000001, 0x80000001, 3))
           == Words(0xFFFFFFFF, 0x0000FFFF, 0xFFFFFFFF, 7));
    Sweep(PpcVsububm, ModelVsububm);
    Sweep(PpcVsubuwm, ModelVsubuwm);

    // vcmpgtsb: signed, strictly greater.
    assert((Run(PpcVcmpgtsb, Bytes{1, 0, 0x7F, 0x80, 0xFF, 0x00, 5, 5, 0x80, 0x81, 0, 0, 0, 0, 0, 2},
                            Bytes{0, 1, 0x80, 0x7F, 0x00, 0xFF, 5, 4, 0x81, 0x80, 0, 0, 0, 0, 0, 1})
           == Bytes{0xFF, 0, 0xFF, 0, 0, 0xFF, 0, 0xFF, 0, 0xFF, 0, 0, 0, 0, 0, 0xFF}));
    Sweep(PpcVcmpgtsb, ModelVcmpgtsb);

    // vaddsws: signed, held at the limits.
    assert(Run(PpcVaddsws, Words(0x7FFFFFFF, 0x80000000, 0x7FFFFFFF, 100), Words(1, 0xFFFFFFFF, 0x80000000, 0xFFFFFF38))
           == Words(0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF9C));
    Sweep(PpcVaddsws, ModelVaddsws);

    // vpkuhus128, vpkuwus128 with vA = vB (the only form the executables
    // have): the narrowed elements twice over, unsigned, held at the limit.
    {
        const Bytes a = Halves({0x0000, 0x00FF, 0x0100, 0xFFFF, 0x8000, 0x007F, 0x0080, 0x0001});
        assert((Run(PpcVpkuhus_abb, a) == Bytes{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x80, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x80, 0x01}));
        const Bytes w = Words(0x00000000, 0x0000FFFF, 0x00010000, 0x80001234);
        assert((Run(PpcVpkuwus_abb, w) == Halves({0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF})));
        // The two in a row as the title has them (825BEBA8): four words to
        // four bytes, each held at 255.
        PPCVRegister v39 = ToRegister(Words(0, 255, 256, 0x7FFFFFFF)), v38, v37;
        PpcVpkuwus_abb(v38, v39);
        PpcVpkuhus_abb(v37, v38);
        const Bytes packed = FromRegister(v37);
        for (int i = 0; i < 16; i += 4) assert(packed[i] == 0 && packed[i + 1] == 255 && packed[i + 2] == 255 && packed[i + 3] == 255);
    }
    for (int n = 0; n < 20000; n++)
    {
        const Bytes a = Random();
        assert(Run(PpcVpkuhus_abb, a) == ModelVpkuhus(a, a));
        assert(Run(PpcVpkuwus_abb, a) == ModelVpkuwus(a, a));
        // With the result's register the operands' too.
        PPCVRegister halves = ToRegister(a), words = ToRegister(a);
        PpcVpkuhus_aaa(halves);
        PpcVpkuwus_aaa(words);
        assert(FromRegister(halves) == ModelVpkuhus(a, a) && FromRegister(words) == ModelVpkuwus(a, a));
    }

    // vupkd3d128, half floats. 0x3C00 is 1, 0xC000 is -2, 0x3800 is 0.5,
    // 0x0001 the smallest denormal (2^-24), 0x7BFF the largest number below
    // the exponent of 31 (65504), 0x7C00 that exponent's first (65536 here,
    // an infinity elsewhere), 0x8000 minus zero.
    {
        // Two: halfwords 6 and 7 are x and y; the other six are not read.
        const Bytes two = Halves({0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x3C00, 0xC000});
        assert(Run(PpcVupkd3dHalf2, two) == Floats(1.0f, -2.0f, 0.0f, 1.0f));
        const Bytes four = Halves({0x1111, 0x2222, 0x3333, 0x4444, 0x3800, 0x0001, 0x7BFF, 0x7C00});
        assert(Run(PpcVupkd3dHalf4, four) == Floats(0.5f, 5.9604644775390625e-8f, 65504.0f, 65536.0f));
        const Bytes zeros = Halves({0, 0, 0, 0, 0x8000, 0x0000, 0x03FF, 0x0400});
        assert(Run(PpcVupkd3dHalf4, zeros) == Words(0x80000000, 0x00000000, 0x387FC000, 0x38800000));
    }
    // Every half there is, in each of the places one is read from.
    for (uint32_t h = 0; h < 0x10000; h++)
    {
        const uint16_t other = uint16_t(h * 40503u + 1);
        const Bytes four = Halves({0xDEAD, 0xBEEF, 0xDEAD, 0xBEEF, uint16_t(h), other, uint16_t(~h), uint16_t(other ^ 0x8000)});
        assert(Run(PpcVupkd3dHalf4, four) == ModelHalf4(four));
        assert(Run(PpcVupkd3dHalf2, four) == ModelHalf2(four));
        // In place, as title update 17's multiplayer has it.
        PPCVRegister inPlace = ToRegister(four);
        PpcVupkd3dHalf4_aa(inPlace);
        assert(FromRegister(inPlace) == ModelHalf4(four));
    }

    // lvehx: the halfword at the address (its low bit dropped) arrives where
    // it lies in its aligned sixteen bytes; the rest of the register is
    // undefined and not looked at.
    for (int i = 0; i < 64; i++) memory[i] = uint8_t(0xA0 + i * 3);
    for (uint32_t address = 16; address < 48; address++)
    {
        PPCVRegister d = ToRegister(Bytes{});
        PPCRegister b;
        b.u64 = 0xFFFFFFFF00000000ull | address; // the upper half of a register is not part of an address
        PpcLvehx(d, b);
        const Bytes got = FromRegister(d);
        const uint32_t at = address & ~1u, place = at & 0xF;
        assert(got[place] == memory[at] && got[place + 1] == memory[at + 1]);
    }

    // mulhd: the upper half of the signed product.
    {
        auto mulhd = [](int64_t a, int64_t b) { PPCRegister d, ra, rb; d.u64 = 0; ra.s64 = a; rb.s64 = b; PpcMulhd(d, ra, rb); return d.s64; };
        assert(mulhd(0, 0) == 0);
        assert(mulhd(1, -1) == -1);                                          // -1 over 128 bits
        assert(mulhd(-1, -1) == 0);
        assert(mulhd(INT64_MAX, INT64_MAX) == 0x3FFFFFFFFFFFFFFFll);         // (2^63-1)^2 >> 64
        assert(mulhd(INT64_MIN, INT64_MIN) == 0x4000000000000000ll);         // 2^126 >> 64
        assert(mulhd(INT64_MIN, 1) == -1);
        assert(mulhd(0x100000000ll, 0x100000000ll) == 1);                    // 2^64 >> 64
        assert(mulhd(0x123456789ABCDEFll, 1000000007ll) == 0x43D11Cll);
        // The title's one use (82451AF8) is a division by a constant, done as
        // a multiplication by its reciprocal. The same with ten: 2^64/10
        // rounded up, times n, is n/10 in the upper half.
        assert(mulhd(0x199999999999999All, 1234567890123ll) == 123456789012ll);
        // In place, as `mulhd r3,r3,r4` would pass it.
        PPCRegister x, y; x.s64 = -5; y.s64 = 0x4000000000000000ll;
        PpcMulhd(x, x, y);
        assert(x.s64 == -2);                                                 // -5 * 2^62 >> 64 = floor(-1.25)
    }

    // bdnzt eq: the count goes down whatever happens; taken while it is not
    // zero and the comparison was equal.
    {
        auto step = [](uint64_t count, bool equal, uint64_t& after)
        {
            PPCRegister ctr; ctr.u64 = count;
            PPCCRRegister cr{}; cr.eq = equal; cr.lt = !equal; cr.gt = 1; cr.so = 1; // only eq is read
            const bool taken = PpcBdnzt_eq(ctr, cr);
            after = ctr.u64;
            return taken;
        };
        uint64_t after;
        assert(step(5, true, after) && after == 4);
        assert(!step(5, false, after) && after == 4);
        assert(!step(1, true, after) && after == 0);
        assert(!step(1, false, after) && after == 0);
        assert(step(0, true, after) && after == 0xFFFFFFFFFFFFFFFFull);      // a count of zero goes round
        // The title's loop (826DECB4): compare up to n pairs, leave at the
        // first that differ or when the count runs out.
        const int left[] = {7, 8, 9, 10}, right[] = {7, 8, 0, 10};
        PPCRegister ctr; ctr.u64 = 4;
        PPCCRRegister cr{};
        int compared = 0;
        do { cr.eq = left[compared] == right[compared]; compared++; } while (PpcBdnzt_eq(ctr, cr));
        assert(compared == 3 && !cr.eq && ctr.u64 == 1);
    }

    // The recording forms' cr0: the result's low word, signed, against zero,
    // and the summary overflow carried over.
    {
        auto record = [](uint64_t result, bool overflow)
        {
            PPCRegister r; r.u64 = result;
            PPCXERRegister xer{}; xer.so = overflow;
            PPCCRRegister cr{}; cr.lt = cr.gt = cr.eq = cr.so = 1;
            PpcRecordCr0(r, cr, xer);
            assert(r.u64 == result);
            return cr.lt * 8 + cr.gt * 4 + cr.eq * 2 + cr.so;
        };
        assert(record(0, false) == 2);                      // the interpreter's false
        assert(record(1, false) == 4);
        assert(record(0x80000000u, false) == 8);
        assert(record(0xFFFFFFFF00000000ull, false) == 2);  // only the low word counts
        assert(record(7, true) == 5);
    }

    std::puts("ppc_extra_instructions_test: twelve instructions and two unpacks as defined");
    return 0;
}

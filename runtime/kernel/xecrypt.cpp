// The kernel's hash functions, XeCryptSha*: SHA-1, SHA-256, SHA-384 and
// SHA-512 over a state the title keeps (sha.cpp does the arithmetic). Black
// Ops III imports all of them; the earlier titles brought their own.
//
// A state in the title's memory is the kernel's XECRYPT_SHA*_STATE: the count
// of bytes, the running hash and the unfinished block, the numbers big-endian.
// The title only hands it back to these functions, so what matters is that
// each state fits where the title made room for it: 88 bytes for SHA-1, 100
// for SHA-256, 200 for the two of the 128-byte block.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "sha.h"
#include "../guest.h"

#include <algorithm>
#include <cstring>

using namespace kernel;

namespace
{
    struct GuestSha1   { be32 count; be32 state[5]; uint8_t buffer[64]; };
    struct GuestSha256 { be32 count; be32 state[8]; uint8_t buffer[64]; };
    struct GuestSha512 { be32 count; be32 padding; be64 state[8]; uint8_t buffer[128]; };
    static_assert(sizeof(GuestSha1) == 88 && sizeof(GuestSha256) == 100 && sizeof(GuestSha512) == 200);

    template <typename Host, typename Guest>
    Host Load(const Guest& g)
    {
        Host h;
        h.count = g.count;
        for (size_t i = 0; i < std::size(h.state); i++) h.state[i] = g.state[i];
        std::memcpy(h.buffer, g.buffer, sizeof h.buffer);
        return h;
    }
    template <typename Host, typename Guest>
    void Store(Guest& g, const Host& h)
    {
        g.count = h.count;
        for (size_t i = 0; i < std::size(h.state); i++) g.state[i] = h.state[i];
        std::memcpy(g.buffer, h.buffer, sizeof h.buffer);
    }

    // (state): a fresh state.
    template <typename Host, typename Guest, void (*Start)(Host&)>
    void Init(PPCContext& ctx)
    {
        auto* guest = GuestPtr<Guest>(ctx.r3.u32);
        if (!guest) return;
        Host h{};
        Start(h);
        Store(*guest, h);
    }
    // (state, bytes, size)
    template <typename Host, typename Guest>
    void Update(PPCContext& ctx)
    {
        auto* guest = GuestPtr<Guest>(ctx.r3.u32);
        const uint8_t* bytes = GuestPtr<uint8_t>(ctx.r4.u32);
        if (!guest || (!bytes && ctx.r5.u32)) return;
        Host h = Load<Host>(*guest);
        sha::Update(h, bytes, ctx.r5.u32);
        Store(*guest, h);
    }
    // (state, digest, size): as much of the digest as the title asked for.
    template <typename Host, typename Guest, size_t Size>
    void Final(PPCContext& ctx)
    {
        auto* guest = GuestPtr<Guest>(ctx.r3.u32);
        if (!guest) return;
        Host h = Load<Host>(*guest);
        uint8_t digest[sha::kSha512Size];
        sha::Final(h, digest);
        Store(*guest, h);
        if (auto* out = GuestPtr<uint8_t>(ctx.r4.u32)) std::memcpy(out, digest, std::min<size_t>(ctx.r5.u32, Size));
    }

    void Start1(sha::Sha1& s) { sha::Init(s); }
    void Start256(sha::Sha256& s) { sha::Init(s); }
}

PPC_FUNC(__imp__XeCryptShaInit)   { Init<sha::Sha1, GuestSha1, Start1>(ctx); }
PPC_FUNC(__imp__XeCryptShaUpdate) { Update<sha::Sha1, GuestSha1>(ctx); }
PPC_FUNC(__imp__XeCryptShaFinal)  { Final<sha::Sha1, GuestSha1, sha::kSha1Size>(ctx); }

PPC_FUNC(__imp__XeCryptSha256Init)   { Init<sha::Sha256, GuestSha256, Start256>(ctx); }
PPC_FUNC(__imp__XeCryptSha256Update) { Update<sha::Sha256, GuestSha256>(ctx); }
PPC_FUNC(__imp__XeCryptSha256Final)  { Final<sha::Sha256, GuestSha256, sha::kSha256Size>(ctx); }

PPC_FUNC(__imp__XeCryptSha384Init)   { Init<sha::Sha512, GuestSha512, sha::Init384>(ctx); }
PPC_FUNC(__imp__XeCryptSha384Update) { Update<sha::Sha512, GuestSha512>(ctx); }
PPC_FUNC(__imp__XeCryptSha384Final)  { Final<sha::Sha512, GuestSha512, sha::kSha384Size>(ctx); }

PPC_FUNC(__imp__XeCryptSha512Init)   { Init<sha::Sha512, GuestSha512, sha::Init512>(ctx); }
PPC_FUNC(__imp__XeCryptSha512Update) { Update<sha::Sha512, GuestSha512>(ctx); }
PPC_FUNC(__imp__XeCryptSha512Final)  { Final<sha::Sha512, GuestSha512, sha::kSha512Size>(ctx); }

// XeCryptSha(bytes1, size1, bytes2, size2, bytes3, size3, digest, size): the
// SHA-1 of up to three pieces one after the other, in one call.
PPC_FUNC(__imp__XeCryptSha)
{
    sha::Sha1 h;
    sha::Init(h);
    const uint32_t pieces[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (const auto& piece : pieces)
        if (piece[0] && piece[1]) sha::Update(h, GuestPtr<uint8_t>(piece[0]), piece[1]);
    uint8_t digest[sha::kSha1Size];
    sha::Final(h, digest);
    if (auto* out = GuestPtr<uint8_t>(ctx.r9.u32)) std::memcpy(out, digest, std::min<size_t>(ctx.r10.u32, sha::kSha1Size));
}

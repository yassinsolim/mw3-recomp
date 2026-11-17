// A party host's partystate names the fragment it is, by an index the staging
// area has no room for.
//
// Modern Warfare 3's multiplayer sends the party state in fragments: the
// members ten to a fragment, then the match rules in chunks (the host's count,
// sub_82194740, is 1 or 2 plus the rules' chunks). Every fragment starts with
// the state's id (a long), its own index and the number of fragments, 3 bits
// each (sub_82193C98 writes them, sub_821948F0 sends them).
//
// A member keeps the fragments until the set is whole. sub_82188020(staging,
// msg) reads the three values and stores by the index, with nothing compared:
//
//     staging[0]               = the number of fragments
//     staging[4 + index*4]     = the id
//     staging + 0x1C + index*0x2C   the fragment's msg_t, 0x2C bytes
//     staging + 0x124 + index*0x4F0 the fragment's bytes, up to 0x4F0
//
// The staging area has room for 6 fragments: the messages end at 0x1C + 6*0x2C
// = 0x124, where the bytes begin, and 6 * 0x4F0 of bytes end at party+0x3D60,
// before the play history (party+0x3D90) and the end of the party (0x3DA8
// bytes; sub_82184CA0 clears that much). An index of 6 or 7 puts up to 0x4F0
// bytes of the host's message over the play history, its ring index and what
// lies after the party; a count of 7 has the member parse a seventh msg_t
// (sub_82189240 -> sub_82187B90) out of the first fragment's own bytes, a
// message whose data pointer and size the host wrote.
//
// The handler takes the message only from the party's host (sub_82189430:
// sub_8216FD80 on the sender), so the sender is a host: the one of a party or
// lobby the player joined. A host's fragments are numbered 0..5 of at most 6.
// Any other is refused before the original runs: nothing is stored, and the
// set in progress is forgotten (its count set to 0), so nothing is applied
// until a whole valid set has arrived. The three values are read with the
// title's own routines, and the message's read state is put back after.
//
// MW3 multiplayer only: MW2's partystate is one message.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "log.h"

#include <cstdint>
#include <cstring>

#if defined(T_PartyClient_StoreStateFragment) && defined(T_MSG_ReadLong) && defined(T_MSG_ReadBits) \
    && defined(T_MSG_SIZE)

// (staging = r3, msg = r4)
GUEST_HOOK(T_PartyClient_StoreStateFragment)
{
    constexpr int32_t kFragments = 6;

    const PPCContext saved = ctx;
    const uint32_t staging = saved.r3.u32;
    const uint32_t msg = saved.r4.u32;

    // The hook runs at the function's entry, so the stack below r1 is free
    // for the calls.
    uint8_t state[T_MSG_SIZE];
    std::memcpy(state, base + msg, sizeof state);
    ctx.r3.u64 = msg;
    GUEST_FUNC(T_MSG_ReadLong)(ctx, base);
    ctx.r3.u64 = msg;
    ctx.r4.u64 = 3;
    GUEST_FUNC(T_MSG_ReadBits)(ctx, base);
    const int32_t index = ctx.r3.s32;
    ctx.r3.u64 = msg;
    ctx.r4.u64 = 3;
    GUEST_FUNC(T_MSG_ReadBits)(ctx, base);
    const int32_t count = ctx.r3.s32;
    std::memcpy(base + msg, state, sizeof state);
    ctx = saved;

    // A message read past its end gives -1 for either.
    if (index < 0 || index >= kFragments || count < 0 || count > kFragments) {
        static bool logged;
        if (!logged)
            LOGW("online: dropped a party state fragment numbered %d of %d (a host sends 0..%d of at most %d)",
                 index, count, kFragments - 1, kFragments);
        logged = true;
        PPC_STORE_U32(staging, 0);
        return;
    }
    GUEST_ORIG(T_PartyClient_StoreStateFragment)(ctx, base);
}

#endif

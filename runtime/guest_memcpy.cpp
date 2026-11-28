// The title's memcpy, made by the host's.
//
// Recompiled, the CRT's memcpy is a loop of byte-swapped 8-byte loads and
// stores with the condition registers and the counter kept in the context
// on every lap; in Shadows of Evil it was a twelfth of the main thread's
// processor time on the AYN Thor and the most sampled function of it. Bytes
// copied are bytes copied whatever their order, so the host's memmove makes
// the same copy. Except one: the guest copies forward, 8 bytes at a time,
// and a copy onto its own source a little higher up repeats what it has
// just written, which memmove does not. That one is the guest's to make, and
// every copy is with MW2_GUEST_MEMCPY=1.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"

#include <cstring>

#ifdef T_Memcpy
GUEST_HOOK(T_Memcpy)
{
    static const bool guest = env::Flag("MW2_GUEST_MEMCPY");
    const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
    if (guest || (destination > source && destination - source < size))
    {
        GUEST_ORIG(T_Memcpy)(ctx, base);
        return;
    }
    std::memmove(base + destination, base + source, size);
    // r3, the destination, is what it returns.
}
#endif

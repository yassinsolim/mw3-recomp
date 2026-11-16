// D3D9's command-buffer arena, from the command processor's side.
//
// D3D9's arena is a ring of its own, and the producer will not overwrite a
// byte until the command processor reports it read past -- a pair of
// EVENT_WRITE_SHDs closing each segment: position, then sequence. Missing one
// report stops the title for good, with an empty ring.
#include <ppc_recomp_shared.h>
#include "internal.h"
#include "gpu.h"
#include "../log.h"
#include "../kernel/kernel.h"

#include <atomic>

namespace
{
    using namespace gpu::detail;

    std::atomic<uint32_t> g_block{ 0 };        // physical
    uint32_t g_pendingPosition = 0;            // GPU thread only, as is the rest
    bool     g_havePosition = false;
    uint32_t g_sequence = 0;
}

// A segment is allocated at a fixed size but filled only as far as the frame
// needs, so a stale trailer from its previous use can sit before the real one.
// Applying it would move the position backwards and deadlock the title with an
// empty ring. The sequence only moves forward, so it settles which is which.
bool gpu::detail::ArenaProgress(uint32_t address, uint32_t value)
{
    const uint32_t block = g_block.load(std::memory_order_relaxed);
    if (!block) return false;
    const uint32_t target = address & ~3u;
    if (target == block + 4) { g_pendingPosition = value; g_havePosition = true; return true; }
    if (target != block) return false;

    // The sequence advances by two per segment, and signed arithmetic keeps the
    // comparison right across a wrap. A report claiming thousands ahead would
    // poison the counter for good -- every genuine one after it would read as
    // stale -- so it is dropped too.
    constexpr int32_t kMaxAdvance = 1024;
    const int32_t advance = int32_t(value - g_sequence);
    if (g_sequence && (advance <= 0 || advance > kMaxAdvance))
    {
#if defined(MW2_ANDROID) && !defined(MW3_TITLE)
        // The MW2 phone build only: a jump backwards of more than a lap of
        // the ring (~64-128 sequence counts), or a restart near 0, is taken
        // for the title resetting its counter and applied. Everywhere else a
        // backwards move is a stale trailer and is dropped, as above.
        const bool isReset = (advance < -128) || (value <= 32 && g_sequence > 64);
        if (isReset)
        {
            LOGI("gpu: arena sequence reset detected (%u -> %u, advance=%d)",
                 g_sequence, value, advance);
        }
        else
        {
            LOGI("gpu: arena dropped stale trailer (seq=%u, current=%u, advance=%d, pos=%08X)",
                 value, g_sequence, advance, g_pendingPosition);
            g_havePosition = false;
            return true;
        }
#else
        g_havePosition = false;
        return true;
#endif
    }
    g_sequence = value;
    if (g_havePosition)
    {
        WritePhysical(block + 4, g_pendingPosition);
        g_havePosition = false;
    }
    WritePhysical(block, value);
    return true;
}

void gpu::SetArenaProgressBlock(uint32_t address)
{
    const uint32_t physical = kernel::ToPhysical(address);
    const uint32_t prev = g_block.exchange(physical, std::memory_order_relaxed);
    if (prev != physical)
    {
        LOGI("gpu: arena progress block at %08X (physical %08X)", address, physical);
#if defined(MW2_ANDROID) && !defined(MW3_TITLE)
        // The MW2 phone build starts the sequence over with a new block.
        // Elsewhere the sequence is the title's and outlives the address it
        // is reported through.
        g_sequence = 0;
        g_havePosition = false;
        g_pendingPosition = 0;
#endif
    }
}

uint32_t gpu::GetArenaProgressBlock()
{
    return g_block.load(std::memory_order_relaxed);
}

uint32_t gpu::GetArenaSequence()
{
    return g_sequence;
}

#if defined(MW2_ANDROID) && !defined(MW3_TITLE)
// The MW2 phone build's hang monitor writes the progress D3D is waiting for
// when the ring has gone idle under it. No other build does: it tells the
// title the command processor read commands it never saw.
void gpu::ForceArenaProgress(uint32_t sequence, uint32_t position)
{
    const uint32_t block = g_block.load(std::memory_order_relaxed);
    if (!block) return;
    g_sequence = sequence;
    g_havePosition = false;
    g_pendingPosition = position;
    WritePhysical(block + 4, position);
    WritePhysical(block, sequence);
    LOGW("gpu: forced arena progress block %08X to seq=%u, pos=%08X",
         block, sequence, position);
}
#endif


// D3D9's command-buffer arena, from the command processor's side.
//
// D3D9's arena is a ring of its own, and the producer will not overwrite a
// byte until the command processor reports it read past -- a pair of
// EVENT_WRITE_SHDs closing each segment: position, then sequence. Missing one
// report stops the title for good, with an empty ring.
#include <ppc_recomp_shared.h>
#include "internal.h"
#include "gpu.h"
#include "../guest.h"
#include "../log.h"
#include "../kernel/kernel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{
    using namespace gpu::detail;

    std::atomic<uint32_t> g_block{ 0 };        // physical
    uint32_t g_pendingPosition = 0;            // GPU thread only, as is the rest
    bool     g_havePosition = false;
    uint32_t g_sequence = 0;

    // For D3D's wait for arena space (gpu::AwaitArenaProgress): taken around
    // nothing by the command processor after each report, so a waiter that
    // looked under it either saw the report or is already asleep.
    std::mutex g_reportLock;
    std::condition_variable g_reported;

    // D3D's own test in its wait: the low two bits of the position are the
    // sequence's, and the wait is over when they are the ones asked for, or
    // one behind with the position reached.
    bool Reached(uint32_t current, uint32_t position, uint32_t sequence)
    {
        const uint32_t behind = (sequence - current) & 3;
        return behind == 0 || (behind == 1 && position <= (current & ~3u));
    }
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
    { std::lock_guard held(g_reportLock); }
    g_reported.notify_all();
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

void gpu::AwaitArenaProgress(uint32_t block, uint32_t position, uint32_t sequence)
{
    const be32* current = GuestPtr<be32>(block + 4);
    if (Reached(*current, position, sequence)) return;
    // No longer than this asleep: the title's loop also watches a flag of
    // the device's and counts its own time-out, and takes over from here.
    constexpr auto kLongest = std::chrono::milliseconds(20);
    std::unique_lock held(g_reportLock);
    g_reported.wait_for(held, kLongest, [&] { return Reached(*current, position, sequence); });
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


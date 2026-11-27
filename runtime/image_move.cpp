// Moving a streamed image's pixels.
//
// MW2 keeps its streamed images in a compacting pool. When the pool relocates a
// block, sub_823A5CB8 allocates the new home, asks sub_823A5780 to move the
// bytes and then points the image record at the new address. The move is not a
// memcpy: it is queued and flushed through T_Image_FlushMove, which builds a
// surface header over the two blocks and has the GPU do the copy -- the 360's
// fast path for shifting several megabytes of texture memory.
//
// The copy is one draw: a vertex shader that fetches the source and memory-
// exports it to the destination, with the export stream constant in c0.x --
// 0x40000000 | the destination's physical address in dwords. Our shaders drop
// memory exports, so the copy is made on the CPU instead, but at that draw:
// every draw the title queued before it still sees the old bytes, every one
// after it the new. Made when the pool asks, it ran up to two frames ahead of
// draws still naming the block, and they drew whatever the pool laid on it
// next. The pool says what to copy; the draw says when.
#include <ppc_recomp_shared.h>
#include "image_move.h"
#include "gpu/memory_watch.h"
#include "title.h"
#include "guest.h"
#include "diagnostics.h"
#include "log.h"
#include "watchpoint.h"
#include "gpu/gpu.h"
#include "gpu/vulkan/texture_cache.h"
#include "gpu/vulkan/renderer.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    struct PendingCopy
    {
        uint32_t destination = 0, source = 0, size = 0;   // guest virtual
        uint32_t streamConstant = 0;                       // c0.x of the copy's draws
        uint64_t frame = 0;                                // the guest frame asked in
        // The draws the title makes of it that the parse has yet to meet: one,
        // or one for each piece of a move down onto its own source (Pieces).
        uint32_t drawsLeft = 1;
        bool made = false;                                 // at the first of them
        // One draw of Black Ops III's second copy (Asked): the first kStripe
        // bytes of every two, within `size` from the two addresses on.
        bool striped = false;
    };
    constexpr uint32_t kStripe = 128;
    std::mutex g_pendingLock;
    std::vector<PendingCopy> g_pending;
    std::atomic<uint32_t> g_pendingCount{ 0 };
    // The copy draw's vertex shader, once seen: the plain copy's, and the
    // striped one's.
    uint64_t g_copyShader[2]{};

    // What the pool has moved lately, for the flash hunt's log lines
    // (diagnostics only).
    struct Move
    {
        uint32_t destination = 0, source = 0, size = 0;   // guest virtual
        uint64_t frame = 0, sequence = 0;
    };
    constexpr size_t kMoves = 1024;
    Move g_moves[kMoves];
    uint64_t g_moveCount = 0;
    std::mutex g_moveLock;

    std::atomic<uint64_t> g_asked{ 0 }, g_bytes{ 0 };
    uint64_t g_atDraw = 0, g_atFrameEnd = 0, g_otherShader = 0;   // GPU thread only
    uint64_t g_laterPieces = 0, g_piecesBeforeAnother = 0;        // GPU thread only
    std::atomic<uint64_t> g_immediate{ 0 };

    // `inStream` when the copy is made where the command stream makes it, on
    // the thread that parses the stream: the renderer then orders it against
    // the draws before it.
    void Copy(const PendingCopy& copy, bool inStream)
    {
        if (inStream) vk::renderer::BeforeStreamWrite(kernel::ToPhysical(copy.destination), copy.size);
        watchpoint::Suspend();
        gpu::watch::WillWrite(copy.destination, copy.size);
        uint8_t* to = guest::Base() + copy.destination;
        const uint8_t* from = guest::Base() + copy.source;
        // The pool never asks for a move that overlaps; memmove says so for free.
        if (!copy.striped) std::memmove(to, from, copy.size);
        else
            for (uint32_t at = 0; at < copy.size; at += 2 * kStripe)
                std::memmove(to + at, from + at, kStripe);
        watchpoint::Resume();
        if constexpr (diag::kOn)
        {
            std::lock_guard held(g_moveLock);
            g_moveCount++;
            g_moves[g_moveCount % kMoves] = { copy.destination, copy.source, copy.size,
                                              gpu::FrameCount(), g_moveCount };
        }
        vk::textures::MemoryWritten(kernel::ToPhysical(copy.destination), copy.size);
    }

    // How many draws the title makes of one move. Advanced Warfare's function
    // (the campaign's sub_825A91F0, the multiplayer's sub_822AEAA8) draws 64
    // bytes a vertex, all of them at once unless the destination lies below
    // the source and reaches into it: then no draw is longer than the distance
    // between the two, and each has the move's stream constant. The earlier
    // titles' functions make the one draw whatever the two blocks are.
    uint32_t Pieces([[maybe_unused]] uint32_t destination, [[maybe_unused]] uint32_t source,
                    [[maybe_unused]] uint32_t size)
    {
#ifdef AW_TITLE
        const bool overlap = destination + size > source && source + size > destination;
        if (!overlap || source <= destination) return 1;
        const uint32_t vertices = size >> 6, most = (source - destination) >> 6;
        return most && vertices ? (vertices + most - 1) / most : 1;
#else
        return 1;
#endif
    }

    // The copies one call of the title's function draws, in its order.
    //
    // Black Ops III's (the zombies' sub_824871A8, the multiplayer's
    // sub_824880D0) takes a fourth argument, and with any of its bits 3 to 8
    // set the block is not the bytes from its address on: it is every other
    // 128 bytes of a 4 KB piece (bit 3) or an 8 KB one, then the other 128s
    // of the next piece, and so on -- a chequerboard, the rest of which is
    // another block's, that starts a piece later (the pool's sources come in
    // such pairs, 4 KB apart). The function draws a piece at a time with a
    // second vertex shader (128 bytes fetched and exported of every 256),
    // each draw with a stream constant of its own and every other one 128
    // bytes further on at both ends. Each draw's stream and vertex buffer
    // are declared a piece long, and what its later vertices would write
    // past that is not written: it is the neighbour's.
    //
    // Taken for the plain copy, such a move matched no draw, the second
    // shader being "another shader", and was made at the end of the frame
    // after, all of it: its image was drawn for two frames from whatever lay
    // at the new place, and the other block's 128s at the new place were
    // written over with what lay beside the source (4,517 of 22,309 moves in
    // a hundred seconds of Shadows of Evil; its far street as flat
    // rectangles while the first zombies' images come in).
    template <typename Each>
    void Asked(uint32_t destination, uint32_t source, uint32_t size,
               [[maybe_unused]] uint32_t flags, Each&& each)
    {
        const uint64_t frame = gpu::FrameCount();
        const auto constant = [](uint32_t address)
        { return 0x40000000u | (kernel::ToPhysical(address) >> 2); };
#ifdef BO3_TITLE
        if (flags & 0x1F8)
        {
            const uint32_t most = flags & 8 ? 4096 : 8192;
            uint32_t shift = kStripe;
            for (uint32_t left = size; left;)
            {
                const uint32_t piece = std::min(left, most);
                each(PendingCopy{ destination, source, piece, constant(destination), frame, 1,
                                  false, true });
                left -= piece;
                destination += piece + shift;
                source += piece + shift;
                shift = 0u - shift;
            }
            return;
        }
#endif
        each(PendingCopy{ destination, source, size, constant(destination), frame,
                          Pieces(destination, source, size) });
    }
}

#ifdef T_Image_FlushMove
GUEST_HOOK(T_Image_FlushMove)
{
    const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
    const uint32_t physical = kernel::ToPhysical(destination);
    // Queued before the title's own call writes the copy draw, so the parse
    // never meets the draw before it knows what the draw copies.
    const bool wanted = destination && source && size;
    const bool atItsDraw = wanted && physical && !(physical & 3) && gpu::Consuming();
    const uint32_t flags = ctx.r6.u32;
    if (atItsDraw)
    {
        std::lock_guard held(g_pendingLock);
        Asked(destination, source, size, flags,
              [](const PendingCopy& copy) { g_pending.push_back(copy); });
        g_pendingCount.store(uint32_t(g_pending.size()), std::memory_order_relaxed);
    }
    GUEST_ORIG(T_Image_FlushMove)(ctx, base);
    if (!wanted) return;
    g_asked.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    if (atItsDraw) return;
    // Nothing is consuming the command stream to order the copy against.
    g_immediate++;
    Asked(destination, source, size, flags, [](const PendingCopy& copy) { Copy(copy, false); });
}

#endif

bool imagepool::CopiesWaiting() { return g_pendingCount.load(std::memory_order_relaxed) != 0; }

void imagepool::AtDraw(uint32_t streamConstant, uint64_t vertexShader)
{
    if ((streamConstant & 0xC0000000u) != 0x40000000u) return;
    PendingCopy copy;
    {
        std::lock_guard held(g_pendingLock);
        auto named = [&](const PendingCopy& c) { return c.streamConstant == streamConstant; };
        auto it = std::find_if(g_pending.begin(), g_pending.end(), named);
        if (it == g_pending.end()) return;
        // c0 outlives the draw that set it. Once the copy shader is known, an
        // old stream constant under some other draw is not a copy.
        uint64_t& shader = g_copyShader[it->striped];
        if (shader && vertexShader != shader) { g_otherShader++; return; }
        shader = vertexShader;
        copy = *it;
        // A move drawn in pieces stays until its last: the pool moves blocks
        // to one destination again and again, and a later piece's draw named
        // the next move there as well as its own, which was then copied
        // before the draws the title had put ahead of it -- from a source the
        // pool had not filled yet, noise for as long as the image stayed.
        if (copy.made)
        {
            g_laterPieces++;
            if (std::find_if(it + 1, g_pending.end(), named) != g_pending.end())
                g_piecesBeforeAnother++;
        }
        it->made = true;
        if (--it->drawsLeft == 0) g_pending.erase(it);
        g_pendingCount.store(uint32_t(g_pending.size()), std::memory_order_relaxed);
    }
    if (copy.made) return;
    Copy(copy, true);
    g_atDraw++;
}

void imagepool::AtFrameEnd(uint64_t parsedFrames)
{
    std::vector<PendingCopy> overdue;
    {
        std::lock_guard held(g_pendingLock);
        auto late = std::stable_partition(g_pending.begin(), g_pending.end(),
                                          [&](const PendingCopy& c) {
                                              return parsedFrames <= c.frame + 1;
                                          });
        overdue.assign(late, g_pending.end());
        g_pending.erase(late, g_pending.end());
        g_pendingCount.store(uint32_t(g_pending.size()), std::memory_order_relaxed);
    }
    // One made at its first draw was only waiting for draws that never came.
    size_t late = 0;
    for (const PendingCopy& c : overdue)
        if (!c.made) { Copy(c, true); late++; }
    g_atFrameEnd += late;
}

void imagepool::DescribeMovesAround(uint32_t physicalAddress, uint32_t size, uint64_t frames,
                                    char* out, size_t bytes)
{
    if (!out || !bytes) return;
    out[0] = 0;
    const uint32_t address = kernel::FromPhysical(physicalAddress);
    if (!address || !size) return;
    const uint64_t now = gpu::FrameCount();
    const uint64_t since = now > frames ? now - frames : 0;
    std::vector<Move> near;
    {
        std::lock_guard held(g_moveLock);
        for (const Move& m : g_moves)
        {
            if (!m.size || m.frame < since) continue;
            const bool into = m.destination + m.size > address && m.destination < address + size;
            const bool outOf = m.source + m.size > address && m.source < address + size;
            if (into || outOf) near.push_back(m);
        }
    }
    std::sort(near.begin(), near.end(),
              [](const Move& a, const Move& b) { return a.sequence < b.sequence; });
    size_t at = 0;
    for (const Move& m : near)
    {
        if (at + 80 >= bytes) break;
        const bool into = m.destination + m.size > address && m.destination < address + size;
        const uint32_t other = into ? m.source : m.destination;
        at += size_t(std::snprintf(out + at, bytes - at, "%s#%llu f%llu %s %08X..%08X",
                                   at ? ", " : "", (unsigned long long)m.sequence,
                                   (unsigned long long)m.frame, into ? "IN from" : "OUT to",
                                   other, other + m.size));
    }
    if (near.empty())
        std::snprintf(out, bytes, "no copy touched it in %llu frames", (unsigned long long)frames);
}

void imagepool::Report()
{
    if constexpr (!diag::kOn) return;
    const uint64_t asked = g_asked.load();
    if (!asked) return;
    LOGI("image pool: %llu blocks moved, %llu MB; %llu copied at their own draw, %llu at the"
         " end of their frame because no draw named them, %llu with nothing consuming the"
         " stream; %llu times an old stream constant sat under another shader",
         (unsigned long long)asked, (unsigned long long)(g_bytes.load() >> 20),
         (unsigned long long)g_atDraw, (unsigned long long)g_atFrameEnd,
         (unsigned long long)g_immediate.load(), (unsigned long long)g_otherShader);
    if (g_laterPieces)
        LOGI("image pool: %llu later draws of moves made in pieces, %llu of them with another move"
             " to the same destination waiting behind",
             (unsigned long long)g_laterPieces, (unsigned long long)g_piecesBeforeAnother);
}

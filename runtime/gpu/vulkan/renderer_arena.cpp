// What a draw reads besides its textures -- constants, vertex data, indices,
// its entries in the draw table -- copied out of guest memory and the register
// file into the frame arena.
#include "renderer_state.h"
#include "../memory_watch.h"
#include "../../guest.h"
#include "../../kernel/physical.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    // A draw's constants, vertex data and indices are copied here and addressed by
    // offset, and it is reset when the frame is submitted. The share's end
    // belongs to the vertex windows the slot keeps (KeptWindow). UINT32_MAX
    // when the share has no room: the draw is made in the next slot's
    // (vk::renderer::Draw).
    uint32_t Allocate(uint32_t bytes, ArenaKind kind)
    {
        const uint32_t aligned = (g.arenaUsed + g.uniformAlignment - 1) & ~(g.uniformAlignment - 1);
        if (aligned + bytes > g.arenaSlotBytes - g.slots[g.slot].keptBytes) return UINT32_MAX;
        g.arenaUsed = aligned + bytes;
        g.arenaWritten[kind] += bytes;
        // Relative to the slot: set 2 is bound at the slot's base (renderer.cpp).
        return aligned;
    }

    // The next `bytes` of a stream (State::Stream): straight after what it
    // handed out last, for as long as its chunk lasts.
    uint32_t AllocateFrom(State::Stream& stream, uint32_t bytes, uint32_t chunk, ArenaKind kind)
    {
        if (stream.end - stream.at < bytes)
        {
            // A whole chunk while the share has room for one; near its end,
            // only what is asked for.
            uint32_t size = std::max(bytes, chunk);
            if (uint64_t(g.arenaUsed) + g.uniformAlignment + size > g.arenaSlotBytes - g.slots[g.slot].keptBytes)
                size = bytes;
            const uint32_t at = Allocate(size, kind);
            if (at == UINT32_MAX) return at;
            // Counted as it is handed out below, not as it is set aside.
            g.arenaWritten[kind] -= size;
            stream = { at, at + size };
        }
        const uint32_t at = stream.at;
        stream.at += bytes;
        g.arenaWritten[kind] += bytes;
        return at;
    }

    uint32_t CopyIntoArena(const void* bytes, uint32_t size, ArenaKind kind)
    {
        const uint32_t at = Allocate(size, kind);
        if (at == UINT32_MAX) return at;
        std::memcpy(ArenaAt(at), bytes, size);
        return at;
    }

    // Vertex data into the arena in host byte order. The console stores a
    // dword of it one of four ways, which its fetch constant names: as it
    // is, the bytes of each half exchanged (8-in-16), all four reversed
    // (8-in-32), the halves exchanged (16-in-32). Every Xenos vertex format
    // is whole dwords, so the exchange is the same whatever the vertices
    // are, and made here it is made once a copy; in the vertex shader it
    // was ten operations for every dword of every vertex, each time the
    // vertex was shaded (shader::VertexOrder). `endian` 0 copies the bytes
    // as they are.
#if !defined(_WIN32)
    __attribute__((target_clones("avx2", "default")))
#endif
    void CopyVertices(uint8_t* to, const uint8_t* from, uint32_t bytes, uint32_t endian)
    {
        typedef uint32_t Unaligned __attribute__((aligned(1), may_alias));
        const Unaligned* in = reinterpret_cast<const Unaligned*>(from);
        Unaligned* out = reinterpret_cast<Unaligned*>(to);
        const uint32_t count = bytes / 4;
        switch (endian & 3)
        {
        case 1:
            for (uint32_t i = 0; i < count; i++)
                out[i] = ((in[i] & 0x00FF00FFu) << 8) | ((in[i] >> 8) & 0x00FF00FFu);
            break;
        case 2:
            for (uint32_t i = 0; i < count; i++) out[i] = __builtin_bswap32(in[i]);
            break;
        case 3:
            for (uint32_t i = 0; i < count; i++) out[i] = (in[i] >> 16) | (in[i] << 16);
            break;
        default:
            std::memcpy(to, from, bytes);
            return;
        }
        std::memcpy(to + count * 4, from + count * 4, bytes & 3);
    }

    // Without a shadow of guest memory -- a storage buffer on Turnip reaches
    // 128 MB, the shadow is 512 -- every draw copied its vertex windows into
    // the arena: 12 to 31 MB a frame of Modern Warfare 2, half of the ring
    // consumer's time on a Snapdragon 865, and in Team Player more than a
    // slot's share holds. Most of it is world geometry the title never
    // writes again. So the slot keeps what it copies, at its share's end,
    // and its later frames draw from the copy for as long as the watch says
    // no page of the window was written (gpu/memory_watch.h).
    //
    // What is kept is not the window but the block of guest memory it lies
    // in: a power of two of 64 KB chunks, at a multiple of its own size, so
    // two blocks are apart or one inside the other and never partly over
    // each other. The windows of other draws into the same buffer fall
    // inside it, and a window that reaches past it makes the block that
    // holds both, which takes the place of the ones inside it. Kept window
    // by window, Team Player's filled the share every few frames; kept as
    // the window rounded out to chunks, by where it starts, the ranges lay
    // over each other and a longer one from the same start threw the
    // shorter one's place away: 76 MB of a share for 41 MB of vertex data
    // on the AYN Thor, and all of it let go thirteen times in one flight.
    // A block is at most four times its window rounded up, so a small
    // window across a large boundary does not bring megabytes with it, and
    // at most kLargestBlock; a window no block is allowed for, or whose
    // block has a page the title keeps writing, is kept alone, as it is.
    //
    // A range is copied the second time the slot meets it unwritten, not
    // the first: the title's dynamic geometry goes round a ring, each
    // stretch of it drawn once and written over a lap later, and kept at
    // first sight it took a megabyte of the share a frame. A window in
    // pages the title keeps writing is not kept either: the watch paces
    // those, and the draw copies as before. MW2_KEEP_VERTICES=0 copies
    // every window.
    constexpr uint32_t kKeptChunk = 64 * 1024;
    constexpr uint32_t kLargestBlock = 16u << 20;
    constexpr uint32_t kFrameOwn = 24u << 20;   // of a share, never given to kept copies
    constexpr uint64_t kStaleFrames = 300;      // a range not drawn from for so long goes first
    constexpr uint32_t kRefreshPages = 64;      // more written than that, and the range is copied whole

    bool KeepingVertices()
    {
        static const bool on = !env::Text("MW2_KEEP_VERTICES") || env::Flag("MW2_KEEP_VERTICES");
        return on && !g.shadowBase && gpu::watch::Enabled();
    }

    // The block the window from `physical` to `end` is kept as; false when
    // there is none it may have.
    bool BlockFor(uint32_t physical, uint32_t end, uint32_t& from, uint32_t& to)
    {
        uint64_t size = kKeptChunk;
        while (size < end - physical) size <<= 1;
        const uint64_t most = std::min<uint64_t>(size * 4, kLargestBlock);
        while (size <= most && (physical & ~(size - 1)) + size < end) size <<= 1;
        if (size > most) return false;
        from = uint32_t(physical & ~(size - 1));
        to = uint32_t(from + size);
        return true;
    }

    uint32_t KeptSize(uint32_t from, uint32_t to)
    {
        return (to - from + g.uniformAlignment - 1) & ~(g.uniformAlignment - 1);
    }

    // A kept range at its first use in a frame: the pages of it written
    // since its copy was made, or last brought up to date, are copied again
    // where they are -- a soldier's skinned vertices, which the title makes
    // again when he moves. In place, because the slot's earlier frames are
    // done before it begins another and no draw of this one has named the
    // range yet; at a later use in the frame one may have, and a draw whose
    // window was written since copies its own. Page by page, because a
    // range is megabytes where a buffer is, and a few pages of it are what
    // the title wrote.
    //
    // The pages are listed before they are watched again, a run of them at
    // a time: one written after that is not watched, and the next frame
    // lists it. A run the watch will not take yet -- the title writes it
    // too often -- stays as it is, unwatched, and Unwritten goes on
    // refusing the windows over it whatever the range's stamp becomes.
    void Refresh(uint32_t from, State::Slot::Kept& kept)
    {
        const uint32_t length = kept.end - from;
        if (gpu::watch::Unwritten(from, length, kept.stamp)) return;
        const uint8_t* source = guest::Base() + guest::kPhysicalBase + from;
        uint32_t pages[kRefreshPages];
        const uint32_t count = gpu::watch::WrittenSince(from, length, kept.stamp, pages, kRefreshPages);
        if (count == UINT32_MAX)
        {
            const uint64_t stamp = gpu::watch::TrackSteady(from, length, CompletedSerial, g.frames);
            if (!stamp) return;
            CopyVertices(ArenaAt(kept.at), source, length, kept.endian);
            g.arenaWritten[kArenaVertices] += length;
            kept.stamp = stamp;
            return;
        }
        const uint64_t firstPage = from / gpu::watch::kPageBytes;
        uint64_t newest = kept.stamp;
        for (uint32_t i = 0, run; i < count; i += run)
        {
            for (run = 1; i + run < count && pages[i + run] == pages[i] + run;) run++;
            // The range's first and last pages may be its in part.
            const uint64_t page = (firstPage + pages[i]) * gpu::watch::kPageBytes;
            const uint32_t start = uint32_t(std::max<uint64_t>(page, from));
            const uint32_t stop = uint32_t(std::min<uint64_t>(page + uint64_t(run) * gpu::watch::kPageBytes, kept.end));
            const uint64_t stamp = gpu::watch::TrackSteady(start, stop - start, CompletedSerial, g.frames);
            if (!stamp) continue;
            CopyVertices(ArenaAt(kept.at + (start - from)), source + (start - from), stop - start, kept.endian);
            g.arenaWritten[kArenaVertices] += stop - start;
            newest = std::max(newest, stamp);
        }
        // Every page listed is copied as it was watched or is not watched
        // at all, and the others were watched before the old stamp.
        kept.stamp = newest;
    }

    // A place in the slot's share that no range has any more: joined to the
    // spare places it touches, and given back to the frame's own when it is
    // the lowest of the kept part.
    void LeaveSpare(State::Slot& slot, uint32_t at, uint32_t size)
    {
        auto next = slot.keptSpare.lower_bound(at);
        if (next != slot.keptSpare.end() && at + size == next->first)
        {
            size += next->second;
            next = slot.keptSpare.erase(next);
        }
        if (next != slot.keptSpare.begin())
            if (const auto before = std::prev(next); before->first + before->second == at)
            {
                at = before->first;
                size += before->second;
                slot.keptSpare.erase(before);
            }
        if (at == g.arenaSlotBytes - slot.keptBytes) slot.keptBytes -= size;
        else slot.keptSpare[at] = size;
    }

    // The end of the smallest spare place that holds `size`; UINT32_MAX when
    // none does.
    uint32_t TakeSpare(State::Slot& slot, uint32_t size)
    {
        auto best = slot.keptSpare.end();
        for (auto spare = slot.keptSpare.begin(); spare != slot.keptSpare.end(); ++spare)
            if (spare->second >= size && (best == slot.keptSpare.end() || spare->second < best->second))
                best = spare;
        if (best == slot.keptSpare.end()) return UINT32_MAX;
        best->second -= size;
        const uint32_t at = best->first + best->second;
        if (!best->second) slot.keptSpare.erase(best);
        return at;
    }

    // Where a range that starts at `from` is remembered once met.
    State::Slot::Met& MetAt(State::Slot& slot, uint32_t from)
    {
        return slot.met[(from * 0x9E3779B9u) >> 20];
    }
    static_assert(State::Slot::kMetRanges == 1u << 12, "the place is the hash's top twelve bits");

    // Where the slot's copy of the `bytes` at `physical` is in its share, made
    // now if it has none and has met the range before; UINT32_MAX when the
    // window is not one to keep, not yet, written since its copy was last
    // brought up to date, or there is no room. A copy is in host byte order
    // as `endian` made it (CopyVertices), and serves the fetch constants
    // that name that endianness: a range holds the vertex buffers of many
    // draws, and a draw whose constant names another copies its own window.
    uint32_t KeptWindow(uint32_t physical, uint32_t bytes, uint32_t endian)
    {
        State::Slot& slot = g.slots[g.slot];
        const uint32_t end = physical + bytes;
        // The ranges that start at or below the window, nearest first: the
        // blocks lie apart, but a window kept alone lies over whatever is
        // around it.
        std::vector<uint32_t>& starts = slot.keptStarts;
        size_t range = size_t(std::upper_bound(starts.begin(), starts.end(), physical) - starts.begin());
        for (uint32_t tries = 0; tries < 6 && range; tries++)
        {
            --range;
            State::Slot::Kept& kept = slot.kept[range];
            if (kept.end < end) continue;
            if (kept.frame != g.frames)
            {
                Refresh(starts[range], kept);
                kept.frame = g.frames;
            }
            if (kept.endian != endian) return UINT32_MAX;
            if (!gpu::watch::Unwritten(physical, bytes, kept.stamp)) return UINT32_MAX;
            g.arenaReused[kArenaVertices] += bytes;
            return kept.at + (physical - starts[range]);
        }
        if (slot.keptLet != State::Slot::Let::None) return UINT32_MAX;

        // The window's block, or the window alone.
        uint32_t from = 0, to = 0;
        uint64_t stamp = 0;
        for (uint32_t alone = 0; alone < 2 && !stamp; alone++)
        {
            if (alone) { from = physical; to = end; }
            else if (!BlockFor(physical, end, from, to)) continue;
            // Never narrower than what the slot has from there: a window
            // inside a range and one that runs past its end would take the
            // entry from each other in turn, each copying again.
            State::Slot::Met& met = MetAt(slot, from);
            const bool isMet = met.end && met.from == from;
            if (isMet) to = std::max(to, met.end);
            const auto had = std::lower_bound(starts.begin(), starts.end(), from);
            if (had != starts.end() && *had == from) to = std::max(to, slot.kept[size_t(had - starts.begin())].end);
            // Met in an earlier frame, and not written since: watched then,
            // so the copy made now is of what the stamp stands for. Met in
            // this one, by another pass over the same geometry: not yet.
            if (isMet && met.end == to && gpu::watch::Unwritten(from, to - from, met.stamp))
            {
                if (met.frame == g.frames) return UINT32_MAX;
                stamp = met.stamp;
                break;
            }
            // The first meeting: watched from here on, and the draw copies.
            const uint64_t seen = gpu::watch::TrackSteady(from, to - from, CompletedSerial, g.frames);
            if (!seen) continue;
            met = { from, to, seen, g.frames };
            return UINT32_MAX;
        }
        if (!stamp) return UINT32_MAX;

        // A place of its own: in what earlier ranges left, or the next down
        // from the share's end.
        const uint32_t size = KeptSize(from, to);
        uint32_t at = TakeSpare(slot, size);
        if (at == UINT32_MAX)
        {
            // The frame's own part of the share is a third, and never less
            // than kFrameOwn.
            const uint32_t own = std::max(kFrameOwn, g.arenaSlotBytes / 3);
            if (uint64_t(slot.keptBytes) + size + own > g.arenaSlotBytes)
            {
                slot.keptLet = std::max(slot.keptLet, State::Slot::Let::Stale);
                return UINT32_MAX;
            }
            if (uint64_t(g.arenaUsed) + size > g.arenaSlotBytes - slot.keptBytes) return UINT32_MAX;
            slot.keptBytes += size;
            g.keptPeak = std::max(g.keptPeak, slot.keptBytes);
            at = g.arenaSlotBytes - slot.keptBytes;
        }
        // The ranges inside it have nothing it has not. Their places are
        // free once this frame is done: its draws may have named them.
        const size_t first = size_t(std::lower_bound(starts.begin(), starts.end(), from) - starts.begin());
        size_t stay = first, past = first;
        for (; past < starts.size() && starts[past] < to; past++)
        {
            // A window kept alone that runs on past the range stays.
            if (slot.kept[past].end > to)
            {
                starts[stay] = starts[past];
                slot.kept[stay++] = slot.kept[past];
                continue;
            }
            slot.keptFreed.push_back({ slot.kept[past].at, KeptSize(starts[past], slot.kept[past].end) });
        }
        starts.erase(starts.begin() + stay, starts.begin() + past);
        slot.kept.erase(slot.kept.begin() + stay, slot.kept.begin() + past);
        CopyVertices(ArenaAt(at), guest::Base() + guest::kPhysicalBase + from, to - from, endian);
        g.arenaWritten[kArenaVertices] += to - from;
        starts.insert(starts.begin() + first, from);
        slot.kept.insert(slot.kept.begin() + first, { to, at, stamp, g.frames, endian });
        if (State::Slot::Met& met = MetAt(slot, from); met.from == from) met = {};
        return at + (physical - from);
    }

    void BeginKeptVertices()
    {
        State::Slot& slot = g.slots[g.slot];
        // The slot's last frame is done, and with it the draws that named
        // the ranges others took the place of.
        for (const auto& [at, size] : slot.keptFreed) LeaveSpare(slot, at, size);
        slot.keptFreed.clear();
        if (slot.keptLet == State::Slot::Let::None) return;
        // No room for another: the ranges no frame of the slot has drawn
        // from for a while go, and their places with what else is spare
        // take the ranges that come. All of them go when that is too little
        // to be worth the pieces it leaves, or a draw found no room beside
        // them (vk::renderer::Draw); the slot starts again from none.
        uint32_t left = 0;
        if (slot.keptLet == State::Slot::Let::Stale)
        {
            size_t stay = 0;
            for (size_t range = 0; range < slot.kept.size(); range++)
            {
                const State::Slot::Kept& kept = slot.kept[range];
                if (g.frames - kept.frame <= kStaleFrames)
                {
                    slot.keptStarts[stay] = slot.keptStarts[range];
                    slot.kept[stay++] = kept;
                    continue;
                }
                const uint32_t size = KeptSize(slot.keptStarts[range], kept.end);
                LeaveSpare(slot, kept.at, size);
                left += size;
            }
            slot.keptStarts.resize(stay);
            slot.kept.resize(stay);
        }
        slot.keptLet = State::Slot::Let::None;
        if (left) g.keptStale++;
        if (left >= g.arenaSlotBytes / 16) return;
        if (slot.keptBytes) g.keptEmptied++;
        slot.keptStarts.clear();
        slot.kept.clear();
        std::fill(std::begin(slot.met), std::end(slot.met), State::Slot::Met{});
        slot.keptSpare.clear();
        slot.keptBytes = 0;
    }

    // The file holds 512 vec4 and a pixel program sees 256 from wherever
    // SQ_PS_CONST's base says -- usually 256, so uploading the low half would
    // hand it the vertex program's constants. `needed` is how many of the 256
    // the program reads; what sits above the copy is whatever the arena held,
    // which it never reads.
    uint32_t WritePixelConstants(const gpu::RegisterFile& r, uint32_t needed)
    {
#if defined(MW2_ANDROID) || defined(__ANDROID__)
        // Skinned models, light grid spherical harmonic probes, and mobile
        // Mali GPU vectorised loads require the full 256-vec4 window to be
        // fully populated so lighting and bone constants are never truncated
        // or read as uninitialised.
        needed = 256;
#endif
        const gpu::ConstantWindow window{ r[gpu::SQ_PS_CONST] };
        const uint32_t base = std::min(window.Base(), 256u);
        const uint32_t first = gpu::kConstantBaseAlu + base * 4;

        State::Cached& cache = g.pixelWindow;
        // The cached copy covers `cache.count` of them; it serves this draw if
        // it reaches far enough and none of what it holds has been written
        // since. Stamping only the copied range, rather than all 256, means a
        // write to a constant nobody in this window reads does not throw it out.
        if (cache.valid && cache.base == base && cache.count >= needed &&
            cache.stamp == r.StampOf(first, cache.count * 4))
        {
            g.arenaReused[kArenaConstants] += needed * 16;
            return cache.at;
        }

        const uint32_t bytes = needed * 16;
        const uint32_t at = Allocate(bytes, kArenaConstants);
        if (at == UINT32_MAX) return at;
        std::memcpy(ArenaAt(at), &r.values[first], bytes);
        cache = { r.StampOf(first, needed * 4), base, at, true, needed };
        return at;
    }

    // A vertex program's constants: each block of 16 its window reaches that
    // has been written since its last copy into this slot is copied again, and
    // the table it reads them through, a word for each block of the window,
    // is written when a word of it has changed. A draw changes a block or so
    // -- its world matrix -- of the ten it reads, so most of the window is
    // not copied again.
    uint32_t WriteConstantTable(const gpu::RegisterFile& r, uint32_t needed)
    {
        static_assert(gpu::kConstantBaseAlu % (1u << gpu::kStampShift) == 0 && (1u << gpu::kStampShift) == 64,
                      "a block of 16 constants is one stamp block");
        const uint32_t base = std::min(gpu::ConstantWindow{ r[gpu::SQ_VS_CONST] }.Base(), 256u);
        State::Cached& table = g.constantTable;
        bool moved = !table.valid;
        for (uint32_t k = 0; k < (needed + 15) / 16; k++)
        {
            uint32_t at;
            if (base % 16)
            {
                // The window's blocks are not the file's, which the stamps
                // and the copies kept are of: D3D9 starts a window at a
                // block, so this is copied without looking for one kept.
                at = CopyIntoArena(&r.values[gpu::kConstantBaseAlu + (base + k * 16) * 4], 256, kArenaConstants);
                if (at == UINT32_MAX) return at;
                at /= 16;
            }
            else
            {
                const uint32_t b = base / 16 + k;
                const uint32_t first = gpu::kConstantBaseAlu + b * 64;
                const uint64_t stamp = r.stamps[first >> gpu::kStampShift];
                State::ConstantBlock& block = g.constantBlocks[b];
                if (block.valid && block.stamp == stamp)
                    g.arenaReused[kArenaConstants] += 256;
                else
                {
                    const uint32_t copy = CopyIntoArena(&r.values[first], 256, kArenaConstants);
                    if (copy == UINT32_MAX) return copy;
                    block = { stamp, copy / 16, true };
                }
                at = block.at;
            }
            if (g.constantTableWords[k] == at) continue;
            g.constantTableWords[k] = at;
            moved = true;
        }
        if (!moved) return table.at;
        const uint32_t at = CopyIntoArena(g.constantTableWords, sizeof g.constantTableWords,
                                          kArenaConstants);
        if (at == UINT32_MAX) return at;
        table = { 0, 0, at, true, 0 };
        return at;
    }

    // What a program needs copied: one past the highest constant it names, or
    // the whole window when it indexes them with a run-time value.
    uint32_t ConstantsNeeded(const shader::Translation& program)
    {
        if (program.addressedConstants) return 256;
        return std::min(program.constantsRead, 256u);
    }

    // Xenos takes three corners of a rectangle and works out the fourth as
    // v0 + v2 - v1. The data is still in the guest's byte order here, and a field
    // that is the same at all three corners -- a packed colour, most often -- is
    // carried across rather than run through arithmetic that would not mean
    // anything for it.
    void BuildFourthCorner(uint32_t* vertices, uint32_t stride, uint32_t rect,
                           uint32_t at)
    {
        const uint32_t* v0 = vertices + (rect * 3 + 0) * stride;
        const uint32_t* v1 = vertices + (rect * 3 + 1) * stride;
        const uint32_t* v2 = vertices + (rect * 3 + 2) * stride;
        uint32_t* out = vertices + at * stride;
        for (uint32_t d = 0; d < stride; d++)
        {
            const uint32_t a = __builtin_bswap32(v0[d]);
            const uint32_t b = __builtin_bswap32(v1[d]);
            const uint32_t c = __builtin_bswap32(v2[d]);
            const uint32_t value =
                (a == b && b == c)
                    ? a
                    : std::bit_cast<uint32_t>(std::bit_cast<float>(a) +
                                              std::bit_cast<float>(c) -
                                              std::bit_cast<float>(b));
            out[d] = __builtin_bswap32(value);
        }
    }

    uint64_t VertexRange(uint32_t lowest, uint32_t highest)
    {
        return (uint64_t(lowest) << 32) | highest;
    }

    ConstantOffsets WriteConstants(const gpu::RegisterFile& r,
                                   const shader::Translation& program,
                                   const shader::Translation& pixelProgram,
                                   uint32_t lowestVertex, uint32_t highestVertex,
                                   uint32_t rectangles)
    {
        const uint32_t (&subs)[3] = program.vertexFetchSubs;
        ConstantOffsets out{ 0, 0, 0, 0, 0, false };
        // The pairs of the fetch window either program reads, and the vertex
        // fetches rewritten below in any case.
        uint32_t pairs[3];
        for (uint32_t i = 0; i < 3; i++)
            pairs[i] = program.fetchPairsRead[i] | pixelProgram.fetchPairsRead[i] |
                       program.vertexFetchPairs[i];

        out.vertexFloats = WriteConstantTable(r, ConstantsNeeded(program));
        if (out.vertexFloats == UINT32_MAX) return out;
        out.pixelFloats = WritePixelConstants(r, ConstantsNeeded(pixelProgram));
        if (out.pixelFloats == UINT32_MAX) return out;
        if (g.drawRecord)
            NoteConstantWindows(r, ConstantsNeeded(program), ConstantsNeeded(pixelProgram));

        // The assembled block depends on the fetch registers and on which groups
        // the shader reads, and the vertex copies it points at stay where they are
        // for the rest of the frame -- so an unchanged pair is the same block.
        const uint64_t fetchStamp = r.StampOf(gpu::kConstantBaseFetch, 32 * 6);
        // A rectangle list gets a copy of its own, with the corners it does not
        // carry written into it -- so it can never share one with another draw.
        const bool fetchCached = !rectangles &&
                                 g.fetchBlock.valid && g.fetchBlock.stamp == fetchStamp &&
                                 g.fetchBlock.base == VertexRange(lowestVertex, highestVertex) &&
                                 std::equal(subs, subs + 3, g.fetchSubs) &&
                                 std::equal(pairs, pairs + 3, g.fetchPairs) &&
                                 std::equal(program.vertexFetchTexels, program.vertexFetchTexels + 3, g.fetchTexels);

        // The fetch window is 32 groups of six dwords; the shader sees 96
        // constants of two dwords each, padded to a vec4.
        if (fetchCached)
        {
            if (g.drawRecord) NoteVertexWindow(nullptr, 0);   // the previous draw's
            g.arenaReused[kArenaConstants] += 96 * 16;
            out.fetch = g.fetchBlock.at;
            out.booleans = g.booleans.at;
            out.loops = g.loops.at;
            out.ok = true;
            return out;
        }
        out.fetch = Allocate(96 * 16, kArenaConstants);
        if (out.fetch == UINT32_MAX) return out;

        // A vertex fetch constant holds a physical address. Rather than making all of
        // guest memory addressable by the GPU, the data each fetch names is copied
        // into the frame arena and the address rewritten to point at the copy -- so
        // the shader's arithmetic is untouched and only what a draw reads crosses.
        // What has been copied for this draw, so two of its fetch slots naming one
        // buffer copy it once. It does not outlive the draw: see below.
        constexpr uint32_t kFetchSlots = 32 * 3;
        struct Made { uint32_t address, base, first, dwords, endian; };
        Made made[kFetchSlots];          // every entry is written before it is read
        uint32_t madeCount = 0;
        // Where the vertex shaders load vertex data as it lies, every copy
        // is made in host byte order (shader::VertexOrder::Host).
        const bool hostOrder = vk::pipeline::HostOrderVertices();
        // Points the entry of a slot the vertex program fetches through at its
        // data; false when the arena has no room for the copy.
        const auto pointAtCopy = [&](uint32_t slot, uint32_t* entry) -> bool
        {
            const uint32_t type = entry[0] & 3;
            if (type != 3) return true;                        // not a vertex fetch
            // The exchange a copy of this buffer is made with: none where
            // the shader makes it.
            const uint32_t endian = hostOrder ? entry[1] & 3 : 0;
            const uint32_t declared = (entry[1] >> 2) & 0xFFFFFF;
            if (!declared) { entry[0] = 0; return true; }
            const uint32_t va = kernel::FromPhysical(entry[0] & ~3u);
            if (!va) { entry[0] = 0; return true; }
            if (g.drawRecord) NoteVertexBuffer(entry[0] & ~3u);

            // The window the program reaches, not the extent the constant spans:
            // a model near the end of a shared vertex heap is indexed from the
            // heap's base, so the prefix below it is as large as the buffer.
            const uint32_t stride = program.vertexFetchStride[slot];

            if (rectangles && stride)
            {
                const uint32_t given = rectangles * 3;
                const uint32_t total = given + rectangles;
                const uint32_t at = Allocate(total * stride * 4, kArenaVertices);
                if (at == UINT32_MAX) return false;
                // The fourth corners are worked out from the other three, so
                // the vertices are put together here and not in the arena,
                // which is not memory to read back from.
                static std::vector<uint32_t> corners;
                corners.assign(size_t(total) * stride, 0);
                uint32_t* vertices = corners.data();
                const uint32_t have = std::min(given * stride, declared);
                std::memcpy(vertices, guest::Base() + va, have * 4);
                for (uint32_t rect = 0; rect < rectangles; rect++)
                    BuildFourthCorner(vertices, stride, rect, given + rect);
                CopyVertices(ArenaAt(at), reinterpret_cast<const uint8_t*>(vertices),
                             uint32_t(corners.size() * 4), endian);
                if (g.drawRecord) NoteVertexWindow(guest::Base() + va, have * 4);
                entry[0] = at | type;
                return true;
            }

            const uint64_t from = uint64_t(lowestVertex) * stride;
            const uint64_t to = uint64_t(highestVertex) * stride +
                                program.vertexFetchTail[slot];
            // The copy stops at the end of the buffer the fetch constant
            // declares; a window that starts past it has nothing to copy.
            if (from >= declared) { entry[0] = 0; return true; }
            const uint32_t first = uint32_t(from);
            const uint32_t last = uint32_t(std::min<uint64_t>(to, declared));
            if (last <= first) { entry[0] = 0; return true; }
            const uint32_t words = last - first;
            if (g.drawRecord) NoteVertexWindow(guest::Base() + va + first * 4, words * 4);

            // Pages the title has not written since the shadow copied them
            // are read there, at the guest's own address, and not copied.
            const uint32_t physical = entry[0] & ~3u;
            if (g.shadowBase && va == guest::kPhysicalBase + physical &&
                gpu::watch::Serve(physical + first * 4, words * 4, g.submissionSerial + 1,
                                  CompletedSerial, g.frames))
            {
                g.arenaReused[kArenaVertices] += words * 4;
                entry[0] = (g.shadowBase - g.arenaBase + physical) | type;
                return true;
            }

            // A program that fetches this data four dwords a texel
            // (Translation::vertexFetchTexels) has to find vertex 0 at a
            // multiple of four dwords. Its vertex is a whole number of
            // texels long and the arena's alignment is coarser, so a copy
            // made for it is; one made for another slot of the draw, or a
            // kept window of a buffer that is not at a multiple of sixteen
            // bytes from its range's start, may not be, and then the draw
            // copies its own.
            const bool texels = (program.vertexFetchTexels[slot / 32] >> (slot % 32)) & 1;
            // A window the slot keeps from frame to frame (KeptWindow).
            if (KeepingVertices() && va == guest::kPhysicalBase + physical)
            {
                const uint32_t at = KeptWindow(physical + first * 4, words * 4, endian);
                if (at != UINT32_MAX && !(texels && ((at - first * 4) & 15)))
                {
                    entry[0] = (at - first * 4) | type;
                    return true;
                }
            }

            // One copy serves the other fetch slots of THIS draw that name
            // the same buffer, and no draw after it. Nothing stamps a write
            // to guest memory the way the register file stamps its own, so a
            // copy made earlier in the frame cannot be told apart from one
            // the title has overwritten since -- and it does overwrite, into
            // a ring of dynamic geometry it reuses within the frame. Serving
            // that stale copy draws one mesh through another's indices,
            // which is a triangle stretched across the screen. Measured:
            // holding the copies for a whole frame saved 3 MB of the 12 MB
            // copied per frame here, and cost correctness for it.
            Made* found = nullptr;
            for (uint32_t i = 0; i < madeCount; i++)
                if (made[i].address == physical && made[i].endian == endian) { found = &made[i]; break; }
            const bool covered = found && found->dwords && found->first <= first &&
                                 found->first + found->dwords >= last &&
                                 !(texels && (found->base & 15));
            if (covered)
                g.arenaReused[kArenaVertices] += words * 4;
            else
            {
                // Never the union with what was cached: draws reaching into a
                // shared heap from both ends would ratchet it out to the whole
                // buffer, and every later miss would copy all of it.
                const uint32_t at = Allocate(words * 4, kArenaVertices);
                if (at == UINT32_MAX) return false;
                CopyVertices(ArenaAt(at), guest::Base() + va + first * 4, words * 4, endian);
                if (!found) found = &made[madeCount++];
                *found = { physical, at - first * 4, first, words, endian };
            }
            entry[0] = found->base | type;
            return true;
        };

        // Only the pairs the programs read: a draw reads two or three of the
        // 96, and assembling and copying the whole window was a kilobyte and
        // a half a draw. The rest of the block is whatever the arena held.
        // Each pair is taken from the register file, rewritten and stored
        // once: the arena is write-combined memory, so nothing is read back
        // from it, and a block put together on the stack first was every
        // pair written and read a second time.
        uint32_t written = 0;
        for (uint32_t i = 0; i < 3; i++)
            for (uint32_t bits = pairs[i]; bits; bits &= bits - 1)
            {
                const uint32_t bit = uint32_t(std::countr_zero(bits));
                const uint32_t pair = i * 32 + bit;
                const uint32_t* source = &r.values[gpu::kConstantBaseFetch + pair * 2];
                uint32_t entry[4] = { source[0], source[1], 0, 0 };
                if ((program.vertexFetchPairs[i] >> bit) & 1)
                {
                    if (!pointAtCopy(pair, entry)) return out;
                    // The endianness where the type was, which a vertex
                    // program has no use for: it loads one dword a fetch.
                    // None where the copy is in host byte order already.
                    entry[0] = (entry[0] & ~3u) | (hostOrder ? 0u : source[1] & 3u);
                }
                g.fetchFirst[pair] = entry[0];
                std::memcpy(ArenaAt(out.fetch + pair * 16), entry, sizeof entry);
                written += 16;
            }

        const uint64_t boolStamp = r.StampOf(gpu::kConstantBaseBool, 8);
        if (g.booleans.valid && g.booleans.stamp == boolStamp)
        {
            g.arenaReused[kArenaConstants] += 8 * 4;
            out.booleans = g.booleans.at;
        }
        else
        {
            out.booleans = CopyIntoArena(&r.values[gpu::kConstantBaseBool], 8 * 4, kArenaConstants);
            if (out.booleans == UINT32_MAX) return out;
            g.booleans = { boolStamp, 0, out.booleans, true };
        }

        const uint64_t loopStamp = r.StampOf(gpu::kConstantBaseLoop, 32);
        if (g.loops.valid && g.loops.stamp == loopStamp)
        {
            g.arenaReused[kArenaConstants] += 32 * 4;
            out.loops = g.loops.at;
        }
        else
        {
            out.loops = CopyIntoArena(&r.values[gpu::kConstantBaseLoop], 32 * 4, kArenaConstants);
            if (out.loops == UINT32_MAX) return out;
            g.loops = { loopStamp, 0, out.loops, true };
        }

        // Allocate counted the whole block.
        g.arenaWritten[kArenaConstants] -= 96 * 16 - written;
        g.fetchBlock = { fetchStamp, VertexRange(lowestVertex, highestVertex), out.fetch, true };
        std::copy(subs, subs + 3, g.fetchSubs);
        std::copy(pairs, pairs + 3, g.fetchPairs);
        std::copy(program.vertexFetchTexels, program.vertexFetchTexels + 3, g.fetchTexels);
        out.ok = true;
        return out;
    }

    // A hardware draw names a draw table as its base vertex, and each of its
    // indices names an entry of that table in its high sixteen bits
    // (bindings.h). A draw takes the entries after the last draw's in the
    // open table, so the two can be one hardware draw if nothing else stands
    // between them (vk::renderer::Draw decides); a table ends when it is
    // full, when its chunk of the stream is, and under MW2_NO_MERGE at every
    // draw.
    DrawPlace PlaceInTable(uint32_t span, uint32_t base)
    {
        DrawPlace place;
        place.entries = 1 + (span >> bindings::kTableIndexBits);
        place.base = base;
        if (place.entries > g.mostTableEntries)
        {
            place.refused = "index range too wide for a draw table";
            return place;
        }
        const uint32_t bytes = place.entries * TableWords() * 4;
        const bool goesOn = MergingDraws() && g.drawTableOpen &&
                            g.drawTableEntries + place.entries <= g.mostTableEntries &&
                            g.tableStream.end - g.tableStream.at >= bytes;
        const uint32_t at = AllocateFrom(g.tableStream, bytes, kTableChunk, kArenaTables);
        if (at == UINT32_MAX)
        {
            place.refused = kArenaFull;
            return place;
        }
        if (!goesOn)
        {
            g.drawTable = at / 4;
            g.drawTableEntries = 0;
            g.drawTableOpen = true;
        }
        place.table = g.drawTable;
        place.slot = g.drawTableEntries;
        g.drawTableEntries += place.entries;
        return place;
    }

    uint32_t TableWords()
    {
        return vk::pipeline::VertexTexels() ? bindings::kTableTexelWords : bindings::kTableWords;
    }

    // Entry k is { the draw's record, the guest index its 65,536 start from }
    // and, where an entry is two texels, what the draw's vertex program reads
    // from it in place of loads through the record: the first dword of a
    // fetch constant as WriteConstants last wrote it, or the place of a block
    // of float constants as the constant table has it -- this draw's, both.
    // Put together here and copied across: the arena is not memory to build
    // something in piece by piece.
    void WriteDrawTable(const DrawPlace& place, uint32_t record, const shader::Translation& program)
    {
        static_assert(bindings::kFetchSlots <= bindings::kHeaderBlock, "a header byte names either");
        const uint32_t words = TableWords();
        uint32_t entry[bindings::kTableTexelWords] = { record, place.base };
        if (words == bindings::kTableTexelWords)
            for (uint32_t i = 0; i < program.vertexHeaderWords; i++)
            {
                const uint32_t what = program.vertexHeader[i];
                entry[bindings::kTableWords + i] = what & bindings::kHeaderBlock
                    ? g.constantTableWords[what & ~bindings::kHeaderBlock] : g.fetchFirst[what];
            }
        uint8_t* out = ArenaAt((place.table + place.slot * words) * 4);
        for (uint32_t k = 0; k < place.entries; k++)
        {
            entry[1] = place.base + (k << bindings::kTableIndexBits);
            std::memcpy(out + k * words * 4, entry, words * 4);
        }
    }

    // The index stream holds 32-bit indices whatever the title's are: the
    // entry in the high half, and below it the guest's index less the base
    // its entry holds. `first` is the draw's first entry, shifted into place.
    //
    // The title's 16-bit indices: byte-swapped, with a base of zero, so one
    // entry reaches all of them and one pass does it, finding the lowest and
    // highest vertex named on the way through. A list of nothing but restarts
    // comes out lowest above highest, which WriteIndices turns into vertex 0.
    // Written without a branch -- the restart index counts as zero toward the
    // highest, and as the largest value it can never be the lowest -- so the
    // loop vectorises; cloned for AVX2, picked at run time. Nothing is read
    // back from `out`, which is write-combined memory.
    //
    // No pipeline here has primitive restart on, so the restart index is a
    // vertex like any other to the GPU, as it was before the indices were
    // encoded: 65535, which stays inside the draw's one entry.
#if !defined(_WIN32)
    __attribute__((target_clones("avx2", "default")))
#endif
    void EncodeIndices16(const uint8_t* source, uint32_t* out, uint32_t count, uint32_t first,
                         uint16_t& lowest, uint16_t& highest)
    {
        typedef uint16_t Unaligned __attribute__((aligned(1), may_alias));
        const Unaligned* in = reinterpret_cast<const Unaligned*>(source);
        constexpr uint16_t restart = 0xFFFF;
        uint16_t low = restart, high = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint16_t value = __builtin_bswap16(in[i]);
            out[i] = first + value;
            low = value < low ? value : low;
            const uint16_t counted = value == restart ? uint16_t(0) : value;
            high = counted > high ? counted : high;
        }
        lowest = low;
        highest = high;
    }

    // The title's 32-bit indices take two passes over guest memory: how many
    // entries the draw needs, and what its indices are taken from, is only
    // known once the lowest and the highest are.
#if !defined(_WIN32)
    __attribute__((target_clones("avx2", "default")))
#endif
    void ScanIndices32(const uint8_t* source, uint32_t count, uint32_t& lowest, uint32_t& highest)
    {
        typedef uint32_t Unaligned __attribute__((aligned(1), may_alias));
        const Unaligned* in = reinterpret_cast<const Unaligned*>(source);
        constexpr uint32_t restart = 0xFFFFFFFF;
        uint32_t low = restart, high = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t value = __builtin_bswap32(in[i]);
            low = value < low ? value : low;
            const uint32_t counted = value == restart ? 0u : value;
            high = counted > high ? counted : high;
        }
        lowest = low;
        highest = high;
    }

    // The restart index is outside the range the draw's entries were counted
    // for, and encoded as it stands would name an entry that is not this
    // draw's, or not an entry at all. It becomes the draw's lowest vertex.
#if !defined(_WIN32)
    __attribute__((target_clones("avx2", "default")))
#endif
    void EncodeIndices32(const uint8_t* source, uint32_t* out, uint32_t count, uint32_t first,
                         uint32_t lowest)
    {
        typedef uint32_t Unaligned __attribute__((aligned(1), may_alias));
        const Unaligned* in = reinterpret_cast<const Unaligned*>(source);
        constexpr uint32_t restart = 0xFFFFFFFF;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t value = __builtin_bswap32(in[i]);
            out[i] = value == restart ? first : first + (value - lowest);
        }
    }

    // Indices are big-endian in guest memory and the hardware reads them that
    // way; Vulkan does not. `lowest` and `highest` bound the window of the vertex
    // buffer the draw can reach.
    DrawPlace WriteIndices(uint32_t address, uint32_t count, bool wide,
                           uint32_t& lowest, uint32_t& highest)
    {
        // Copied afresh for every draw. One call is one draw, so a cache here
        // could only ever serve a later draw from an earlier draw's bytes, and
        // an index buffer the title rewrites within the frame -- its dynamic
        // geometry does -- would then be drawn twice from the first version.
        const uint8_t* source = guest::Base() + address;
        if (g.drawRecord) NoteIndices(source, count * (wide ? 4 : 2));
        if (wide)
        {
            ScanIndices32(source, count, lowest, highest);
            if (lowest > highest) lowest = highest;
        }
        DrawPlace place = wide ? PlaceInTable(highest - lowest, lowest) : PlaceInTable(0, 0);
        if (place.refused) return place;
        place.indices = AllocateFrom(g.indexStream, count * 4, kIndexChunk, kArenaIndices);
        if (place.indices == UINT32_MAX)
        {
            place.refused = kArenaFull;
            return place;
        }
        uint32_t* out = reinterpret_cast<uint32_t*>(ArenaAt(place.indices));
        const uint32_t first = place.slot << bindings::kTableIndexBits;
        if (wide) EncodeIndices32(source, out, count, first, lowest);
        else
        {
            uint16_t low, high;
            EncodeIndices16(source, out, count, first, low, high);
            lowest = low;
            highest = high;
            if (lowest > highest) lowest = highest;
        }
        return place;
    }

    // A rectangle list's indices, made here: `a b c a c d` a rectangle, where
    // d is the corner WriteConstants puts after the vertices the title gave.
    DrawPlace WriteRectangleIndices(uint32_t rectangles)
    {
        DrawPlace place = PlaceInTable(rectangles * 4 - 1, 0);
        if (place.refused) return place;
        place.indices = AllocateFrom(g.indexStream, rectangles * 6 * 4, kIndexChunk, kArenaIndices);
        if (place.indices == UINT32_MAX)
        {
            place.refused = kArenaFull;
            return place;
        }
        uint8_t* list = ArenaAt(place.indices);
        const uint32_t first = place.slot << bindings::kTableIndexBits;
        for (uint32_t rect = 0; rect < rectangles; rect++)
        {
            const uint32_t a = first + rect * 3, b = a + 1, c = a + 2;
            const uint32_t d = first + rectangles * 3 + rect;   // the corner made here
            const uint32_t six[6] = { a, b, c, a, c, d };
            std::memcpy(list + rect * 24, six, sizeof six);
        }
        return place;
    }
}

#endif  // MW2_HAVE_VULKAN

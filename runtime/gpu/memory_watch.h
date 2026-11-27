#pragma once
#include <cstdint>

// Which pages of guest physical memory the title has written, found out the way
// Xenia finds out: a page the GPU reads from is made read-only, and the first
// write to it faults once, which marks it written and makes it writable again.
//
// What it is for is the shadow: a copy of physical memory the GPU can read,
// kept page by page. A draw whose vertex data lies in pages nobody has written
// since they were copied reads them there, where it used to copy its window of
// the buffer into the frame arena on every draw -- 28 MB a frame, most of it
// world geometry that never changes.
//
// A page's copy is only replaced once no submission still on the GPU can read
// it: those were recorded against the bytes it holds, which is what the console
// would have drawn from.
namespace gpu::watch
{
    // For a bug report (report.cpp): totals so far. The protection changes
    // are counted and timed only while MW2_REPORT is on.
    struct Costs { uint64_t faults = 0, protects = 0, protectNanoseconds = 0; };
    Costs CostsSoFar();

    // The unit the watch protects, and mprotect works in whole pages of the
    // host's size. 4 KB is every desktop and most phones -- but an Android
    // 15 device has 16 KB pages, and there a 4 KB protection either fails
    // outright, because the address is not page aligned, or succeeds and
    // covers four of these pages while only one of them is recorded as
    // watched. A write to one of the other three then faults into a handler
    // that does not recognise the page, and an ordinary guest write becomes
    // a crash.
    //
    // 16 KB on Android is right either way: exact on a 16 KB kernel, and
    // four whole kernel pages on a 4 KB one, which is legal and still lands
    // on a single watch page. Coarser, and on a phone that is no loss --
    // fewer faults and a quarter of the mprotect calls.
#ifdef MW2_ANDROID
    constexpr uint32_t kPageBytes = 16384;
#else
    constexpr uint32_t kPageBytes = 4096;
#endif

    // Starts the watch. `shadow` is the copy, as mapped for the CPU: physical
    // address N at `shadow + N`. Null watches without one: draws stay on the
    // per-draw copy (Serve refuses), but texture and constant readers still
    // learn which pages are unwritten (Track, Unwritten) instead of hashing
    // the bytes again -- on the Thor that hashing was a fifth of the ring
    // consumer's time. MW2_NO_SHADOW=1 starts it without one.
    void Start(uint8_t* shadow, uint32_t bytes);
    // Every page writable again and the watch off, before the renderer goes.
    void Stop();
    bool Enabled();

    // From the fault handler, before anything else sees the fault, for a write
    // to a page that is there but read-only. True when the page is a watched
    // one, which is writable again and marked.
    bool HandleFault(const void* address);

    // Before this runtime writes a large range of guest memory itself -- the
    // image pool's moves: the range is made writable in one call rather than
    // taking a fault a page. Any thread. `guestAddress` is virtual.
    void WillWrite(uint32_t guestAddress, uint32_t bytes);

    // Nothing may have a system call write into guest memory: the kernel does
    // not fault on a read-only page, it fails the call (EFAULT, ERROR_NOACCESS). File reads
    // and received packets go through a buffer of the runtime's own and are
    // copied in, which faults like any other write.

    // Ring consumer only. Whether [physical, physical + bytes) of the shadow
    // holds what guest memory holds now, copying in the pages that can be. A
    // true answer marks those pages read by `recording`, the submission being
    // recorded; `completed` says the last one the GPU has finished, asked only
    // when a page has to be copied, and `frame` paces the pages that keep being
    // written.
    bool Serve(uint32_t physical, uint32_t bytes, uint64_t recording, uint64_t (*completed)(),
               uint64_t frame);

    // The same watch for a reader that keeps its own copy -- the texture
    // cache. Track, *before* reading the range, returns a stamp when every page
    // of it is watched from then on (0 when not); Unwritten says whether none
    // of them has been written since that stamp. Ring consumer only.
    //
    // Unlike the shadow, Track does not back off from pages the title keeps
    // writing. The shadow's copy of such a page is overwritten unread; a
    // texture is read again whenever a page of it is written, which costs far
    // more than the fault that says so. The lighting table is one: without the
    // watch, all 2 MB of it were hashed at each of the fifteen or so points a
    // frame where it has to be looked at again, for eight pages written.
    uint64_t Track(uint32_t physical, uint32_t bytes, uint64_t (*completed)(), uint64_t frame);
    bool Unwritten(uint32_t physical, uint32_t bytes, uint64_t since);
    // Track for a reader whose copy is not worth making of pages the title
    // keeps writing -- a draw's vertex window kept from frame to frame: paced
    // as the shadow's copies are, and 0 while a page of the range waits.
    uint64_t TrackSteady(uint32_t physical, uint32_t bytes, uint64_t (*completed)(), uint64_t frame);
    // The stamp of the latest copy of any page: what a reader that copies a
    // page it finds watched, without a Track of its own, may say its copy is
    // good from. Ring consumer only.
    uint64_t Stamp();
    // Which pages of the range are not what Unwritten would pass: written
    // since that stamp, or not watched. Their places from the range's first
    // page go into `pages` and their number is returned; UINT32_MAX when
    // there are more than `most` or the range cannot be answered for. Asked
    // after the Track that watches the range again, so a page written while
    // it is asked is either listed now or shows against the new stamp.
    uint32_t WrittenSince(uint32_t physical, uint32_t bytes, uint64_t since, uint32_t* pages,
                          uint32_t most);

    void Report();
}

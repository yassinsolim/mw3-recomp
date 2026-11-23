// Nt*VirtualMemory / Mm* -- guest memory management over the heaps in kernel.cpp.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include "../guest_memory.h"
#include "../guest.h"
#include "../log.h"

namespace
{
    constexpr uint32_t X_MEM_COMMIT  = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE = 0x00002000;
    constexpr uint32_t X_MEM_RELEASE = 0x00008000;
}

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    auto* basePtr = GuestPtr<be32>(ctx.r3.u32);
    auto* sizePtr = GuestPtr<be32>(ctx.r4.u32);
    uint32_t type = ctx.r5.u32;

    if (!basePtr || !sizePtr) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    uint32_t requested = *sizePtr;
    uint32_t wanted    = *basePtr;
    uint32_t size = (requested + 0xFFF) & ~0xFFFu;
    if (!size) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    // A commit onto an existing reservation just succeeds in place.
    if (wanted && (type & X_MEM_COMMIT) && !(type & X_MEM_RESERVE))
    {
        *sizePtr = size;
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }

    uint32_t addr = kernel::AllocateGuest(size, 0x1000);
    if (!addr) { LOGW("NtAllocateVirtualMemory: out of guest heap (%u bytes)", size); ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }

    *basePtr = addr;
    *sizePtr = size;
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    // (BaseAddress*, RegionSize*, FreeType, DebugMemory). A decommit keeps the
    // reservation, which here is the allocation itself.
    auto* basePtr = GuestPtr<be32>(ctx.r3.u32);
    if (!basePtr) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }
    auto* sizePtr = GuestPtr<be32>(ctx.r4.u32);
    if (!(ctx.r5.u32 & X_MEM_RELEASE)) { ctx.r3.u64 = X_STATUS_SUCCESS; return; }
    ctx.r3.u64 = kernel::FreeGuest(*basePtr) ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER;
}

PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    // X_MEMORY_BASIC_INFORMATION; report the whole heap as committed and RW.
    auto* info = GuestPtr<be32>(ctx.r4.u32);
    if (info)
    {
        info[0] = guest::kHeapBase;                       // BaseAddress
        info[1] = guest::kHeapBase;                       // AllocationBase
        info[2] = 0x04;                                   // AllocationProtect = PAGE_READWRITE
        info[3] = guest::kHeapEnd - guest::kHeapBase;      // RegionSize
        info[4] = 0x1000;                                 // State = MEM_COMMIT
        info[5] = 0x04;                                   // Protect
        info[6] = 0x20000;                                // Type = MEM_PRIVATE
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    uint32_t size      = ctx.r4.u32;
    uint32_t minAddr   = ctx.r6.u32;
    uint32_t maxAddr   = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    // The console hands physical memory out in 64 KB pages (16 MB for the
    // largest requests), so a block is never at an odd 4 KB offset whatever
    // alignment the caller asked for -- and the title relies on it. MW2 takes
    // one 444 MB block for nearly all of its memory with alignment 0x1000, and
    // on a 4 KB boundary its own sub-allocators corrupt the script compiler's
    // data during level loads (docs/runtime.md).
    constexpr uint32_t kPageAlignment = 0x10000;
    if (alignment < kPageAlignment) alignment = kPageAlignment;
    uint32_t addr = kernel::AllocatePhysical((size + 0xFFF) & ~0xFFFu, alignment);
    if (!addr)
        LOGW("MmAllocatePhysicalMemoryEx: out of physical heap (%u bytes, align %u, range %08X..%08X; %u MB already handed out)",
             size, alignment, minAddr, maxAddr,
             (kernel::PhysicalHighWater() - guest::kPhysicalBase) / (1024 * 1024));
    ctx.r3.u64 = addr;
}

PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    // (Type, BaseAddress)
    kernel::FreePhysical(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// MmGetPhysicalAddress is in video.cpp, next to the Vd* calls that consume it.
#ifdef COD4_TITLE
// Call of Duty 4's physical memory pool (PMem_Alloc, sub_82188600) asks what
// the page it is about to hand out is protected as, and where the answer is
// not the kind it was asked for (read-write, or write-combined for what the
// GPU reads) it starts a new 64 KB page and protects the block itself. Told
// "read-write" whatever it had set, every write-combined allocation started a
// page of its own, some 270 of them before a level is asked for and most a
// few kilobytes or less, and the 431 MB pool came 3 MB short for Charlie
// Don't Surf started from the menu: "Out of memory" and a restart of the
// title. So the kernel keeps what was set, a page of 4 KB at a time, and
// says it back.
// Only for this title: the other two were measured with the constant answer.
namespace
{
    constexpr uint32_t kReadWrite = 0x04;
    constexpr uint32_t kBankMask  = guest::kPhysicalEnd - guest::kPhysicalBase - 1;
    // 0: never set. Both windows of the bank are the same pages.
    std::atomic<uint32_t> g_protect[(guest::kPhysicalEnd - guest::kPhysicalBase) >> 12];

    bool InBank(uint32_t address) { return address >= guest::kPhysicalBase && address < 0xE0000000u; }
}

PPC_FUNC(__imp__MmQueryAddressProtect)
{
    const uint32_t address = ctx.r3.u32;
    const uint32_t set = InBank(address) ? g_protect[(address & kBankMask) >> 12].load(std::memory_order_relaxed) : 0;
    ctx.r3.u64 = set ? set : kReadWrite;
}

// (BaseAddress, Size, NewProtect)
PPC_FUNC(__imp__MmSetAddressProtect)
{
    const uint32_t address = ctx.r3.u32, size = ctx.r4.u32, protect = ctx.r5.u32;
    if (InBank(address) && size)
    {
        const uint32_t first = (address & kBankMask) >> 12;
        const uint32_t last = std::min<uint32_t>(((address & kBankMask) + size - 1) >> 12, kBankMask >> 12);
        for (uint32_t page = first; page <= last; page++) g_protect[page].store(protect, std::memory_order_relaxed);
    }
    ctx.r3.u64 = 0;
}
#else
PPC_FUNC(__imp__MmQueryAddressProtect) { ctx.r3.u64 = 0x04; }
PPC_FUNC(__imp__MmSetAddressProtect)   { ctx.r3.u64 = 0; }
#endif

PPC_FUNC(__imp__MmQueryStatistics)
{
    auto* s = GuestPtr<be32>(ctx.r3.u32);
    if (s)
    {
        uint32_t total = guest::kHeapEnd - guest::kHeapBase;
        s[1] = total;   // TotalPhysicalPages-ish; the title only sanity-checks these
        s[2] = total / 2;
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

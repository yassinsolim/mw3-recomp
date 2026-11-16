#pragma once
// A fake guest for asset-free tests: the 4 GiB guest space behind
// guest::Base(), small virtual and physical heaps built from the kernel's own
// Heap in its first 16 MiB, and the clock. Include it from exactly one
// translation unit per test executable.
#include "runtime/guest.h"
#include "runtime/kernel/heap.h"
#include "runtime/kernel/kernel.h"

#include <chrono>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace fake
{
    // Guest addresses of the two heaps, inside the 16 MiB below.
    inline constexpr uint32_t kHeapBase = 0x00100000, kHeapEnd = 0x00900000;
    inline constexpr uint32_t kPhysicalBase = 0x00900000, kPhysicalEnd = 0x00F00000;
}

namespace guest
{
    // Address space for all of it, as the runtime reserves, with memory behind
    // what a test touches: the heaps at the bottom and the GPU's register
    // aperture at 7FC80000, which the command processor mirrors writes into.
    uint8_t* Base()
    {
#ifdef _WIN32
        static uint8_t* const memory = [] {
            auto* space = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_READWRITE));
            VirtualAlloc(space, 1u << 24, MEM_COMMIT, PAGE_READWRITE);
            VirtualAlloc(space + 0x7FC80000u, 0x40000, MEM_COMMIT, PAGE_READWRITE);
            return space;
        }();
#else
        static uint8_t* const memory = static_cast<uint8_t*>(
            mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
#endif
        return memory;
    }
}

namespace
{
    kernel::Heap g_fakeHeap{ "virtual", fake::kHeapBase, fake::kHeapEnd };
    kernel::Heap g_fakePhysical{ "physical", fake::kPhysicalBase, fake::kPhysicalEnd };
}

namespace kernel
{
    uint32_t AllocateGuest(uint32_t size, uint32_t alignment) { return g_fakeHeap.Alloc(size, alignment); }
    uint32_t AllocatePhysical(uint32_t size, uint32_t alignment) { return g_fakePhysical.Alloc(size, alignment); }
    bool FreeGuest(uint32_t address) { return g_fakeHeap.Free(address); }
    bool FreePhysical(uint32_t address) { return g_fakePhysical.Free(address); }
    uint32_t PhysicalHighWater() { return g_fakePhysical.HighWater(); }

    uint64_t SystemTime100ns()
    {
        using namespace std::chrono;
        return 1'000'000'000ull +
               uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
    }

    // The watchdog's listing of blocked threads; a test has no watchdog.
    void ReportWaits() {}
}

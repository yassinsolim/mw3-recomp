#include "kernel.h"
#include "xex_module_handle.h"
#include "xex_header.h"
#include "title.h"
#include "heap.h"
#include "../guest.h"
#include "../diagnostics.h"
#include "../log.h"
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>

namespace
{
    std::filesystem::path g_gameRoot;

    kernel::Heap g_heap{ "virtual", guest::kHeapBase, guest::kHeapEnd };
    kernel::Heap g_phys{ "physical", guest::kPhysicalBase, guest::kPhysicalEnd };

    // Whether something live in this arena covers `va`. FromPhysical needs
    // this to tell two arenas apart that answer to the same physical
    // address. The answer is exact, never a guess made because the lock was
    // busy: an answer that depended on that would resolve the same address
    // to different windows from one call to the next.
    //
    // The command processor asks this for every texture and vertex address,
    // millions of times a minute, and the heap's lock and map walk were a
    // seventh of its thread on a phone. So each thread keeps answers -- a
    // live block, or the gap between two -- with the heap's generation they
    // were read at; while the generation has not moved, the blocks have
    // not, and the answer stands without the lock. An answer is kept by
    // the address' megabyte, 64 of them: the title's vertex and index
    // buffers are blocks of their own by the hundred, more than a few
    // answers kept in turn hold (the lock and the walk were still 2% of the
    // ring consumer in Team Player). One thread-local holds them all: on
    // Android each thread-local read is a call.
    bool Covers(kernel::Heap& heap, uint32_t va)
    {
        struct Answer
        {
            const kernel::Heap* heap = nullptr;
            uint64_t generation = 0;
            uint64_t from = 0, to = 0;   // [from, to)
            bool covered = false;
        };
        constexpr uint32_t kKept = 64;
        struct Answers { Answer kept[kKept]; };
        thread_local Answers answers;
        Answer& kept = answers.kept[(va >> 20) % kKept];

        const uint64_t now = heap.generation.load(std::memory_order_acquire);
        if (kept.heap == &heap && kept.generation == now && va >= kept.from && va < kept.to) return kept.covered;

        std::lock_guard held(heap.lock);
        Answer a{ &heap, heap.generation.load(std::memory_order_relaxed), 0, uint64_t(UINT32_MAX) + 1, false };
        auto it = heap.live.upper_bound(va);
        if (it != heap.live.end()) a.to = it->first;
        if (it != heap.live.begin())
        {
            --it;
            const uint64_t blockEnd = uint64_t(it->first) + it->second;
            if (uint64_t(va) < blockEnd) { a.from = it->first; a.to = blockEnd; a.covered = true; }
            else a.from = blockEnd;
        }
        kept = a;
        return a.covered;
    }

    // How often FromPhysical resolved an address to the general heap that the
    // high water mark alone would have sent to the physical arena. Every one of
    // these was a word the command processor and the title disagreed about.
    diag::Stat g_physicalCorrected;

    struct TlsTemplate { uint32_t address = 0, size = 0; };
    TlsTemplate g_tlsTemplate;

    std::mutex g_unimplLock;
    std::map<std::string, uint64_t> g_unimpl;

    uint32_t g_executableXexHeader = 0;

#if defined(MW3_TITLE)
    void InstallExecutableModuleHandle(const std::vector<uint8_t>& xexHeader)
    {
        if (xexHeader.empty()) return; // Flat-PE development runs have no XEX metadata.
        if (xexHeader.size() < 0x18 ||
            std::memcmp(xexHeader.data(), "XEX2", 4) != 0 ||
            uint32_t(*reinterpret_cast<const be32*>(xexHeader.data() + 8)) != xexHeader.size())
        {
            LOGW("XexExecutableModuleHandle: refusing an invalid or incomplete XEX header");
            return;
        }

        unsigned matches = 0;
        const uint32_t importRecord = kernel::xex::FindUniqueDataImportRecord(
            guest::Base() + PPC_IMAGE_BASE, uint32_t(PPC_IMAGE_SIZE),
            uint32_t(PPC_IMAGE_BASE), 0x0193, matches);
        if (!importRecord)
        {
            LOGW("XexExecutableModuleHandle: expected one xboxkrnl ordinal 0x193 data import, found %u",
                 matches);
            return;
        }

        // The audited MW3 callers load this global from the image's own import
        // record (title.h). A unique marker elsewhere is not sufficient: it
        // would leave the actual startup pointer unresolved.
        constexpr uint32_t kModuleHandleGlobal = T_DATA_ModuleHandlePtr;
        if (importRecord != kModuleHandleGlobal)
        {
            LOGW("XexExecutableModuleHandle: ordinal 0x193 is at %08X, MW3 caller reads %08X",
                 importRecord, kModuleHandleGlobal);
            return;
        }

        constexpr uint32_t kLdrEntrySize = 0x64;
        constexpr uint32_t kXexHeaderBaseOffset = 0x58;
        const uint32_t headerAddress = kernel::AllocateGuest(uint32_t(xexHeader.size()), 16);
        const uint32_t ldrAddress = kernel::AllocateGuest(kLdrEntrySize, 16);
        const uint32_t handleSlot = kernel::AllocateGuest(4, 4);
        if (!headerAddress || !ldrAddress || !handleSlot)
        {
            if (headerAddress) kernel::FreeGuest(headerAddress);
            if (ldrAddress) kernel::FreeGuest(ldrAddress);
            if (handleSlot) kernel::FreeGuest(handleSlot);
            LOGE("XexExecutableModuleHandle: could not allocate guest module metadata");
            return;
        }

        std::memcpy(guest::Base() + headerAddress, xexHeader.data(), xexHeader.size());
        std::memset(guest::Base() + ldrAddress, 0, kLdrEntrySize);
        *GuestPtr<be32>(ldrAddress + kXexHeaderBaseOffset) = headerAddress;
        *GuestPtr<be32>(handleSlot) = ldrAddress;
        *GuestPtr<be32>(importRecord) = handleSlot;
        g_executableXexHeader = headerAddress;

        LOGI("XexExecutableModuleHandle: import %08X -> handle slot %08X -> LDR %08X -> XEX header %08X (%zu bytes)",
             importRecord, handleSlot, ldrAddress, headerAddress, xexHeader.size());
    }
#endif

#if defined(AW_TITLE)
    // The kernel's variables, which an executable imports as it does the
    // kernel's functions: its record for one holds, once loaded, the
    // variable's address. Advanced Warfare's D3D9 (XDK 21256) enters
    // VdHSIOCalibrationLock around VdRetrainEDRAM as the device is made, and a
    // record left as the file has it (library and ordinal, 0x000101C1) is the
    // critical section it then spins on. Each variable here gets storage of its
    // own with what a console's would hold; the three the earlier titles read
    // have installers of their own (above, and misc.cpp).
    //
    // The records are read from the XEX header's import table (key 0x103FF:
    // the libraries' names, then for each library the addresses of its
    // records), not looked for in the image by their value: 0x0001000E is not
    // rare enough a word for that. A variable's record has no type in its top
    // byte, as a function's first record has not, but a function has a second
    // one, of type 1, where its thunk is.
    std::map<uint16_t, uint32_t> KernelVariableRecords()
    {
        std::map<uint16_t, uint32_t> variables;
        const uint32_t table = kernel::xex::ImageHeaderField(guest::Base(), g_executableXexHeader, 0x103FF);
        if (!table) return variables;
        const uint32_t namesSize = *GuestPtr<be32>(table + 4);
        const uint32_t libraries = *GuestPtr<be32>(table + 8);
        const char* names = GuestPtr<char>(table + 12);
        uint32_t library = table + 12 + namesSize;
        for (uint32_t i = 0; i < libraries; i++)
        {
            const uint32_t size = *GuestPtr<be32>(library);
            // The name's number counts the table's strings, each padded to four bytes.
            uint32_t wanted = uint16_t(*GuestPtr<be16>(library + 36)) & 0xFF, at = 0;
            while (wanted && at < namesSize)
            {
                at += uint32_t(strnlen(names + at, namesSize - at)) + 1;
                at = (at + 3) & ~3u;
                wanted--;
            }
            if (at < namesSize && std::strncmp(names + at, "xboxkrnl.exe", namesSize - at) == 0)
            {
                std::set<uint16_t> thunked;
                const uint32_t count = uint16_t(*GuestPtr<be16>(library + 38));
                for (uint32_t r = 0; r < count; r++)
                {
                    const uint32_t record = *GuestPtr<be32>(library + 40 + r * 4);
                    const uint32_t value = *GuestPtr<be32>(record);
                    if ((value >> 24) == 1) thunked.insert(uint16_t(value));
                    else if ((value >> 24) == 0) variables[uint16_t(value)] = record;
                }
                for (uint16_t ordinal : thunked) variables.erase(ordinal);
            }
            library += size;
        }
        return variables;
    }

    void InstallKernelVariables()
    {
        struct Variable { uint16_t ordinal; const char* name; uint32_t size; void (*fill)(uint32_t address); };
        static const Variable kVariables[] = {
            // An object type is known by its address alone.
            { 0x00E, "ExEventObjectType", 16, nullptr },
            { 0x01B, "ExThreadObjectType", 16, nullptr },
            // The command line the title was started with: none.
            { 0x1AE, "ExLoadedCommandLine", 512, nullptr },
            // { flags, processors, ... }: a hard disk (0x20), six hardware threads.
            { 0x156, "XboxHardwareInfo", 16, [](uint32_t a) { *GuestPtr<be32>(a) = 0x20; *GuestPtr<uint8_t>(a + 4) = 6; } },
            // { major, minor, build, qfe }: the last system software, 2.0.17559.0.
            { 0x158, "XboxKrnlVersion", 8, [](uint32_t a) { *GuestPtr<be16>(a) = 2; *GuestPtr<be16>(a + 4) = 17559; } },
            // The devices D3D9 and the system's interface register: none yet.
            { 0x1BE, "VdGlobalDevice", 4, nullptr },
            { 0x1BF, "VdGlobalXamDevice", 4, nullptr },
            { 0x1C0, "VdGpuClockInMHz", 4, [](uint32_t a) { *GuestPtr<be32>(a) = 500; } },
            // An RTL_CRITICAL_SECTION nobody holds (sync.cpp): a synchronisation
            // event with an empty wait list, a lock count of -1.
            { 0x1C1, "VdHSIOCalibrationLock", 28, [](uint32_t a) {
                  *GuestPtr<uint8_t>(a) = 1;
                  *GuestPtr<be32>(a + 8) = a + 8;
                  *GuestPtr<be32>(a + 12) = a + 8;
                  *GuestPtr<be32>(a + 16) = uint32_t(-1);
              } },
            // No certification monitor.
            { 0x266, "KeCertMonitorData", 4, nullptr },
        };
        const std::map<uint16_t, uint32_t> records = KernelVariableRecords();
        for (const Variable& variable : kVariables)
        {
            // Not every executable imports every one (the campaign has no ExEventObjectType).
            const auto found = records.find(variable.ordinal);
            if (found == records.end()) continue;
            const uint32_t record = found->second;
            const uint32_t address = kernel::AllocateGuest(variable.size, 16);
            if (!address) { LOGE("%s: no guest memory for the kernel's variable", variable.name); continue; }
            std::memset(guest::Base() + address, 0, variable.size);
            if (variable.fill) variable.fill(address);
            *GuestPtr<be32>(record) = address;
            LOGI("%s: import %08X -> %08X", variable.name, record, address);
        }
        // The three installed before this are pointers by now; what is left
        // with its ordinal in it is a variable this table does not know.
        for (const auto& [ordinal, record] : records)
            if (uint32_t(*GuestPtr<be32>(record)) == (0x00010000u | ordinal))
                LOGW("the kernel's variable of ordinal %03X (import %08X) is not provided", ordinal, record);
    }
#endif
}

namespace
{
    // IMAGE_TLS_DIRECTORY32, big-endian, located through the PE data directory.
    void ReadTlsDirectory()
    {
        const uint8_t* image = guest::Base() + PPC_IMAGE_BASE;
        uint32_t peOffset;
        std::memcpy(&peOffset, image + 0x3C, 4);          // e_lfanew, little-endian
        if (peOffset + 0x100 > PPC_IMAGE_SIZE) return;

        uint16_t optionalMagic;
        std::memcpy(&optionalMagic, image + peOffset + 24, 2);
        const uint32_t directories = peOffset + 24 + (optionalMagic == 0x10B ? 96 : 112);

        uint32_t tlsRva, tlsSize;
        std::memcpy(&tlsRva, image + directories + 9 * 8, 4);
        std::memcpy(&tlsSize, image + directories + 9 * 8 + 4, 4);
        if (!tlsRva || tlsSize < 24) return;

        // Guest-endian: raw data start/end, then index.
        const be32* entry = reinterpret_cast<const be32*>(image + tlsRva);
        uint32_t rawStart = entry[0], rawEnd = entry[1], zeroFill = entry[4];
        if (rawEnd <= rawStart) return;

        g_tlsTemplate.address = rawStart;
        g_tlsTemplate.size = (rawEnd - rawStart) + zeroFill;
    }
}

const TlsTemplate& ImageTlsTemplate() { return g_tlsTemplate; }

void kernel::Initialise(const std::filesystem::path& gameRoot, const std::vector<uint8_t>& xexHeader)
{
    ReadTlsDirectory();
    InstallTimeStampBundle();
    InstallDebugMonitorData();
#if defined(MW3_TITLE)
    InstallExecutableModuleHandle(xexHeader);
#else
    (void)xexHeader;
#endif
#if defined(AW_TITLE)
    InstallKernelVariables();
#endif
    g_gameRoot = gameRoot;
    LOGI("game root: %s%s", gameRoot.string().c_str(),
         std::filesystem::exists(gameRoot) ? "" : "  (does not exist)");
}

const std::filesystem::path& kernel::GameRoot() { return g_gameRoot; }

uint32_t kernel::AllocateGuest(uint32_t size, uint32_t alignment) { return g_heap.Alloc(size, alignment); }
uint32_t kernel::AllocatePhysical(uint32_t size, uint32_t alignment) { return g_phys.Alloc(size, alignment); }
bool     kernel::FreeGuest(uint32_t address) { return g_heap.Free(address); }
bool     kernel::FreePhysical(uint32_t address) { return g_phys.Free(address); }

void kernel::ReportMemory()
{
    if constexpr (!diag::kOn) return;
    g_heap.Report();
    g_phys.Report();
    if (g_physicalCorrected)
        LOGI("memory: %llu physical addresses resolved to the virtual heap that the high"
             " water mark alone would have sent to the physical arena",
             (unsigned long long)g_physicalCorrected.load());
}

namespace
{
    // X_TIME_STAMP_BUNDLE: interrupt time, system time, millisecond tick count.
    // GetTickCount loads the pointer and reads the third field, so an unresolved
    // pointer freezes the title's clock and every timeout built on it.
    struct TimeStampBundle
    {
        be64 interruptTime;
        be64 systemTime;
        be32 tickCount;
        be32 padding;
    };

    uint32_t g_timeStampBundle = 0;
    std::chrono::steady_clock::time_point g_bootTime;

    // The image word that holds the pointer is an unresolved import record rather
    // than an address, which is how it is recognised: the low half is an ordinal
    // and the high half a small type tag, so a real pointer cannot be mistaken
    // for one.
    constexpr uint32_t kTimeStampBundlePointer = T_DATA_TimeStampBundlePtr;

    bool LooksLikeUnresolvedImport(uint32_t value)
    {
        return (value >> 24) == 0 && (value & 0xFFFF) != 0 && value < 0x01000000;
    }
}

void kernel::InstallTimeStampBundle()
{
    uint32_t existing = *GuestPtr<be32>(kTimeStampBundlePointer);
    if (!LooksLikeUnresolvedImport(existing))
    {
        LOGW("time stamp bundle: %08X already holds %08X, not an unresolved import -- leaving it alone",
             kTimeStampBundlePointer, existing);
        return;
    }

    g_timeStampBundle = AllocateGuest(sizeof(TimeStampBundle), 16);
    if (!g_timeStampBundle) { LOGE("could not allocate the time stamp bundle"); return; }
    std::memset(guest::Base() + g_timeStampBundle, 0, sizeof(TimeStampBundle));
    *GuestPtr<be32>(kTimeStampBundlePointer) = g_timeStampBundle;
    g_bootTime = std::chrono::steady_clock::now();
    UpdateTimeStampBundle();
}

// KeDebugMonitorData: a pointer to the debug monitor's dispatch table, null on
// a retail console. XAudio2's mixer reads it on every pass and, when the word
// it points at is not zero, calls a method through it -- so left as an import
// record it points into the low guest space, and whatever the title happens
// to have written there (handles handed back as object pointers live there)
// becomes a call target.
void kernel::InstallDebugMonitorData()
{
    constexpr uint32_t kPointer = T_DATA_DebugMonitorPtr;
    constexpr uint32_t kOrdinal = 0x59;
    uint32_t existing = *GuestPtr<be32>(kPointer);
    if (!LooksLikeUnresolvedImport(existing) || (existing & 0xFFFF) != kOrdinal)
    {
        LOGW("debug monitor data: %08X holds %08X, not the import record -- leaving it alone", kPointer, existing);
        return;
    }
    uint32_t word = AllocateGuest(16, 16);
    if (!word) { LOGE("could not allocate the debug monitor word"); return; }
    *GuestPtr<be32>(word) = 0;
    *GuestPtr<be32>(kPointer) = word;
}

void kernel::UpdateTimeStampBundle()
{
    if (!g_timeStampBundle) return;
    auto* bundle = GuestPtr<TimeStampBundle>(g_timeStampBundle);
    const auto elapsed = std::chrono::steady_clock::now() - g_bootTime;
    const uint64_t hundredNanoseconds =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
    bundle->interruptTime = hundredNanoseconds;
    bundle->systemTime = SystemTime100ns();
    bundle->tickCount = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

uint32_t kernel::PhysicalHighWater() { return g_phys.HighWater(); }

uint32_t kernel::ToPhysical(uint32_t va)
{
    // Exactly the sequence the title inlines around its Vd* calls:
    //   phys = (va & 0x1FFFFFFF) + (((va >> 20) + 0x200) & 0x1000)
    return (va & 0x1FFFFFFFu) + ((((va >> 20) + 0x200u) & 0x1000u));
}

uint32_t kernel::FromPhysical(uint32_t phys)
{
    // Both arenas answer to the same physical address. The title reduces a
    // virtual address to a physical one by masking off the top three bits, so
    // the general heap at 0x84600000 and the physical arena at 0xA4600000 both
    // come out as 0x04600000. On hardware those are two windows onto one bank;
    // here they are separate memory, so the window meant has to be identified
    // and not guessed.
    //
    // Asking each arena whether it has anything live at that offset is exact.
    // What stood here before -- "is it below the physical arena's high water
    // mark" -- is a ceiling that only rises, so as soon as the title had
    // allocated past a heap address' offset (about 70 MB in, which it reaches
    // almost immediately) every heap address resolved to the physical arena
    // instead. The command processor then wrote one window while the title
    // polled the other, and a WAIT_REG_MEM on that word could never be
    // satisfied -- the 2000 ms stalls, and the frame rate pinned to whatever
    // the timeout was set to.
    const uint32_t arena  = guest::kPhysicalBase + phys;
    const uint32_t cached = 0x80000000u + phys;
    const bool mightBeArena  = arena >= guest::kPhysicalBase && arena < guest::kPhysicalEnd;
    const bool mightBeCached = cached >= 0x82000000u && cached < guest::kHeapEnd;

    // The physical arena is asked first, and no shortcut for the image's
    // window stands in front of it: the arena's low addresses reduce to the
    // same physical addresses as the image and the function lookup table
    // below the heap, and an address the GPU was handed out of the arena
    // must never resolve into the XEX image.
    if (mightBeArena && Covers(g_phys, arena)) return arena;

    // The image is not allocated through the heap, so nothing covers it and
    // it falls through to the high water mark below, as it always did.
    if (mightBeCached && cached >= guest::kHeapBase && Covers(g_heap, cached))
    {
        // Worth counting: this is the case that used to come out wrong.
        if (mightBeArena && phys < (g_phys.HighWater() - guest::kPhysicalBase))
            g_physicalCorrected++;
        return cached;
    }

    // Nothing live either side: the answer this gave before. An address can
    // be named before the title allocates it, so it is better to answer than
    // to refuse.
    if (mightBeArena && phys < (g_phys.HighWater() - guest::kPhysicalBase)) return arena;
    if (mightBeCached) return cached;
    return 0;
}

uint32_t kernel::CreateThreadBlock(uint32_t threadId)
{
    // [block .. block+0x8000) is the KPCR, with r13 pointing 0x4000 in so the
    // negative r13-relative offsets the game uses stay inside it. The KTHREAD
    // follows, then this thread's copy of the image's TLS block.
    constexpr uint32_t kPcrSize     = 0x8000;
    constexpr uint32_t kPcrBias     = 0x4000;
    constexpr uint32_t kThreadSize  = 0x1000;
    constexpr uint32_t kTlsSize     = 0x200;
    constexpr uint32_t kTlsPointerOffset    = 0;     // KPCR.TlsData
    constexpr uint32_t kCurrentThreadOffset = 256;   // KPCR.CurrentThread
    constexpr uint32_t kThreadIdOffset      = 332;   // KTHREAD.ThreadId

    const uint32_t total = kPcrSize + kThreadSize + kTlsSize;
    uint32_t block = AllocateGuest(total, 0x1000);
    if (!block) return 0;
    std::memset(guest::Base() + block, 0, total);

    uint32_t pcr     = block + kPcrBias;
    uint32_t kthread = block + kPcrSize;
    uint32_t tls     = block + kPcrSize + kThreadSize;

    // The CRT hangs its per-thread scratch allocator off a slot in the TLS
    // template, reached as [[r13] + slot], so a null TlsData sends every thread to
    // the same imaginary pool at guest address 0 -- where they corrupt each other.
    const auto& tlsTemplate = ImageTlsTemplate();
    if (tlsTemplate.address && tlsTemplate.size && tlsTemplate.size <= kTlsSize)
        std::memcpy(guest::Base() + tls, guest::Base() + tlsTemplate.address, tlsTemplate.size);

    *GuestPtr<be32>(pcr + kTlsPointerOffset)    = tls;
    *GuestPtr<be32>(pcr + kCurrentThreadOffset) = kthread;
    *GuestPtr<be32>(kthread + kThreadIdOffset)  = threadId;
    return pcr;
}

void kernel::Unimplemented(const char* name)
{
    std::lock_guard g(g_unimplLock);
    auto [it, inserted] = g_unimpl.try_emplace(name, 0);
    it->second++;
    if (inserted) LOGW("unimplemented: %s", name);
}

void kernel::ReportUnimplemented()
{
    if constexpr (!diag::kOn) return;
    std::lock_guard g(g_unimplLock);
    if (g_unimpl.empty()) { LOGI("no unimplemented imports were reached"); return; }
    std::vector<std::pair<std::string, uint64_t>> v(g_unimpl.begin(), g_unimpl.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
    LOGI("unimplemented imports actually reached (%zu distinct):", v.size());
    for (auto& [name, count] : v) LOGI("  %8llu  %s", (unsigned long long)count, name.c_str());
}

PPCFunc* kernel::GuestFunction(uint32_t address)
{
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
    return PPC_LOOKUP_FUNC(guest::Base(), address);
}

void kernel::CallGuest(const PPCContext& caller, uint32_t address,
                       uint32_t a0, uint32_t a1, uint32_t a2)
{
    PPCFunc* fn = GuestFunction(address);
    if (!fn) { LOGW("CallGuest: no recompiled function at %08X", address); return; }

    PPCContext ctx{};
    std::memset(&ctx, 0, sizeof(ctx));
    // Leave the caller's frame and its 64-byte red zone untouched.
    ctx.r1.u32  = (caller.r1.u32 - 0x400) & ~0xFu;
    ctx.r13.u32 = caller.r13.u32;
    ctx.r3.u32  = a0;
    ctx.r4.u32  = a1;
    ctx.r5.u32  = a2;
    // The caller's code keeps its own idea of the float mode (whether
    // denormals flush: on for vector code, off for scalar) and only switches
    // when it thinks the mode differs. The console keeps the two in separate
    // registers, FPSCR and VSCR; x86 has one, MXCSR, and the recompiled code
    // tracks which it last set. A routine run here in a context of its own can
    // leave MXCSR switched -- vector code turns flushing on -- and the caller,
    // believing it off, would flush every tiny value its scalar code makes from
    // then on. So the caller gets the register back as it left it.
    const uint32_t before = ctx.fpscr.getcsr();
    ctx.fpscr.loadFromHost();
    fn(ctx, guest::Base());
    if (ctx.fpscr.getcsr() != before) ctx.fpscr.setcsr(before);
}

namespace
{
    struct PendingApc
    {
        uint32_t routine, context, ioStatusBlock;
        std::function<void()> work;
    };
    thread_local std::vector<PendingApc> t_apcs;
}

void kernel::QueueUserApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock,
                          std::function<void()> work)
{
    if (!routine) { if (work) work(); return; }
    t_apcs.push_back({ routine, context, ioStatusBlock, std::move(work) });
}

bool kernel::DeliverUserApcs(const PPCContext& caller)
{
    if (t_apcs.empty()) return false;
    // An APC may queue more work, so drain rather than iterate.
    std::vector<PendingApc> batch;
    batch.swap(t_apcs);
    for (const auto& a : batch)
    {
        if (a.work) a.work();
        CallGuest(caller, a.routine, a.context, a.ioStatusBlock, 0);
    }
    return true;
}

uint32_t kernel::ExecutableXexHeader() { return g_executableXexHeader; }

// Turn a guest fault or hang into something readable. The recompiled functions
// are ordinary global symbols named sub_XXXXXXXX, so a host backtrace doubles
// as the guest call stack.
#include "crash.h"
#include "kernel/kernel.h"
#include "guest_memory.h"
#include "watchpoint.h"
#include "gpu/memory_watch.h"
#include "log.h"
#include "diagnostics.h"
#include "platform.h"
#include "report.h"
#include "gpu/internal.h"
#include "gpu/gpu.h"
#include <ppc_config.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

// Where start-up has got to (crash::Step): a process that is killed, or dies
// inside a driver, leaves this as the last line of its log, and a fault's
// report quotes it.
namespace
{
    std::atomic<const char*> g_step{ nullptr };
}

void crash::Step(const char* name, const char* what)
{
    g_step.store(name, std::memory_order_relaxed);
    LOGI("step: %s (%s)", name, what);
    // MW2_DEBUG_CRASH_AT=<step>: a fault here, on purpose, to see that a
    // crash this early is reported whole (docs/switches.md). Read from the
    // environment as it is now: the first steps come before the switches
    // are loaded.
    if (const char* at = std::getenv("MW2_DEBUG_CRASH_AT"); at && !std::strcmp(at, name))
    {
        LOGW("step: MW2_DEBUG_CRASH_AT=%s: writing through a null pointer here, on purpose", name);
        int* volatile nothing = nullptr;
        *nothing = 1;
    }
}

const char* crash::LastStep()
{
    const char* step = g_step.load(std::memory_order_relaxed);
    return step ? step : "none";
}

#ifndef _WIN32
#if !defined(__ANDROID__)
#include <execinfo.h>   // bionic has none; platform::PrintBacktrace unwinds instead
#endif
#if defined(__aarch64__)
#include <sys/ucontext.h>   // faulting host PC + registers in the crash log
#endif
// The Android library's file name, which cmake/android.cmake gives (MW3's is
// named for its title). An arm64 desktop build is an executable: nothing in
// its maps has this name, and no address is marked.
#ifndef MW2_LIBRARY_FILE
#define MW2_LIBRARY_FILE "libmw2.so"
#endif
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#ifdef MW2_ANDROID
#include "android/android.h"
#endif

namespace
{
    struct ThreadEntry { pthread_t handle; const char* name; };
    std::mutex g_threadsLock;
    std::vector<ThreadEntry> g_threads;
    // Never shrinks: the wait and predicate records quote a thread's name
    // after the thread has gone.
    std::deque<std::string> g_names;
    thread_local const char* t_name = nullptr;
    std::atomic<bool> g_dumping{ false };

    void DumpBacktrace(const char* why)
    {
#if defined(__ANDROID__)
        // The same walk, through the unwinder bionic ships (platform.cpp).
        platform::PrintBacktrace(why);
#else
        void* frames[64];
        int n = backtrace(frames, 64);
        std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
        std::fflush(stderr);
        backtrace_symbols_fd(frames, n, 2);
        std::fprintf(stderr, "[E] --------------------------------\n");
        std::fflush(stderr);
#endif
    }

    // The line of /proc/self/maps that covers an address, straight from the
    // file: the only account of the address space that cannot be out of
    // date. Raw syscalls and no allocation, because this runs in a signal
    // handler after something has already gone wrong.
    void PrintMapping(const void* address)
    {
        const int fd = ::open("/proc/self/maps", O_RDONLY);
        if (fd < 0) return;
        const uintptr_t want = uintptr_t(address);
        char buffer[8192];
        std::string held;
        ssize_t got;
        bool found = false;
        while (!found && (got = ::read(fd, buffer, sizeof buffer)) > 0)
        {
            held.append(buffer, size_t(got));
            size_t start = 0;
            for (;;)
            {
                const size_t nl = held.find('\n', start);
                if (nl == std::string::npos) break;
                const std::string line = held.substr(start, nl - start);
                start = nl + 1;
                unsigned long long from = 0, to = 0;
                if (std::sscanf(line.c_str(), "%llx-%llx", &from, &to) == 2 &&
                    want >= from && want < to)
                {
                    std::fprintf(stderr, "[E] mapped as: %s\n", line.c_str());
                    found = true;
                    break;
                }
            }
            held.erase(0, start);
        }
        if (!found) std::fprintf(stderr, "[E] mapped as: nothing -- the address is in no mapping\n");
        ::close(fd);
    }

    void DescribeFault(int sig, siginfo_t* info, const void* addr)
    {
        const char* what = "?";
        if (sig == SIGSEGV)
            what = info->si_code == SEGV_MAPERR ? "SEGV_MAPERR, nothing is mapped there"
                 : info->si_code == SEGV_ACCERR ? "SEGV_ACCERR, mapped but the access is not allowed"
                                                : "an unfamiliar SIGSEGV code";
        else if (sig == SIGBUS)
            what = info->si_code == BUS_ADRALN ? "BUS_ADRALN, the address is misaligned"
                 : info->si_code == BUS_ADRERR ? "BUS_ADRERR, no object behind the mapping"
                 : info->si_code == BUS_OBJERR ? "BUS_OBJERR, the object behind it failed"
                                               : "an unfamiliar SIGBUS code";
        std::fprintf(stderr, "[E] %s (si_code %d)\n", what, info->si_code);
        PrintMapping(addr);
    }

    // How a fault's report ends. On Android the process then dies of the
    // signal itself, through the handlers that were there before this one
    // (the system's, which write a tombstone): the system then records a
    // native crash with its own backtrace, which the app's log share reads
    // back (LogSummary.kt), where _exit recorded "exited by itself" and
    // nothing else. A fault repeats when its instruction runs again; a
    // signal that was sent (abort, a refused system call) is sent again.
    // The first thread here ends the process; another waits for that.
#ifdef MW2_ANDROID
    constexpr int kFatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGSYS };
    struct sigaction g_before[std::size(kFatal)];
#endif

    void EndOfFault(int sig, bool sent)
    {
        crash::OnFault();
#ifdef MW2_ANDROID
        static std::atomic<bool> ending{ false };
        if (ending.exchange(true)) for (;;) pause();
        // The report went through a pipe another thread reads into the log.
        android::SettleStandardStreams();
        for (size_t i = 0; i < std::size(kFatal); i++) sigaction(kFatal[i], &g_before[i], nullptr);
        if (sent) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
#else
        (void)sig; (void)sent;
        _exit(3);
#endif
    }

#ifdef MW2_ANDROID
    // abort() and a system call the device's policy refuses: a driver that
    // gives up, an uncaught C++ exception, a fortified libc call. Neither is
    // a fault at an address, so the report is the thread's stack.
    void SentSignalHandler(int sig, siginfo_t*, void*)
    {
        std::fprintf(stderr, "\n[E] ---- the process is ending on %s ----\n",
                     sig == SIGABRT ? "SIGABRT: abort() was called (a failed assertion, an uncaught"
                                      " exception, or a driver giving up)"
                                    : "SIGSYS: a system call this device's policy refuses");
        std::fprintf(stderr, "[E] last step: %s, on thread %s\n", crash::LastStep(), t_name ? t_name : "?");
        std::fflush(stderr);
        DumpBacktrace("backtrace");
        EndOfFault(sig, true);
    }
#endif

    void FaultHandler(int sig, siginfo_t* info, void* uctx)
    {
        // A write watchpoint takes the page's faults and steps over them; it is not
        // the process dying, so it must be asked first.
        if (sig == SIGSEGV && watchpoint::HandleWrite(info, uctx)) return;
        // A write to a page the GPU's shadow of guest memory watches.
        //
        // Any SIGSEGV, whatever the kernel calls it. Writing to a page the
        // watch made read-only is SEGV_ACCERR on an x86 Linux desktop, and
        // this phone's kernel reports the very same thing as SEGV_MAPERR --
        // "nothing is mapped there", with /proc/self/maps showing the page
        // right where it should be, r--s, in the physical bank. Gating on
        // the code meant the handler was never asked, and an ordinary
        // watched write -- the event the watch exists for -- was taken for
        // a crash. The watch checks the address and its own record of the
        // page before it claims a fault, so asking it is safe.
        if (sig == SIGSEGV && gpu::watch::HandleFault(info->si_addr)) return;
        if (sig == SIGTRAP && watchpoint::HandleStep(uctx)) return;

        uint8_t* base = guest::Base();
        void* addr = info->si_addr;
        const char* name = (sig == SIGSEGV) ? "SIGSEGV"
                         : (sig == SIGBUS)  ? "SIGBUS"
                         : (sig == SIGFPE)  ? "SIGFPE"
                         : (sig == SIGTRAP) ? "SIGTRAP" : "SIGILL";

        std::fprintf(stderr, "\n[E] ---- guest fault: %s at host %p ----\n", name, addr);
        std::fprintf(stderr, "[E] last step: %s\n", crash::LastStep());
        // Which kind, in the kernel's own words. This is the difference
        // between "nothing is mapped there" and "it is mapped and you may
        // not write to it", and those have entirely different causes -- one
        // is a hole in the address space, the other is something that called
        // mprotect. Guessing between them has cost a day already.
        DescribeFault(sig, info, addr);
        // The recompiler turns the PowerPC trap instructions into a host trap, and
        // IW4 builds its asserts out of them -- so this is the title saying one of its
        // own invariants broke, not the runtime crashing.
        if (sig == SIGTRAP)
            std::fprintf(stderr, "[E] -> a guest trap instruction: an assert inside the title fired\n");
        if (base && addr >= (void*)base && addr < (void*)(base + 0x100000000ull))
        {
            uint64_t g = uint64_t((uint8_t*)addr - base);
            std::fprintf(stderr, "[E] guest address %08llX\n", (unsigned long long)g);
            if (g < 0x10000)
                std::fprintf(stderr, "[E] -> null dereference (a stub returned 0 where a pointer was wanted)\n");
            else if (g >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE &&
                     g <  PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2)
                std::fprintf(stderr, "[E] -> function table slot for guest code %08llX\n",
                             (unsigned long long)(PPC_CODE_BASE + (g - (PPC_IMAGE_BASE + PPC_IMAGE_SIZE)) / 2));
            else if (g >= PPC_IMAGE_BASE && g < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
                std::fprintf(stderr, "[E] -> inside the loaded image\n");
        }
        else
        {
            // Guest addresses are 32 bits, so a fault a few bytes past the top of the
            // window is a load that started just below 0x100000000 and ran off the end.
            int64_t delta = base ? int64_t((uint8_t*)addr - base) : 0;
            if (base && delta >= 0x100000000ll && delta < 0x100000010ll)
                std::fprintf(stderr,
                             "[E] a guest access ran off the top of the address space (%+lld bytes past)\n"
                             "[E] -> the pointer was around 0xFFFFFFFF: something returned -1 where an address was wanted\n",
                             (long long)(delta - 0x100000000ll));
            else
                std::fprintf(stderr, "[E] outside the guest space (offset %+lld) -- fault in host runtime code\n",
                             (long long)delta);
        }
        std::fflush(stderr);
        DumpBacktrace("fault backtrace");
#if defined(__aarch64__)
        // The unwinder often dies on the first host frame (no unwind tables
        // where it matters), leaving the fault site unknown. The signal
        // context names it exactly: host PC, thread, and registers. The PC
        // resolves offline against the build's symbol map / disassembly; a
        // register holding the fault address shows the data flow.
        // Async-signal-safe enough: plain reads + fprintf, like the rest here.
        {
            ucontext_t* uc = static_cast<ucontext_t*>(uctx);
            const unsigned long long pc = uc->uc_mcontext.pc;
            // This library's executable ranges, so code addresses below get a
            // mark (same /proc/self/maps walk as PrintMapping above). More
            // than one where the recompiled code is a library of its own.
            struct { unsigned long long from, to; } code[4] = {};
            int codeRanges = 0;
            const auto inCode = [&](unsigned long long address)
            {
                for (int i = 0; i < codeRanges; i++)
                    if (address >= code[i].from && address < code[i].to) return true;
                return false;
            };
            {
                const int fd = ::open("/proc/self/maps", O_RDONLY);
                if (fd >= 0)
                {
                    char buffer[4096];
                    std::string held;
                    ssize_t got;
                    while ((got = ::read(fd, buffer, sizeof buffer)) > 0)
                    {
                        held.append(buffer, size_t(got));
                        size_t start = 0;
                        for (;;)
                        {
                            const size_t nl = held.find('\n', start);
                            if (nl == std::string::npos) break;
                            const std::string line = held.substr(start, nl - start);
                            start = nl + 1;
                            unsigned long long from = 0, to = 0;
                            char perms[8] = {};
                            if (std::sscanf(line.c_str(), "%llx-%llx %7s", &from, &to, perms) == 3 &&
                                perms[0] == 'r' && perms[2] == 'x' && codeRanges < 4 &&
                                line.find(MW2_LIBRARY_FILE) != std::string::npos)
                            {
                                code[codeRanges].from = from;
                                code[codeRanges].to = to;
                                codeRanges++;
                            }
                        }
                        held.erase(0, start);
                    }
                    ::close(fd);
                }
            }
            std::fprintf(stderr, "[E] fault host pc %016llX on thread %s\n",
                         pc, t_name ? t_name : "?");
            for (int r = 0; r < 30; r += 2)
                std::fprintf(stderr, "[E]   x%-2d %016llX  x%-2d %016llX\n",
                             r, (unsigned long long)uc->uc_mcontext.regs[r],
                             r + 1, (unsigned long long)uc->uc_mcontext.regs[r + 1]);
            std::fprintf(stderr, "[E]   x30 %016llX  sp %016llX\n",
                         (unsigned long long)uc->uc_mcontext.regs[30],
                         (unsigned long long)uc->uc_mcontext.sp);
            std::fflush(stderr);
            // The return-address chain survives even when the fault PC is
            // garbage (as here): any word pointing into this library's code
            // is a caller and resolves offline. 32-bit words in the guest
            // code range read as guest callers directly.
            {
                const unsigned long long sp = uc->uc_mcontext.sp;
                std::fprintf(stderr, "[E] stack at %016llX (<= is this library's code):\n", sp);
                for (int i = 0; i < 64; i++)
                {
                    unsigned long long w = 0;
                    std::memcpy(&w, reinterpret_cast<const void*>(sp + (unsigned long long)i * 8), 8);
                    const bool inLib = inCode(w);
                    std::fprintf(stderr, "[E]   sp+%03x %016llX%s\n", i * 8, w, inLib ? " <=" : "");
                }
                std::fflush(stderr);
            }
            if (inCode(pc))
            {
                uint32_t word = 0;
                std::memcpy(&word, reinterpret_cast<const void*>(pc), 4);
                std::fprintf(stderr, "[E] fault instruction word %08X\n", word);
                std::fflush(stderr);
            }
        }
#endif
        EndOfFault(sig, false);
    }

    // MW2_DUMP=<hexaddr>[:words][,...] prints blocks of guest memory when the
    // watchdog fires. Several blocks, because the state that explains a stall is
    // rarely all in one place.
    void DumpOneBlock(const char* spec)
    {
        unsigned long addr = std::strtoul(spec, nullptr, 16);
        unsigned words = 16;
        if (const char* colon = std::strchr(spec, ':'); colon && (!std::strchr(spec, ',') || colon < std::strchr(spec, ',')))
            words = unsigned(std::strtoul(colon + 1, nullptr, 10));
        std::fprintf(stderr, "\n[E] guest memory at %08lX:\n", addr);
        for (unsigned i = 0; i < words; i++)
        {
            uint32_t v;
            std::memcpy(&v, guest::Base() + addr + i * 4, 4);
            v = __builtin_bswap32(v);
            std::fprintf(stderr, "[E]   +%-5u %08lX = %08X (%d)\n", i * 4, addr + i * 4, v, int(v));
        }
        std::fflush(stderr);
    }

    void DumpGuestMemory()
    {
        const char* spec = diag::Text("MW2_DUMP");
        if (!spec || !guest::Base()) return;
        for (const char* at = spec; at && *at; )
        {
            DumpOneBlock(at);
            const char* comma = std::strchr(at, ',');
            at = comma ? comma + 1 : nullptr;
        }
    }

    // Runs on each guest thread when the watchdog pokes it.
    void ThreadDumpHandler(int)
    {
        DumpBacktrace("thread stack");
        g_dumping.store(false, std::memory_order_release);
    }

    void DumpOtherThreads()
    {
        std::vector<ThreadEntry> threads;
        {
            std::lock_guard g(g_threadsLock);
            threads = g_threads;
        }
        pthread_t self = pthread_self();
        for (const auto& t : threads)
        {
            if (pthread_equal(t.handle, self)) continue;
            std::fprintf(stderr, "\n[E] ==== %s ====\n", t.name);
            std::fflush(stderr);
            // On Android only where the diagnostics are built: a stack is
            // written from a signal handler, which a release does not risk.
#if !defined(MW2_ANDROID) || MW2_DIAGNOSTICS
            g_dumping.store(true, std::memory_order_release);
            if (pthread_kill(t.handle, SIGUSR1) != 0) { g_dumping.store(false); continue; }
            // A last-gasp diagnostic, so a thread that never responds is skipped.
            for (int i = 0; i < 200 && g_dumping.load(std::memory_order_acquire); i++)
                usleep(1000);
#endif
        }
    }

    // SIGUSR2: every thread's stack, and the run goes on -- sent a few times
    // over a hang, it tells a thread that is stuck from one that is looping.
    void SampleHandler(int)
    {
        DumpBacktrace("sampled, this thread is here");
        DumpOtherThreads();
    }

    void AlarmHandler(int)
    {
        DumpGuestMemory();
        kernel::ReportWaits();
        DumpBacktrace("watchdog fired, this thread is here");
        DumpOtherThreads();
        crash::OnFault();
        _exit(5);
    }
    std::atomic<bool> g_hangMonitorRunning{ false };
    std::thread g_hangMonitorThread;

    void HangMonitorLoop()
    {
        uint32_t lastLoggedSec = 0;
#if !(defined(MW2_ANDROID) && !defined(MW3_TITLE))
        // The arena wait already reported, so one stuck wait is one line.
        uint32_t reportedSeq = 0, reportedPos = 0;
        bool reportedWait = false;
#endif
        while (g_hangMonitorRunning.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!g_hangMonitorRunning.load(std::memory_order_relaxed)) break;

            auto batch = gpu::detail::CurrentBatchStatus();

            uint32_t neededSeq = 0, neededPos = 0;
            if (gpu::CheckArenaWaitDeadlock(2000000ull, neededSeq, neededPos))
            {
                if (!batch.inBatch || batch.readIndex == batch.wptr)
                {
                    const uint32_t block = gpu::GetArenaProgressBlock();
                    if (block)
                    {
#if defined(MW2_ANDROID) && !defined(MW3_TITLE)
                        // The MW2 phone build only: write what D3D is waiting
                        // for into its progress block.
                        const uint32_t posTag = (neededPos & ~3u) | (neededSeq & 3u);
                        std::fprintf(stderr, "[W] hang_monitor: T_D3D_ArenaWait spinning for >2s while GPU ring is idle (readIndex=%u, wptr=%u)! Auto-resolving arena deadlock to seq=%u, pos=%08X\n",
                                     batch.readIndex, batch.wptr, neededSeq, posTag);
                        std::fflush(stderr);
                        gpu::ForceArenaProgress(neededSeq, posTag);
#else
                        // Reported and left alone: the progress block holds
                        // what the command processor has read, and writing
                        // the awaited values into it would have the title
                        // overwrite commands that were never executed.
                        if (!reportedWait || reportedSeq != neededSeq || reportedPos != neededPos)
                        {
                            reportedWait = true;
                            reportedSeq = neededSeq;
                            reportedPos = neededPos;
                            std::fprintf(stderr, "[W] hang_monitor: T_D3D_ArenaWait spinning for >2s while GPU ring is idle (readIndex=%u, wptr=%u), waiting for seq=%u, pos=%08X\n",
                                         batch.readIndex, batch.wptr, neededSeq, neededPos);
                            std::fflush(stderr);
                        }
#endif
                    }
                }
            }

            if (batch.inBatch && batch.elapsedMs >= 4000)
            {
                const char* drawStage = gpu::detail::CurrentDrawStage();
                const bool isCompiling = drawStage && std::strstr(drawStage, "Draw_EnsurePipeline") != nullptr;
                uint32_t sec = uint32_t(batch.elapsedMs / 1000);
                if (isCompiling)
                {
                    if (sec != lastLoggedSec && sec % 5 == 0)
                    {
                        lastLoggedSec = sec;
                        auto opcode = gpu::detail::CurrentOpcodeStatus();
                        std::fprintf(stderr, "[I] Shader compilation in progress (%llu ms): stage=%s, vHash=%016llX, pHash=%016llX\n",
                                     (unsigned long long)batch.elapsedMs, drawStage,
                                     (unsigned long long)opcode.lastVertexHash, (unsigned long long)opcode.lastPixelHash);
                        std::fflush(stderr);
                    }
                }
                else if (sec != lastLoggedSec && (sec % 3 == 0 || sec == 4))
                {
                    lastLoggedSec = sec;
                    auto opcode = gpu::detail::CurrentOpcodeStatus();
                    std::fprintf(stderr, "\n[E] ==================================================\n");
                    std::fprintf(stderr, "[E] HANG DETECTED: GPU ring batch running for %llu ms!\n", (unsigned long long)batch.elapsedMs);
                    std::fprintf(stderr, "[E]   Batch stats: available=%u, readIndex=%u, wptr=%u\n", batch.available, batch.readIndex, batch.wptr);
                    std::fprintf(stderr, "[E]   Current opcode: %u (%s), depth=%u, batchWork=%llu\n", opcode.opcode, opcode.name ? opcode.name : "?", opcode.depth, (unsigned long long)opcode.batchWork);
                    std::fprintf(stderr, "[E]   Draw stage: %s, last vHash=%016llX, pHash=%016llX\n", drawStage ? drawStage : "none", (unsigned long long)opcode.lastVertexHash, (unsigned long long)opcode.lastPixelHash);
                    std::fprintf(stderr, "[E] ==================================================\n");
                    std::fflush(stderr);

                    kernel::ReportWaits();
                    crash::DumpAllThreads("GPU ring batch hang");
                }
            }
        }
    }
}

void crash::DumpAllThreads(const char* reason)
{
    if (reason)
        std::fprintf(stderr, "\n[E] ==== HANG / THREAD DUMP: %s ====\n", reason);
    std::fflush(stderr);
    DumpBacktrace("caller thread backtrace");
    DumpOtherThreads();
}

void crash::StartHangMonitor()
{
    if (g_hangMonitorRunning.exchange(true)) return;
    g_hangMonitorThread = std::thread(HangMonitorLoop);
}

void crash::StopHangMonitor()
{
    if (!g_hangMonitorRunning.exchange(false)) return;
    if (g_hangMonitorThread.joinable())
    {
        if (g_hangMonitorThread.get_id() != std::this_thread::get_id())
            g_hangMonitorThread.join();
        else
            g_hangMonitorThread.detach();
    }
}

void crash::RegisterThread(const char* name)
{
    report::ThreadStarted(name);
    std::lock_guard g(g_threadsLock);
    t_name = g_names.emplace_back(name).c_str();
    g_threads.push_back({ pthread_self(), t_name });
    // The system's name as well, so a profiler or /proc tells the threads apart.
    // Fifteen characters is all it keeps. Not the main thread's: its name is
    // the process's, and `pgrep mw2` has to go on finding it.
    if (!platform::IsMainThread()) platform::SetThreadName(name);
#if defined(MW2_ANDROID) && MW2_DIAGNOSTICS
    // A thread started from the Java side inherits its mask, with SIGUSR1
    // blocked for the virtual machine's own catcher: the thread dump's signal
    // would wait for ever.
    sigset_t dump;
    sigemptyset(&dump);
    sigaddset(&dump, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &dump, nullptr);
#endif
}

const char* crash::CurrentThreadName() { return t_name; }

void crash::UnregisterThread()
{
    report::ThreadEnded();
    std::lock_guard g(g_threadsLock);
    pthread_t self = pthread_self();
    for (auto it = g_threads.begin(); it != g_threads.end(); ++it)
        if (pthread_equal(it->handle, self)) { g_threads.erase(it); return; }
}

void crash::Install()
{
    struct sigaction sa{};
    sa.sa_sigaction = FaultHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
#ifdef MW2_ANDROID
    struct sigaction sent = sa;
    sent.sa_sigaction = SentSignalHandler;
    for (size_t i = 0; i < std::size(kFatal); i++)
        sigaction(kFatal[i], kFatal[i] == SIGABRT || kFatal[i] == SIGSYS ? &sent : &sa, &g_before[i]);
#else
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
#endif

    struct sigaction dump{};
    dump.sa_handler = ThreadDumpHandler;
    dump.sa_flags = SA_RESTART;
    sigemptyset(&dump.sa_mask);
    sigaction(SIGUSR1, &dump, nullptr);
    if constexpr (diag::kOn) std::signal(SIGUSR2, SampleHandler);

    if (const unsigned seconds = unsigned(diag::Number("MW2_WATCHDOG")))
    {
        std::signal(SIGALRM, AlarmHandler);
        alarm(seconds);
        LOGI("watchdog armed for %u seconds", seconds);
    }

    StartHangMonitor();
}

extern "C" [[noreturn]] void MW2ReportBadIndirectCall(unsigned int guestAddress)
{
    std::fprintf(stderr, "\n[E] ---- indirect call to %08X has no recompiled function ----\n", guestAddress);
    if (guestAddress == 0)
        std::fprintf(stderr, "[E] target is null: the guest read a function pointer that was never written\n");
    else if (guestAddress < PPC_CODE_BASE || guestAddress >= PPC_CODE_BASE + PPC_CODE_SIZE)
        std::fprintf(stderr, "[E] target is outside the recompiled code range\n");
    else
        std::fprintf(stderr, "[E] target is inside the code range but was not identified as a function start\n");
    DumpBacktrace("indirect call backtrace");
    crash::OnFault();
    std::exit(4);
}
#else
// Windows: one vectored exception handler does what the signal handlers do --
// first the GPU watch's write faults and the pages the guest touches for the
// first time, then the report of a real fault.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    std::mutex g_threadsLock;
    std::deque<std::string> g_names;
    thread_local const char* t_name = nullptr;

    const char* Describe(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:    return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "integer divide by zero";
        case EXCEPTION_BREAKPOINT:          return "trap";
        case EXCEPTION_STACK_OVERFLOW:      return "stack overflow";
        case EXCEPTION_IN_PAGE_ERROR:       return "page error";
        default:                            return nullptr;
        }
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* exception)
    {
        const EXCEPTION_RECORD* record = exception->ExceptionRecord;
        const DWORD code = record->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            const bool write = record->ExceptionInformation[0] == 1;
            const void* at = reinterpret_cast<const void*>(record->ExceptionInformation[1]);
            if (write && gpu::watch::HandleFault(at)) return EXCEPTION_CONTINUE_EXECUTION;
            if (guest::CommitOnTouch(at)) return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Everything else Windows raises -- C++ exceptions, debug output -- is
        // someone else's to handle.
        const char* name = Describe(code);
        if (!name) return EXCEPTION_CONTINUE_SEARCH;

        std::fprintf(stderr, "\n[E] ---- guest fault: %s at %p ----\n", name, record->ExceptionAddress);
        std::fprintf(stderr, "[E] last step: %s\n", crash::LastStep());
        if (code == EXCEPTION_BREAKPOINT)
            std::fprintf(stderr, "[E] -> a guest trap instruction: an assert inside the title fired\n");
        if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            const auto* at = reinterpret_cast<const uint8_t*>(record->ExceptionInformation[1]);
            const uint8_t* base = guest::Base();
            if (base && at >= base && at < base + 0x100000000ull)
                std::fprintf(stderr, "[E] guest address %08llX\n", (unsigned long long)(at - base));
            else
                std::fprintf(stderr, "[E] host address %p, outside the guest space\n", static_cast<const void*>(at));
        }
        std::fflush(stderr);
        platform::PrintBacktrace("fault backtrace");
        crash::OnFault();
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void crash::Install()
{
    AddVectoredExceptionHandler(1, Handler);
    if (const unsigned seconds = unsigned(diag::Number("MW2_WATCHDOG")))
    {
        LOGI("watchdog armed for %u seconds", seconds);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            kernel::ReportWaits();
            crash::OnFault();
            TerminateProcess(GetCurrentProcess(), 5);
        }).detach();
    }
}

void crash::RegisterThread(const char* name)
{
    report::ThreadStarted(name);
    std::lock_guard g(g_threadsLock);
    t_name = g_names.emplace_back(name).c_str();
    if (!platform::IsMainThread()) platform::SetThreadName(name);
}

const char* crash::CurrentThreadName() { return t_name; }
void crash::UnregisterThread() { report::ThreadEnded(); }

void crash::DumpAllThreads(const char* reason)
{
    if (reason)
        std::fprintf(stderr, "\n[E] ==== HANG / THREAD DUMP: %s ====\n", reason);
    platform::PrintBacktrace("caller thread backtrace");
}
void crash::StartHangMonitor() {}
void crash::StopHangMonitor() {}

extern "C" [[noreturn]] void MW2ReportBadIndirectCall(unsigned int guestAddress)
{
    std::fprintf(stderr, "\n[E] ---- indirect call to %08X has no recompiled function ----\n", guestAddress);
    platform::PrintBacktrace("indirect call backtrace");
    crash::OnFault();
    std::exit(4);
}
#endif

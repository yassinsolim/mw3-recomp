#pragma once
// The few things the runtime asks of the host system that Linux and Windows
// spell differently. Everything else goes through the C++ library or SDL.
#include <cstddef>
#include <cstdint>

namespace platform
{
    // First thing in main, before any other thread starts: Windows sleeps in
    // 15.6 ms steps unless asked for 1 ms, and the title paces itself with
    // short sleeps; and a release build there finds the terminal it was
    // started from, if any.
    void Initialise();

    // The system's id for the calling thread, as a profiler shows it.
    uint32_t ThreadId();
    bool IsMainThread();
    // Up to fifteen characters are kept on Linux.
    void SetThreadName(const char* name);
    // Background work that must not take time from the frame.
    void LowerThreadPriority();
    // Only the time nothing else wants.
    void IdleThreadPriority();

    enum class ThreadAffinityRole {
        PrimeCore,       // The ring consumer: the one stage every frame goes through, the only thread let on the fastest core
        PerformanceCore, // The title's threads, the command recorder, the texture readers
        WorkerCore,      // Background shader warmup / audio / file I/O
        SpareCore,       // A title's threads beyond the ones that make its frame in turn: the performance and the efficiency cores
        Default          // OS scheduler default
    };

    // Pins a thread (ThreadId's number; 0 is the calling thread) to the SoC
    // core cluster appropriate for its role.
    void SetThreadAffinity(ThreadAffinityRole role, uint32_t thread = 0);
    // How many cores that cluster has; 0 where threads are not pinned (the
    // desktop), so a caller sizing a pool falls back to its own number.
    uint32_t CoreCount(ThreadAffinityRole role);

    // The calling thread's host stack to stderr. The recompiled functions are
    // named sub_XXXXXXXX, so where symbols resolve this is the guest's stack.
    void PrintBacktrace(const char* why);

    // What a bug report says about the machine's load (report.cpp).
    // A clock of the calling thread's processor time that any thread can read;
    // 0 when there is none.
    uintptr_t ThreadClock();
    // The thread's processor time so far; false once it has ended.
    bool ThreadTime(uintptr_t clock, uint64_t& nanoseconds);
    // How long the thread `id` (ThreadId) has stood ready to run with no
    // processor thread to run on; false where the system does not say.
    bool ThreadReadyTime(uint32_t id, uint64_t& nanoseconds);
    // Processor time so far: this process's, and the busy and whole time of
    // every thread of the processor together.
    struct Load { uint64_t process = 0, busy = 0, total = 0; };
    bool ProcessorLoad(Load& load);
    // In bytes; 0 where the system does not say.
    struct Memory
    {
        uint64_t installed = 0, usable = 0, free = 0;
        uint64_t commitLimit = 0, committed = 0;           // the whole system's
        uint64_t processPeak = 0, processCommitted = 0;    // this process's
    };
    Memory MemoryState();

    // A detached host thread with a stack of at least this size. The
    // recompiled functions carry the guest's frames on the host stack, and
    // the guest's are large: a title thread that recurses through a few
    // hundred of them wants several megabytes. glibc gives a new thread 8 MB
    // and the question never comes up; bionic gives it 1 MB, and the run
    // ends in a stack overflow somewhere deep in the recompiled code with
    // nothing to show for it. Returns false only if the thread could not be
    // started at all.
    bool StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                     const char* name);

    // A shared library: loaded, or only if something already loaded it.
    void* OpenLibrary(const char* path);
    void* LoadedLibrary(const char* name);
    void* Symbol(void* library, const char* name);
}

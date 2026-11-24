#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>
#include <string>
#else
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
// bionic has no execinfo.h: Android's unwinder is reached another way, and
// the backtrace below is written by hand there.
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef _WIN32

#include <cmath>

// C23's round-half-to-even, which the recompiled vector rounding reaches
// through simde and the UCRT does not have. Whatever the rounding mode: a tie
// goes to the even neighbour, anything else to the nearer one.
extern "C" float roundevenf(float x)
{
    const float nearest = std::round(x);   // ties away from zero
    if (std::fabs(nearest - x) != 0.5f) return nearest;
    return 2.0f * std::round(x * 0.5f);
}

namespace { DWORD g_mainThread = 0; }

void platform::Initialise()
{
    g_mainThread = GetCurrentThreadId();
    timeBeginPeriod(1);
    // A player's build has no console window of its own. Started from a
    // terminal, it writes there, as a console program would.
    if (!GetConsoleWindow() && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

uint32_t platform::ThreadId() { return uint32_t(GetCurrentThreadId()); }
bool platform::IsMainThread() { return GetCurrentThreadId() == g_mainThread; }

void platform::SetThreadName(const char* name)
{
    wchar_t wide[64];
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    if (n <= 0) return;
    // Windows 10 1607 and later; looked up, so older systems just go without.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set = reinterpret_cast<SetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), wide);
}

void platform::LowerThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
void platform::IdleThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE); }

// Only Android pins threads (below): a desktop scheduler places them better
// than a guess at the machine's cores would.
void platform::SetThreadAffinity(ThreadAffinityRole, uint32_t) {}
uint32_t platform::CoreCount(ThreadAffinityRole) { return 0; }

// No symbols without a PDB: each frame as its offset in the executable, which
// `llvm-symbolizer --obj=mw2.exe` turns back into a name on the build machine.
void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "\n[E] ---- %s (%u frames) ----\n", why, unsigned(n));
    for (USHORT i = 0; i < n; i++)
    {
        const auto at = reinterpret_cast<uintptr_t>(frames[i]);
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frames[i]), &owner);
        if (reinterpret_cast<uintptr_t>(owner) == module)
            std::fprintf(stderr, "[E]   exe+0x%llx\n", (unsigned long long)(at - module));
        else
            std::fprintf(stderr, "[E]   %p\n", frames[i]);
    }
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    DWORD WINAPI RunThread(LPVOID raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return 0;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    auto* start = new ThreadStart{ body, argument, name };
    const HANDLE thread = CreateThread(nullptr, stackBytes, &RunThread, start, 0, nullptr);
    if (!thread) { delete start; return false; }
    CloseHandle(thread);   // detached: nothing joins these
    return true;
}

void* platform::OpenLibrary(const char* path)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n);
    // The library's own folder is searched for what it loads in turn.
    return LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

uintptr_t platform::ThreadClock()
{
    HANDLE self = nullptr;
    // A handle of its own: GetCurrentThread's means whichever thread asks.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        return 0;
    return reinterpret_cast<uintptr_t>(self);
}

namespace
{
    uint64_t Nanoseconds(FILETIME time)
    {
        return ((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime) * 100;
    }
}

bool platform::ThreadTime(uintptr_t clock, uint64_t& nanoseconds)
{
    FILETIME created, exited, kernel, user;
    if (!clock || !GetThreadTimes(reinterpret_cast<HANDLE>(clock), &created, &exited, &kernel, &user))
        return false;
    nanoseconds = Nanoseconds(kernel) + Nanoseconds(user);
    return true;
}

bool platform::ThreadReadyTime(uint32_t, uint64_t&) { return false; }

bool platform::ProcessorLoad(Load& load)
{
    FILETIME created, exited, kernel, user, idle;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    load.process = Nanoseconds(kernel) + Nanoseconds(user);
    // The system's kernel time has its idle time in it.
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    load.total = Nanoseconds(kernel) + Nanoseconds(user);
    load.busy = load.total - Nanoseconds(idle);
    return true;
}

platform::Memory platform::MemoryState()
{
    Memory memory;
    ULONGLONG kilobytes = 0;
    if (GetPhysicallyInstalledSystemMemory(&kilobytes)) memory.installed = uint64_t(kilobytes) << 10;
    MEMORYSTATUSEX status{ sizeof(status) };
    if (GlobalMemoryStatusEx(&status))
    {
        memory.usable = status.ullTotalPhys;
        memory.free = status.ullAvailPhys;
        memory.commitLimit = status.ullTotalPageFile;
        memory.committed = status.ullTotalPageFile - status.ullAvailPageFile;
    }
    PROCESS_MEMORY_COUNTERS counters{ sizeof(counters) };
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        memory.processPeak = counters.PeakWorkingSetSize;
        memory.processCommitted = counters.PeakPagefileUsage;
    }
    return memory;
}

void* platform::LoadedLibrary(const char* name) { return GetModuleHandleA(name); }

void* platform::Symbol(void* library, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

#else

void platform::Initialise() {}
uint32_t platform::ThreadId() { return uint32_t(gettid()); }
bool platform::IsMainThread() { return gettid() == getpid(); }

void platform::SetThreadName(const char* name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
}

void platform::LowerThreadPriority() { setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10); }

void platform::IdleThreadPriority()
{
    sched_param none{};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &none) != 0) LowerThreadPriority();
}

#if defined(__ANDROID__)
namespace
{
    struct CpuTopology
    {
        uint32_t primeMask = 0;
        uint32_t perfMask = 0;
        uint32_t workerMask = 0;
        bool discovered = false;
    };

    CpuTopology DiscoverCpuTopology()
    {
        CpuTopology topo;
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (cpus <= 1) return topo;

        const int maxCpus = std::min(int(cpus), 32);
        struct CoreFreq { int id; unsigned long freq; };
        std::vector<CoreFreq> freqs;

        for (int i = 0; i < maxCpus; ++i)
        {
            char path[128];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
            FILE* f = std::fopen(path, "r");
            unsigned long freq = 0;
            if (f)
            {
                if (std::fscanf(f, "%lu", &freq) == 1) freqs.push_back({ i, freq });
                std::fclose(f);
            }
        }

        if (freqs.size() == size_t(maxCpus) && !freqs.empty())
        {
            std::sort(freqs.begin(), freqs.end(), [](const CoreFreq& a, const CoreFreq& b) {
                return a.freq > b.freq;
            });

            unsigned long maxFreq = freqs[0].freq;
            for (const auto& c : freqs)
            {
                if (c.freq >= maxFreq * 95 / 100)
                    topo.primeMask |= (1u << c.id);
                else if (c.freq >= maxFreq * 75 / 100)
                    topo.perfMask |= (1u << c.id);
                else
                    topo.workerMask |= (1u << c.id);
            }
            if (!topo.perfMask) topo.perfMask = topo.primeMask;
            if (!topo.workerMask) topo.workerMask = topo.perfMask;
        }
        else
        {
            if (maxCpus >= 8)
            {
                topo.primeMask = (1u << 7);
                topo.perfMask = (1u << 4) | (1u << 5) | (1u << 6);
                topo.workerMask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
            }
            else if (maxCpus >= 4)
            {
                topo.primeMask = (1u << (maxCpus - 1));
                for (int i = maxCpus / 2; i < maxCpus; ++i) topo.perfMask |= (1u << i);
                for (int i = 0; i < maxCpus / 2; ++i) topo.workerMask |= (1u << i);
            }
            else
            {
                topo.primeMask = 1u;
                topo.perfMask = 1u;
                topo.workerMask = 1u;
            }
        }

        topo.discovered = true;
        return topo;
    }

    const CpuTopology& Topology()
    {
        static const CpuTopology topology = DiscoverCpuTopology();
        return topology;
    }

    uint32_t MaskOf(const CpuTopology& topology, platform::ThreadAffinityRole role)
    {
        switch (role)
        {
        // The performance cores as well as the fastest one. A thread held
        // to the one core has nowhere to go when something else takes it:
        // the ring consumer, runnable, got 0.19 s of processor in 4.4 s on
        // an AYN Thor's (a frame 4.1 s long), and a second or so of nearly
        // none nine more times in three minutes. What took it then was a
        // thread of this game's own that never slept (online/server.cpp,
        // since put right), but any thread the system runs there does the
        // same. The scheduler still gives the fastest core to the busiest
        // thread that may have it, which this one is.
        case platform::ThreadAffinityRole::PrimeCore:
            return topology.primeMask | topology.perfMask;
        case platform::ThreadAffinityRole::PerformanceCore:
            return topology.perfMask ? topology.perfMask : topology.primeMask;
        case platform::ThreadAffinityRole::WorkerCore:
            return topology.workerMask ? topology.workerMask : topology.perfMask;
        // Every core but the fastest, which stays the ring consumer's.
        case platform::ThreadAffinityRole::SpareCore:
            return topology.perfMask | topology.workerMask;
        default:
            return 0;
        }
    }
}

// Pins the thread to the SoC's core cluster for its role. Android
// only: a desktop scheduler places threads better than this would.
void platform::SetThreadAffinity(ThreadAffinityRole role, uint32_t thread)
{
    const CpuTopology& topology = Topology();
    if (!topology.discovered) return;
    const uint32_t targetMask = MaskOf(topology, role);
    if (!targetMask) return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 32; ++i)
    {
        if (targetMask & (1u << i))
            CPU_SET(i, &cpuset);
    }
    sched_setaffinity(pid_t(thread), sizeof(cpu_set_t), &cpuset);
}

uint32_t platform::CoreCount(ThreadAffinityRole role)
{
    const CpuTopology& topology = Topology();
    return topology.discovered ? uint32_t(__builtin_popcount(MaskOf(topology, role))) : 0;
}
#else
void platform::SetThreadAffinity(ThreadAffinityRole, uint32_t) {}
uint32_t platform::CoreCount(ThreadAffinityRole) { return 0; }
#endif

#if defined(__ANDROID__)
// _Unwind_Backtrace is in libgcc/libunwind and is what bionic itself uses.
// dladdr turns an address into a library and a symbol where there is one,
// which is enough to tell "crashed in the driver" from "crashed in the
// recompiled code" -- the two that matter here.
#include <unwind.h>

namespace
{
    struct Frames { void** at; int taken; int room; };

    _Unwind_Reason_Code TakeFrame(_Unwind_Context* context, void* argument)
    {
        auto* frames = static_cast<Frames*>(argument);
        const uintptr_t pc = _Unwind_GetIP(context);
        if (!pc) return _URC_NO_REASON;
        if (frames->taken >= frames->room) return _URC_END_OF_STACK;
        frames->at[frames->taken++] = reinterpret_cast<void*>(pc);
        return _URC_NO_REASON;
    }
}

void platform::PrintBacktrace(const char* why)
{
    void* addresses[64];
    Frames frames{ addresses, 0, 64 };
    _Unwind_Backtrace(&TakeFrame, &frames);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, frames.taken);
    for (int i = 0; i < frames.taken; i++)
    {
        Dl_info info{};
        if (dladdr(addresses[i], &info) && info.dli_fname)
        {
            const char* file = std::strrchr(info.dli_fname, '/');
            file = file ? file + 1 : info.dli_fname;
            const uintptr_t offset = uintptr_t(addresses[i]) - uintptr_t(info.dli_fbase);
            if (info.dli_sname)
                std::fprintf(stderr, "[E]  #%02d %s+0x%zx (%s)\n", i, info.dli_sname,
                             size_t(uintptr_t(addresses[i]) - uintptr_t(info.dli_saddr)), file);
            else
                std::fprintf(stderr, "[E]  #%02d %s+0x%zx\n", i, file, size_t(offset));
        }
        else
        {
            std::fprintf(stderr, "[E]  #%02d %p\n", i, addresses[i]);
        }
    }
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}
#else
void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}
#endif

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    void* RunThread(void* raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return nullptr;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackBytes)
    {
        // Rounded up to a page, and never below what the platform insists on.
        const size_t page = size_t(sysconf(_SC_PAGESIZE));
        size_t wanted = (stackBytes + page - 1) & ~(page - 1);
#ifdef PTHREAD_STACK_MIN
        if (wanted < size_t(PTHREAD_STACK_MIN)) wanted = size_t(PTHREAD_STACK_MIN);
#endif
        pthread_attr_setstacksize(&attributes, wanted);
    }
    auto* start = new ThreadStart{ body, argument, name };
    pthread_t thread{};
    const int made = pthread_create(&thread, &attributes, &RunThread, start);
    pthread_attr_destroy(&attributes);
    if (made != 0)
    {
        delete start;
        // A smaller stack is worth trying before giving up: a device short of
        // address space would rather have the thread than the headroom.
        pthread_t fallback{};
        auto* retry = new ThreadStart{ body, argument, name };
        if (pthread_create(&fallback, nullptr, &RunThread, retry) != 0)
        {
            delete retry;
            return false;
        }
        pthread_detach(fallback);
    }
    return true;
}

uintptr_t platform::ThreadClock()
{
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return 0;
    // Shifted past 0, which says there is none.
    return uintptr_t(uint32_t(clock)) + 1;
}

bool platform::ThreadTime(uintptr_t clock, uint64_t& nanoseconds)
{
    timespec time;
    if (!clock || clock_gettime(clockid_t(uint32_t(clock - 1)), &time) != 0) return false;
    nanoseconds = uint64_t(time.tv_sec) * 1000000000u + uint64_t(time.tv_nsec);
    return true;
}

bool platform::ThreadReadyTime(uint32_t id, uint64_t& nanoseconds)
{
    // "time on a processor, time waiting on a run queue, timeslices".
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%u/schedstat", id);
    std::FILE* file = std::fopen(path, "r");
    if (!file) return false;
    unsigned long long ran = 0, ready = 0;
    const bool read = std::fscanf(file, "%llu %llu", &ran, &ready) == 2;
    std::fclose(file);
    nanoseconds = ready;
    return read;
}

bool platform::ProcessorLoad(Load& load)
{
    timespec time;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) return false;
    load.process = uint64_t(time.tv_sec) * 1000000000u + uint64_t(time.tv_nsec);
    // The first line sums every processor thread, in ticks of USER_HZ.
    std::FILE* stat = std::fopen("/proc/stat", "r");
    if (!stat) return false;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    const int read = std::fscanf(stat, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle,
                                 &iowait, &irq, &softirq, &steal);
    std::fclose(stat);
    if (read < 4) return false;
    const uint64_t tick = 1000000000u / uint64_t(sysconf(_SC_CLK_TCK));
    load.total = (user + nice + system + idle + iowait + irq + softirq + steal) * tick;
    load.busy = load.total - (idle + iowait) * tick;
    return true;
}

namespace
{
    // A "Name: N kB" line of a /proc file, in bytes.
    uint64_t Kilobytes(const char* path, const char* name)
    {
        std::FILE* file = std::fopen(path, "r");
        if (!file) return 0;
        const size_t length = std::strlen(name);
        char line[256];
        uint64_t bytes = 0;
        while (std::fgets(line, sizeof line, file))
            if (!std::strncmp(line, name, length) && line[length] == ':')
            {
                bytes = std::strtoull(line + length + 1, nullptr, 10) << 10;
                break;
            }
        std::fclose(file);
        return bytes;
    }
}

platform::Memory platform::MemoryState()
{
    Memory memory;
    memory.usable = Kilobytes("/proc/meminfo", "MemTotal");
    memory.free = Kilobytes("/proc/meminfo", "MemAvailable");
    memory.processPeak = Kilobytes("/proc/self/status", "VmHWM");
    return memory;
}

void* platform::OpenLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* platform::LoadedLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }
void* platform::Symbol(void* library, const char* name) { return dlsym(library, name); }

#endif

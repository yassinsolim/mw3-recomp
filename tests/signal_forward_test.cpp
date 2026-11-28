// How the last fault handler hands a crash to the handler before it
// (runtime/signal_forward.h): from the dying thread, with its own siginfo and
// context, while another thread's write to a page the GPU watch made
// read-only is still taken by the runtime's handler and not by the system's.
// Case C0009: a Snapdragon 8 Elite's crash in Qualcomm's shader compiler was
// recorded by Android as a SEGV_ACCERR on the title's Database thread, whose
// next watched write reached the system's handler once crash.cpp had put the
// system's handlers back for the whole process.
#include "signal_forward.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <thread>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        if (ok) return;
        g_failures++;
        std::printf("FAILED: %s\n", what.c_str());
    }

    long Tid() { return long(syscall(SYS_gettid)); }

    constexpr size_t kPage = 4096;
    // Two pages: the one the "watch" keeps read-only, and the one a thread
    // crashes on (no access at all).
    char* g_pages = nullptr;
    char* Watched() { return g_pages; }
    char* Crashing() { return g_pages + kPage; }

    struct sigaction g_before {};

    std::atomic<int> g_watchFaults{ 0 };
    std::atomic<int> g_oursFatal{ 0 };
    std::atomic<siginfo_t*> g_oursInfo{ nullptr };
    std::atomic<void*> g_oursContext{ nullptr };

    std::atomic<int> g_previousCalls{ 0 };
    std::atomic<long> g_previousTid{ 0 };
    std::atomic<siginfo_t*> g_previousInfo{ nullptr };
    std::atomic<void*> g_previousContext{ nullptr };
    std::atomic<void*> g_previousAddress{ nullptr };
    std::atomic<bool> g_oursInstalledMeanwhile{ false };

    std::atomic<bool> g_letDatabaseWrite{ false };
    std::atomic<bool> g_databaseDone{ false };

    void Ours(int sig, siginfo_t* info, void* context);

    // The system's handler, as far as this test needs one: it is told which
    // thread crashed and where, and while it is busy (crash_dump attaching to
    // the threads, writing the tombstone) the other threads run on.
    void Previous(int sig, siginfo_t* info, void* context)
    {
        g_previousCalls++;
        g_previousTid = Tid();
        g_previousInfo = info;
        g_previousContext = context;
        g_previousAddress = info ? info->si_addr : nullptr;
        struct sigaction now {};
        sigaction(sig, nullptr, &now);
        g_oursInstalledMeanwhile = (now.sa_flags & SA_SIGINFO) && now.sa_sigaction == Ours;

        // The Database thread writes to a watched page now.
        g_letDatabaseWrite = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!g_databaseDone && std::chrono::steady_clock::now() < until) std::this_thread::yield();

        // Unlike the system's, this one lets the thread go on, so the test
        // can: the crashing page becomes writable and the write runs again.
        if (sig == SIGSEGV) mprotect(Crashing(), kPage, PROT_READ | PROT_WRITE);
    }

    // The runtime's handler, reduced to the two things crash.cpp's does with
    // a SIGSEGV: a write to a watched page is the watch's (the page is made
    // writable and the write runs again); anything else ends the process.
    void Ours(int sig, siginfo_t* info, void* context)
    {
        char* at = static_cast<char*>(info->si_addr);
        if (sig == SIGSEGV && at >= Watched() && at < Watched() + kPage)
        {
            mprotect(Watched(), kPage, PROT_READ | PROT_WRITE);
            g_watchFaults++;
            return;
        }
        g_oursFatal++;
        g_oursInfo = info;
        g_oursContext = context;
        signal_forward::End(g_before, sig, info, context, false);
    }

    void Install(int sig, void (*handler)(int, siginfo_t*, void*), struct sigaction* before)
    {
        struct sigaction action {};
        action.sa_sigaction = handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(sig, &action, before);
    }

    void ACrashWhileAnotherThreadWritesToAWatchedPage()
    {
        g_pages = static_cast<char*>(mmap(nullptr, 2 * kPage, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(g_pages != MAP_FAILED, "two pages mapped");
        if (g_pages == MAP_FAILED) return;
        mprotect(Watched(), kPage, PROT_READ);
        mprotect(Crashing(), kPage, PROT_NONE);

        struct sigaction original {};
        Install(SIGSEGV, Previous, &original);
        Install(SIGSEGV, Ours, &g_before);

        std::thread database([] {
            while (!g_letDatabaseWrite) std::this_thread::yield();
            volatile char* page = Watched();
            page[0] = 42;
            g_databaseDone = true;
        });
        std::atomic<long> workerTid{ 0 };
        std::thread worker([&] {
            workerTid = Tid();
            volatile char* page = Crashing();
            page[0] = 7;
        });
        worker.join();
        database.join();

        Check(g_oursFatal == 1, "the crash reached the runtime's handler once");
        Check(g_previousCalls == 1, "the handler before it was called once, for the crash and nothing else");
        Check(g_previousTid == workerTid, "on the thread that crashed");
        Check(g_previousAddress == Crashing(), "with the crash's address");
        Check(g_previousInfo == g_oursInfo && g_previousContext == g_oursContext,
              "with the very siginfo and context the kernel gave");
        Check(g_oursInstalledMeanwhile, "the runtime's handler stays installed while the system's runs");
        Check(g_databaseDone, "the Database thread's write went through");
        Check(g_watchFaults == 1, "and was taken by the watch, not recorded as the crash");
        Check(Watched()[0] == 42 && Crashing()[0] == 7, "both writes landed");

        sigaction(SIGSEGV, &original, nullptr);
        munmap(g_pages, 2 * kPage);
    }

    std::atomic<int> g_plainCalls{ 0 };
    std::atomic<int> g_plainSignal{ 0 };
    void Plain(int sig)
    {
        g_plainCalls++;
        g_plainSignal = sig;
    }

    void AHandlerWithoutSiginfoAndNone()
    {
        siginfo_t info {};
        struct sigaction plain {};
        plain.sa_handler = Plain;
        signal_forward::ToPrevious(plain, SIGBUS, &info, nullptr);
        Check(g_plainCalls == 1 && g_plainSignal == SIGBUS, "a handler without SA_SIGINFO is called with the signal");

        struct sigaction none {};
        none.sa_handler = SIG_DFL;
        signal_forward::ToPrevious(none, SIGBUS, &info, nullptr);
        none.sa_handler = SIG_IGN;
        signal_forward::ToPrevious(none, SIGBUS, &info, nullptr);
        Check(g_plainCalls == 1, "SIG_DFL and SIG_IGN are not called");
    }

    // A sent signal (abort, a refused system call) whose earlier handler
    // returns without ending the process: that handler is put back for this
    // signal and the signal sent again, as before.
    std::atomic<int> g_sentPrevious{ 0 };
    struct sigaction g_sentBefore {};
    void SentPrevious(int, siginfo_t*, void*) { g_sentPrevious++; }
    void SentOurs(int sig, siginfo_t* info, void* context)
    {
        signal_forward::End(g_sentBefore, sig, info, context, true);
    }

    void ASentSignalWhoseHandlerReturns()
    {
        struct sigaction original {};
        Install(SIGUSR1, SentPrevious, &original);
        Install(SIGUSR1, SentOurs, &g_sentBefore);
        raise(SIGUSR1);
        Check(g_sentPrevious == 2, "called from the runtime's handler, then for the signal sent again");
        struct sigaction now {};
        sigaction(SIGUSR1, nullptr, &now);
        Check(now.sa_sigaction == SentPrevious, "and left installed for that signal");
        sigaction(SIGUSR1, &original, nullptr);
    }
}

int main()
{
    ACrashWhileAnotherThreadWritesToAWatchedPage();
    AHandlerWithoutSiginfoAndNone();
    ASentSignalWhoseHandlerReturns();
    if (g_failures == 0) std::printf("signal forwarding: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}

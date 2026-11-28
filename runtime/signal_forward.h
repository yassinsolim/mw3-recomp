#pragma once

// How the runtime's last fault handler ends the process (crash.cpp,
// EndOfFault): the handler that was installed before ours -- on Android the
// system's, which writes the tombstone and the native crash the app's log
// share reads back -- is called from the dying thread, with that thread's
// siginfo and context, while every other thread keeps the runtime's handlers.
//
// The runtime's SIGSEGV handler is not only for crashes: it is how the GPU's
// watch of guest memory sees a write to a page it made read-only, hundreds
// of times a second while a level loads. Putting the system's handlers back
// for the whole process and letting the fault happen again (what crash.cpp
// did up to AW 0.1.6) left a window in which the next watched write on
// another thread went to the system as the crash: a Snapdragon 8 Elite's
// log had Qualcomm's shader compiler crash on a pipeline worker, the
// runtime's report of it, and Android's record of a SEGV_ACCERR at a page
// boundary on the title's Database thread, which was only loading a level.
#include <csignal>
#include <sys/syscall.h>
#include <unistd.h>

namespace signal_forward
{
    // Calls the handler [before] describes as the kernel would have, with
    // the same signal, siginfo and context. Nothing for SIG_DFL or SIG_IGN.
    inline void ToPrevious(const struct sigaction& before, int sig, siginfo_t* info, void* context)
    {
        if (before.sa_flags & SA_SIGINFO)
        {
            if (before.sa_sigaction) before.sa_sigaction(sig, info, context);
        }
        else if (before.sa_handler != SIG_DFL && before.sa_handler != SIG_IGN)
        {
            before.sa_handler(sig);
        }
    }

    // The end of a fatal signal's report on this thread. Android's handler
    // ends the process itself: it queues the signal again with the default
    // action, which is taken when the runtime's handler returns. Only where
    // there was none, or it returned without that, is [before] put back for
    // this one signal and the fault repeated (by returning) or the signal
    // sent again ([sent]: abort, a refused system call).
    inline void End(const struct sigaction& before, int sig, siginfo_t* info, void* context, bool sent)
    {
        ToPrevious(before, sig, info, context);
        sigaction(sig, &before, nullptr);
        if (sent) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
    }
}

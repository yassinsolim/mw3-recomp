// The display's blanks, as the system's Choreographer announces them.
//
// The presenter shows one frame a blank where the display paces the title
// (frame_rate.h). Left to the swapchain's queue to enforce -- FIFO, a present
// that waits while the queue is full -- the queue is what stays full: every
// frame reaches the screen a blank later than it need, behind the one queued
// before it. Counting the blanks here lets the presenter hand over one frame
// for each without the queue filling (presenter.cpp, MW2_PRESENT_AT_BLANK).
//
// A Choreographer calls back on the thread that asked, through that thread's
// looper, so this is a thread of its own that does nothing else.
#ifdef MW2_ANDROID

#include "android.h"
#include "../log.h"
#include "../platform.h"

#include <android/choreographer.h>
#include <android/looper.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    std::mutex g_lock;
    std::condition_variable g_wake;
    uint64_t g_count = 0;                      // blanks announced so far, under g_lock
    // Whether the thread below has been started, and what it found. Start
    // is the presenter thread's alone.
    enum : int { kNotAsked, kAsking, kCounting, kNone };
    std::atomic<int> g_state{ kNotAsked };

    void Announce(long, void* choreographer)
    {
        {
            std::lock_guard lock(g_lock);
            g_count++;
        }
        g_wake.notify_all();
        // One callback an asking: asked again for the next blank.
        AChoreographer_postFrameCallback(static_cast<AChoreographer*>(choreographer), Announce, choreographer);
    }

    void Thread()
    {
        platform::SetThreadName("display blanks");
        ALooper_prepare(0);
        AChoreographer* choreographer = AChoreographer_getInstance();
        if (!choreographer)
        {
            LOGW("blank: no Choreographer on this thread; the swapchain's queue paces the frames");
            g_state.store(kNone, std::memory_order_release);
            g_state.notify_all();
            return;
        }
        AChoreographer_postFrameCallback(choreographer, Announce, choreographer);
        g_state.store(kCounting, std::memory_order_release);
        g_state.notify_all();
        // The run's end is the process's: nothing here is waited for.
        while (android::Running()) ALooper_pollOnce(250, nullptr, nullptr, nullptr);
    }
}

bool android::blank::Start()
{
    if (g_state.load(std::memory_order_acquire) == kNotAsked)
    {
        g_state.store(kAsking, std::memory_order_release);
        std::thread(Thread).detach();
    }
    // The thread's answer, which is a moment away: a caller told yes by a
    // thread that then found no Choreographer would wait for blanks that
    // are never announced.
    g_state.wait(kAsking, std::memory_order_acquire);
    return g_state.load(std::memory_order_acquire) == kCounting;
}

uint64_t android::blank::WaitPast(uint64_t count, int64_t timeoutNs)
{
    std::unique_lock lock(g_lock);
    g_wake.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [&] { return g_count > count; });
    return g_count;
}

#endif  // MW2_ANDROID

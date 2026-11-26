#include "report.h"
#include "frame_rate.h"
#include "log.h"
#include "platform.h"
#include "gpu/memory_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // A frame every blank is 16.7 ms; the bounds are a blank and a half, then
    // two, three and four blanks.
    constexpr double kBounds[] = { 17.5, 25.0, 34.0, 50.0 };
    constexpr size_t kBuckets = std::size(kBounds) + 1;

    struct State
    {
        std::mutex lock;
        std::chrono::steady_clock::time_point first, last;
        bool lastWasWorld = false;
        uint64_t frames = 0, worldFrames = 0;
        // Between two world frames in a row: a loading screen in between is
        // not a slow frame.
        uint64_t intervals = 0, buckets[kBuckets]{};
        double intervalMs = 0, longestMs = 0;
        // Against the display's blank, whatever its rate: frames that came a
        // blank after the one before them, and within two. The 5% over is
        // what a frame's finishing wanders by; it applies to both, so a 60
        // a second frame on a 120 Hz display counts as two blanks.
        uint64_t withinABlank = 0, withinTwoBlanks = 0, againstDisplay = 0;
        // The consumer's times as they stood at the last frame, and what the
        // intervals counted above took of them.
        uint64_t spentAtLast[report::kTimeCount]{}, spentInPlay[report::kTimeCount]{};
        uint64_t costCount[report::kCostCount]{}, costMicroseconds[report::kCostCount]{},
                 costLongest[report::kCostCount]{};
        // How long frames took from the renderer to the window and to the
        // screen (report::Late), in the seconds that were all play.
        uint64_t lateInPlay[report::kLatenessCount]{}, lateCountInPlay[report::kLatenessCount]{};
        bool written = false;
    } s;

    // Written by the ring consumer between frames, outside the lock.
    std::atomic<uint64_t> g_spent[report::kTimeCount]{};
    // When the ring batch being executed began. A frame ends in the middle of
    // one, so its part up to there is counted with the frame.
    std::chrono::steady_clock::time_point g_executingSince;
    bool g_executing = false;

    std::atomic<uint64_t> g_shaders[3]{};   // by ShaderBuilt's fromCache, plus one
    std::atomic<uint64_t> g_pipelines[3]{}; // by PipelineBuilt's, likewise
    std::string g_compiled = "not used: a development run";
    // The share of its width the scene was last drawn at, in percent
    // (report::Scene); 0 where it is never drawn smaller.
    std::atomic<uint32_t> g_scene{ 0 };
    // The window's and the display's threads (report::Late).
    std::atomic<uint64_t> g_late[report::kLatenessCount]{}, g_lateCount[report::kLatenessCount]{};
}

void report::Scene(uint32_t percent) { g_scene.store(percent, std::memory_order_relaxed); }

void report::Late(Lateness of, uint64_t nanoseconds)
{
    g_late[of].fetch_add(nanoseconds, std::memory_order_relaxed);
    g_lateCount[of].fetch_add(1, std::memory_order_relaxed);
}

namespace
{
    // The machine's load in play, read once a second: whether the processor
    // was ours or another program's, which of our threads took it, and how
    // close the memory came to running out. A few system calls a second.
    struct Thread
    {
        std::string name;
        uintptr_t clock = 0;
        uint32_t id = 0;
        uint64_t last = 0, inPlay = 0;
        // Ready to run and not running: the threads outnumbering the cores
        // they may run on shows here and nowhere else.
        uint64_t lastReady = 0, readyInPlay = 0;
        uint64_t unsampled = 0;    // between the last reading and the thread's end
        bool read = false, ended = false;
    };
    std::mutex g_threadsLock;
    std::deque<Thread> g_threads;   // a deque: t_thread stays put
    thread_local Thread* t_thread = nullptr;

    struct Machine
    {
        std::chrono::steady_clock::time_point sampled;
        bool started = false;
        bool allPlay = false;      // every frame since `sampled` was of play
        bool loaded = false;
        platform::Load last, inPlay;
        uint64_t playNanoseconds = 0;
        // MW2_REPORT_SECONDS: what the second since the last reading held.
        uint64_t frames = 0, onTime = 0, spentAtSample[report::kTimeCount]{};
        report::Work workAtSample{};
        uint64_t lateAtSample[report::kLatenessCount]{}, lateCountAtSample[report::kLatenessCount]{};
        double longestMs = 0;
        uint64_t leastFree = UINT64_MAX, mostCommitted = 0;
        gpu::watch::Costs watch, watchInPlay;
    } m;

    // Under s.lock. `play` when the second since the last reading was all play.
    void Sample(bool play, std::chrono::steady_clock::duration since)
    {
        platform::Load load;
        const bool loaded = platform::ProcessorLoad(load);
        const platform::Memory memory = platform::MemoryState();
        {
            std::lock_guard lock(g_threadsLock);
            for (Thread& thread : g_threads)
            {
                if (thread.ended)
                {
                    if (play) thread.inPlay += thread.unsampled;
                    thread.unsampled = 0;
                    continue;
                }
                uint64_t now;
                if (!platform::ThreadTime(thread.clock, now)) { thread.ended = true; continue; }
                if (play && thread.read) thread.inPlay += now - thread.last;
                thread.last = now;
                uint64_t ready;
                if (platform::ThreadReadyTime(thread.id, ready))
                {
                    if (play && thread.read) thread.readyInPlay += ready - thread.lastReady;
                    thread.lastReady = ready;
                }
                thread.read = true;
            }
        }
        if (play && loaded && m.loaded)
        {
            m.inPlay.process += load.process - m.last.process;
            m.inPlay.busy += load.busy - m.last.busy;
            m.inPlay.total += load.total - m.last.total;
            m.playNanoseconds += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
        }
        if (loaded) { m.last = load; m.loaded = true; }
        const gpu::watch::Costs watch = gpu::watch::CostsSoFar();
        if (play)
        {
            m.watchInPlay.faults += watch.faults - m.watch.faults;
            m.watchInPlay.protects += watch.protects - m.watch.protects;
            m.watchInPlay.protectNanoseconds += watch.protectNanoseconds - m.watch.protectNanoseconds;
        }
        m.watch = watch;
        if (play && memory.usable) m.leastFree = std::min(m.leastFree, memory.free);
        m.mostCommitted = std::max(m.mostCommitted, memory.committed);
    }

    std::string Gigabytes(uint64_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / double(1ull << 30));
        return text;
    }
}

void report::ThreadStarted(const char* name)
{
    if (!On()) return;
    std::lock_guard lock(g_threadsLock);
    Thread& thread = g_threads.emplace_back();
    // "guest thread 12 (routine 8228B2C8)" is guest thread 12 here.
    thread.name = name;
    if (const size_t at = thread.name.find(" ("); at != std::string::npos) thread.name.resize(at);
    thread.clock = platform::ThreadClock();
    thread.id = platform::ThreadId();
    t_thread = &thread;
}

void report::ThreadEnded()
{
    if (!t_thread) return;
    // Read on the way out: the clock goes with the thread.
    uint64_t now;
    const bool read = platform::ThreadTime(t_thread->clock, now);
    std::lock_guard lock(g_threadsLock);
    if (read && t_thread->read) t_thread->unsampled = now - t_thread->last;
    t_thread->ended = true;
    t_thread = nullptr;
}

void report::ShaderBuilt(int fromCache)
{
    g_shaders[std::clamp(fromCache, -1, 1) + 1].fetch_add(1, std::memory_order_relaxed);
}

void report::PipelineBuilt(int fromCache)
{
    g_pipelines[std::clamp(fromCache, -1, 1) + 1].fetch_add(1, std::memory_order_relaxed);
}

void report::ShadersCompiled(std::string state)
{
    std::lock_guard lock(s.lock);
    g_compiled = std::move(state);
}

report::Timed::Timed(Time what) : time(what)
{
    if (!on) return;
    began = std::chrono::steady_clock::now();
    if (time == kExecuting) { g_executingSince = began; g_executing = true; }
}

report::Timed::~Timed()
{
    if (!on) return;
    if (time == kExecuting) g_executing = false;
    Spent(time, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - began).count()));
}

namespace
{
    report::Work g_work{};   // the renderer's thread's, like Frame
}

void report::Done(const Work& work) { g_work = work; }

void report::Frame(bool world)
{
    if (!On()) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(s.lock);
    if (!s.frames) s.first = now;
    s.frames++;
    uint64_t spent[kTimeCount];
    for (size_t i = 0; i < kTimeCount; i++) spent[i] = g_spent[i].load(std::memory_order_relaxed);
    if (g_executing)
        spent[kExecuting] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          now - g_executingSince).count());
    if (world)
    {
        s.worldFrames++;
        if (s.lastWasWorld)
        {
            for (size_t i = 0; i < kTimeCount; i++) s.spentInPlay[i] += spent[i] - s.spentAtLast[i];
            const double ms = std::chrono::duration<double, std::milli>(now - s.last).count();
            size_t bucket = 0;
            while (bucket < std::size(kBounds) && ms > kBounds[bucket]) bucket++;
            s.buckets[bucket]++;
            s.intervals++;
            s.intervalMs += ms;
            s.longestMs = std::max(s.longestMs, ms);
            m.frames++;
            m.longestMs = std::max(m.longestMs, ms);
            if (const double blankMs = frame_rate::DisplayPeriodNs() / 1e6; blankMs > 0)
            {
                s.againstDisplay++;
                if (ms <= 1.05 * blankMs) { s.withinABlank++; m.onTime++; }
                if (ms <= 2.05 * blankMs) s.withinTwoBlanks++;
            }
        }
    }
    std::copy(std::begin(spent), std::end(spent), s.spentAtLast);
    if (!(world && s.lastWasWorld)) m.allPlay = false;
    s.lastWasWorld = world;
    s.last = now;
    if (!m.started || now - m.sampled >= std::chrono::seconds(1))
    {
        const bool play = m.started && m.allPlay;
        Sample(play, now - m.sampled);
        // How long the second's frames took to the window and the screen.
        double lateMs[kLatenessCount]{};
        for (size_t of = 0; of < kLatenessCount; of++)
        {
            const uint64_t late = g_late[of].load(std::memory_order_relaxed);
            const uint64_t count = g_lateCount[of].load(std::memory_order_relaxed);
            if (count > m.lateCountAtSample[of])
                lateMs[of] = double(late - m.lateAtSample[of]) / 1e6 / double(count - m.lateCountAtSample[of]);
            if (play)
            {
                s.lateInPlay[of] += late - m.lateAtSample[of];
                s.lateCountInPlay[of] += count - m.lateCountAtSample[of];
            }
            m.lateAtSample[of] = late;
            m.lateCountAtSample[of] = count;
        }
        // A second of play in one line: where in a level the frame rate
        // falls, and whether the graphics chip or the renderer's thread is
        // what it waits for there.
        static const bool seconds = env::Flag("MW2_REPORT_SECONDS");
        if (seconds && play && m.frames)
        {
            auto took = [&](Time time) { return double(spent[time] - m.spentAtSample[time]) / 1e6 / double(m.frames); };
            auto each = [&](uint64_t now, uint64_t before) { return double(now - before) / double(m.frames); };
            char scene[48] = "";
            if (const uint32_t share = g_scene.load(std::memory_order_relaxed))
                std::snprintf(scene, sizeof scene, "; the scene at %u%% of its width", share);
            const Work& was = m.workAtSample;
            LOGI("report: second %.0f: %llu frames, %llu on time, the longest %.0f ms; a frame: the graphics chip"
                 " %.1f ms, the renderer's thread %.1f ms (%.1f waiting for the chip, %.1f reading textures,"
                 " %.1f compiling)%s; %.0f draws in %.0f segments, copied: %.0f KB of constants, %.0f KB of"
                 " vertex data, %.0f KB of indices; presented %.1f ms after it was finished, on screen %.1f ms",
                 std::chrono::duration<double>(now - s.first).count(), (unsigned long long)m.frames,
                 (unsigned long long)m.onTime, m.longestMs, took(kGpuBusy), took(kExecuting) - took(kDisplayWait),
                 took(kGpuWait), took(kTextures), took(kCompiling), scene,
                 each(g_work.draws, was.draws), each(g_work.segments, was.segments),
                 each(g_work.constantBytes, was.constantBytes) / 1024,
                 each(g_work.vertexBytes, was.vertexBytes) / 1024,
                 each(g_work.indexBytes, was.indexBytes) / 1024, lateMs[kToWindow], lateMs[kToScreen]);
        }
        m.frames = m.onTime = 0;
        m.longestMs = 0;
        std::copy(std::begin(spent), std::end(spent), m.spentAtSample);
        m.workAtSample = g_work;
        m.started = true;
        m.sampled = now;
        m.allPlay = true;
    }
}

void report::Spent(Time time, uint64_t nanoseconds)
{
    g_spent[time].fetch_add(nanoseconds, std::memory_order_relaxed);
}

uint64_t report::SpentSoFar(Time time) { return g_spent[time].load(std::memory_order_relaxed); }

namespace { std::atomic<bool> g_timesKept{ false }; }
void report::KeepTimes() { g_timesKept.store(true, std::memory_order_relaxed); }
bool report::TimesKept() { return g_timesKept.load(std::memory_order_relaxed); }

void report::Add(Cost cost, uint64_t microseconds)
{
    if (!On()) return;
    std::lock_guard lock(s.lock);
    s.costCount[cost]++;
    s.costMicroseconds[cost] += microseconds;
    s.costLongest[cost] = std::max(s.costLongest[cost], microseconds);
    Spent(kCompiling, microseconds * 1000);
}

void report::Write()
{
    if (!On()) return;
    std::lock_guard lock(s.lock);
    if (s.written) return;
    s.written = true;
    const double seconds = s.frames ? std::chrono::duration<double>(s.last - s.first).count() : 0.0;
    LOGI("report: %llu frames in %.0f s, %llu of them of the world", (unsigned long long)s.frames, seconds,
         (unsigned long long)s.worldFrames);
    if (s.intervals)
    {
        auto share = [](uint64_t count) { return 100.0 * double(count) / double(s.intervals); };
        LOGI("report: in play, %.1f frames a second on average; the longest frame took %.0f ms",
             1000.0 * double(s.intervals) / s.intervalMs, s.longestMs);
        LOGI("report: frame times: %.1f%% within %.1f ms, %.1f%% to %.0f ms, %.1f%% to %.0f ms, %.1f%% to %.0f ms,"
             " %.1f%% longer",
             share(s.buckets[0]), kBounds[0], share(s.buckets[1]), kBounds[1], share(s.buckets[2]), kBounds[2],
             share(s.buckets[3]), kBounds[3], share(s.buckets[4]));
        if (s.againstDisplay)
            LOGI("report: in play, %.1f%% of frames came within 1.05 display periods of the one before,"
                 " %.1f%% within 2.05; the period is %.2f ms (%.2f Hz)",
                 100.0 * double(s.withinABlank) / double(s.againstDisplay),
                 100.0 * double(s.withinTwoBlanks) / double(s.againstDisplay),
                 frame_rate::DisplayPeriodNs() / 1e6, 1e9 / frame_rate::DisplayPeriodNs());
        auto took = [](Time time) { return double(s.spentInPlay[time]) / 1e4 / s.intervalMs; };
        // The wait for the display is time in hand, and not work.
        LOGI("report: in play, the renderer's thread was at work %.0f%% of the time: %.0f%% waiting for the"
             " graphics chip, %.0f%% for its recorder thread, %.0f%% reading textures, %.0f%% compiling",
             took(kExecuting) - took(kDisplayWait), took(kGpuWait), took(kRecorderWait), took(kTextures),
             took(kCompiling));
        if (s.spentInPlay[kDisplayWait])
            LOGI("report: in play, the renderer's thread waited for the display %.0f%% of the time",
                 took(kDisplayWait));
        if (s.spentInPlay[kGpuBusy])
            LOGI("report: in play, the graphics chip was at work %.0f%% of the time, %.1f ms a frame",
                 took(kGpuBusy), double(s.spentInPlay[kGpuBusy]) / 1e6 / double(s.intervals));
        // The second is only known where the device times its presents.
        if (s.lateCountInPlay[kToWindow])
            LOGI("report: in play, a frame was presented %.1f ms after the renderer finished it, and was on"
                 " screen %.1f ms after (%llu frames timed)",
                 double(s.lateInPlay[kToWindow]) / 1e6 / double(s.lateCountInPlay[kToWindow]),
                 s.lateCountInPlay[kToScreen]
                     ? double(s.lateInPlay[kToScreen]) / 1e6 / double(s.lateCountInPlay[kToScreen]) : 0.0,
                 (unsigned long long)s.lateCountInPlay[kToScreen]);
    }
    if (m.playNanoseconds && m.inPlay.total)
    {
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        const double total = double(m.inPlay.total);
        const uint64_t others = m.inPlay.busy > m.inPlay.process ? m.inPlay.busy - m.inPlay.process : 0;
        LOGI("report: in play, the processor's %u threads were %.0f%% busy: %.0f%% with this game, %.0f%% with"
             " other programs",
             threads, 100.0 * double(m.inPlay.busy) / total, 100.0 * double(m.inPlay.process) / total,
             100.0 * double(others) / total);
        // The busiest of ours; the rest has the driver's and the system's threads in it.
        std::vector<std::pair<uint64_t, std::string>> busiest, readiest;
        {
            std::lock_guard lock(g_threadsLock);
            for (const Thread& thread : g_threads)
            {
                if (thread.inPlay) busiest.emplace_back(thread.inPlay, thread.name);
                if (thread.readyInPlay) readiest.emplace_back(thread.readyInPlay, thread.name);
            }
        }
        std::sort(busiest.begin(), busiest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const double wall = double(m.playNanoseconds) / 100.0;   // one thread's time, in %
        std::string list;
        uint64_t listed = 0;
        for (size_t i = 0; i < busiest.size() && i < 8 && double(busiest[i].first) / wall >= 1.0; i++)
        {
            char entry[96];
            std::snprintf(entry, sizeof entry, "%s%s %.0f", list.empty() ? "" : ", ", busiest[i].second.c_str(),
                          double(busiest[i].first) / wall);
            list += entry;
            listed += busiest[i].first;
        }
        if (!list.empty())
        {
            char rest[48];
            std::snprintf(rest, sizeof rest, ", the rest %.0f",
                          double(m.inPlay.process > listed ? m.inPlay.process - listed : 0) / wall);
            LOGI("report: in play, the game's threads in %% of one processor thread: %s%s", list.c_str(), rest);
        }
        std::sort(readiest.begin(), readiest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        list.clear();
        for (size_t i = 0; i < readiest.size() && i < 8 && double(readiest[i].first) / wall >= 1.0; i++)
        {
            char entry[96];
            std::snprintf(entry, sizeof entry, "%s%s %.0f", list.empty() ? "" : ", ", readiest[i].second.c_str(),
                          double(readiest[i].first) / wall);
            list += entry;
        }
        if (!list.empty())
            LOGI("report: in play, the game's threads ready to run and waiting for a processor thread, in %% of the"
                 " time: %s", list.c_str());
    }
    if (m.playNanoseconds && m.watchInPlay.protects)
    {
        // Write faults and protection changes are cheap on Linux; what they
        // cost elsewhere (Windows, more so with Memory Integrity on) is the question.
        const double seconds = double(m.playNanoseconds) / 1e9;
        LOGI("report: in play, the memory watch took %.0f write faults and %.0f protection changes a second;"
             " the changes took %.1f ms a second, %.1f us each",
             double(m.watchInPlay.faults) / seconds, double(m.watchInPlay.protects) / seconds,
             double(m.watchInPlay.protectNanoseconds) / 1e6 / seconds,
             double(m.watchInPlay.protectNanoseconds) / 1e3 / double(m.watchInPlay.protects));
    }
    {
        const platform::Memory memory = platform::MemoryState();
        std::string line = "report: memory: ";
        if (memory.installed > memory.usable)
            line += Gigabytes(memory.usable) + " usable of " + Gigabytes(memory.installed) + " (" +
                    Gigabytes(memory.installed - memory.usable) + " kept by the hardware)";
        else
            line += Gigabytes(memory.usable) + " usable";
        if (m.leastFree != UINT64_MAX) line += "; in play at least " + Gigabytes(m.leastFree) + " free";
        if (memory.commitLimit)
            line += "; commit charge up to " + Gigabytes(std::max(m.mostCommitted, memory.committed)) + " of " +
                    Gigabytes(memory.commitLimit);
        if (memory.processPeak) line += "; this game at most " + Gigabytes(memory.processPeak) + " in memory";
        if (memory.processCommitted) line += " and " + Gigabytes(memory.processCommitted) + " committed";
        LOGI("%s", line.c_str());
    }
    LOGI("report: compiled at a draw: %llu shaders in %llu ms (the longest %llu ms), %llu pipelines in %llu ms"
         " (the longest %llu ms)",
         (unsigned long long)s.costCount[kShader], (unsigned long long)(s.costMicroseconds[kShader] / 1000),
         (unsigned long long)(s.costLongest[kShader] / 1000), (unsigned long long)s.costCount[kPipeline],
         (unsigned long long)(s.costMicroseconds[kPipeline] / 1000),
         (unsigned long long)(s.costLongest[kPipeline] / 1000));
    // Every one should have come out of the launcher's cache.
    const uint64_t unknown = g_shaders[0], compiled = g_shaders[1], cached = g_shaders[2];
    LOGI("report: shaders compiled before play by the launcher: %s", g_compiled.c_str());
    if (unknown)
        LOGI("report: %llu shaders built in this run, %llu of them not in the launcher's (the driver says so for %llu)",
             (unsigned long long)(unknown + compiled + cached), (unsigned long long)compiled,
             (unsigned long long)(compiled + cached));
    else
        LOGI("report: %llu shaders built in this run, %llu of them not in the launcher's",
             (unsigned long long)(compiled + cached), (unsigned long long)compiled);
    // A pipeline compiled is the hitch a second run of the same level should
    // not have: its count, not the count built, is what a warm cache brings
    // to nothing.
    const uint64_t pipelines = g_pipelines[0] + g_pipelines[1] + g_pipelines[2];
    if (g_pipelines[0])
        LOGI("report: %llu pipelines built in this run; the driver does not say which it compiled",
             (unsigned long long)pipelines);
    else
        LOGI("report: %llu pipelines built in this run, %llu of them compiled by the driver and %llu out of the"
             " pipeline cache", (unsigned long long)pipelines, (unsigned long long)g_pipelines[1].load(),
             (unsigned long long)g_pipelines[2].load());
}

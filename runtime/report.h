#pragma once
// MW2_REPORT=1: what a bug report needs to say about a run, in every build.
//
// The launcher's REPORT A BUG starts the game with it. The log then names the
// graphics driver, and ends -- at exit or at a fault -- with how the run
// went: the frame rate, how the frame times spread, where the renderer's
// thread spent play, and what the renderer compiled in the middle of it. Off,
// every call here is one load of a flag.
#include "env.h"
#include <chrono>
#include <string>
#include <cstdint>

namespace report
{
    inline bool On()
    {
        static const bool on = env::Flag("MW2_REPORT");
        return on;
    }
    // The renderer finished a frame; `world` when it drew the world and not
    // only a menu or a loading screen.
    void Frame(bool world);
    // Work done at a draw that play had to wait for.
    enum Cost { kShader, kPipeline, kCostCount };
    void Add(Cost cost, uint64_t microseconds);
    // Where the ring consumer's time goes, which says what holds a slow run
    // back: the thread itself when it is busy all the time, the graphics chip
    // when the busy time is waiting for it, the title when it is neither.
    // kExecuting has the others in it. All are timed on the consumer's
    // thread, where report::Frame is called from, but kGpuBusy: the time the
    // graphics chip itself was at work, by its own clock (gpu_time.h).
    // kDisplayWait is the wait for the window to take a frame when the
    // display paces the title (frame_rate.h): time in hand, not work.
    enum Time { kExecuting, kRecorderWait, kGpuWait, kTextures, kCompiling, kGpuBusy, kDisplayWait, kTimeCount };
    void Spent(Time time, uint64_t nanoseconds);
    // A shader the renderer built, and whether the driver found it in the
    // pipeline cache the launcher compiled every shader of the game into:
    // 1 it did, 0 it compiled it, -1 it does not say. Any thread.
    void ShaderBuilt(int fromCache);
    // A pipeline the renderer built whole, or linked again optimised, and
    // whether it came out of the pipeline cache
    // (vk::pipeline::CreateGraphicsPipeline), as ShaderBuilt. Any thread.
    void PipelineBuilt(int fromCache);
    // A thread of ours, so the report can say what each one took of the
    // processor in play. Called on the thread itself (crash::RegisterThread).
    void ThreadStarted(const char* name);
    void ThreadEnded();
    // What the launcher's compile pass is to this run (shader_marker.h).
    void ShadersCompiled(std::string state);
    // The share of its width the scene is drawn at, in percent, where it is
    // drawn smaller under load (MW2_SCENE_DYNAMIC): MW2_REPORT_SECONDS says
    // it beside each second's times. The renderer's thread.
    void Scene(uint32_t percent);
    struct Timed
    {
        const Time time;
        const bool on = On();
        std::chrono::steady_clock::time_point began;
        explicit Timed(Time what);
        ~Timed();
    };
    void Write();
}

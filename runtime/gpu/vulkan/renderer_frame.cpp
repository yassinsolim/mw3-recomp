// Command buffers and their submission: the frame slots, the end of a frame,
// and the title's occlusion queries, which live and die with the render passes.
#include "renderer_state.h"
#include "capture.h"
#include "gpu_time.h"
#include "picture_probe.h"
#include "presenter.h"
#include "texture_cache.h"
#include "../../log.h"
#include "../../pacing_trace.h"
#include "../../report.h"

#include <atomic>

#include <cstdio>
#include <cstring>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    namespace
    {
        // MW2_FRAME_LOG=<file>: a line a frame of what the title asked for
        // and what was drawn, the counts as they stand at the frame's end,
        // for laying beside the frames a checker found a fault in
        // (tools/visualqa): the frame, the seconds since the start,
        // whether the world was drawn, the draws asked for and recorded,
        // the draws skipped for any reason, those skipped for a pipeline
        // still compiling and those of them that wrote no colour, the
        // pipelines the consumer built itself with the milliseconds they
        // took, and the times it stood still for a worker's, those that
        // came in time and the milliseconds stood.
        void LogFrame(bool world)
        {
            static std::FILE* const log = [] {
                const char* path = env::Text("MW2_FRAME_LOG");
                std::FILE* file = path ? std::fopen(path, "w") : nullptr;
                if (file)
                    std::fputs("frame,seconds,world,draws,recorded,skipped,awaiting,awaiting_no_colour,"
                               "built_at_draw,built_at_draw_ms,waits,waits_met,wait_ms\n", file);
                return file;
            }();
            if (!log) return;
            uint64_t skipped = 0;
            for (const auto& [why, count] : g.skipped) skipped += count;
            std::fprintf(log, "%llu,%.4f,%d,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%llu,%llu,%.1f\n",
                         (unsigned long long)g.frames, double(pacing::Microseconds()) / 1e6, int(world),
                         (unsigned long long)g.draws, (unsigned long long)g.drawsRecorded,
                         (unsigned long long)skipped, (unsigned long long)g.drawsAwaitingPipelines,
                         (unsigned long long)g.depthDrawsAwaitingPipelines,
                         (unsigned long long)g.pipelinesAtDraw, double(g.pipelineAtDrawMicroseconds) / 1e3,
                         (unsigned long long)g.pipelineWaits, (unsigned long long)g.pipelineWaitsMet,
                         double(g.pipelineWaitMicroseconds) / 1e3);
            // A run that is stopped from outside keeps what it wrote.
            if (!(g.frames & 63)) std::fflush(log);
        }
        // The decisions the recorder waits for at segment boundaries
        // (CloseSegment), a frame slot's own: the slot is not used again
        // until the recorder has made its last frame's calls.
        constexpr uint32_t kGates = 1024;
        enum : uint32_t { kPending, kJoin, kSplit };
        std::atomic<uint32_t> gates[kFrameSlots][kGates];
        bool recorderInPass = false;
        // The thread that queues the frame's commands, which alone settles a
        // boundary, and the boundary it has pending, for any other thread
        // that has to wait for the recorder (ReleaseGate).
        thread_local bool consumer = false;
        std::atomic<std::atomic<uint32_t>*> pending{ nullptr };

        // MW2_JOIN_SEGMENTS=0 gives every segment a command buffer and
        // rendering instance of its own, as before: what joining them saves,
        // measured on the device, which runs a release build.
        bool JoiningSegments()
        {
            static const bool on = !env::Text("MW2_JOIN_SEGMENTS") || env::Flag("MW2_JOIN_SEGMENTS");
            return on;
        }

        // Sets 1 and 2 -- every texture, and the arena -- are the same for
        // every draw, so they are bound once a command buffer (bindings.h).
        void BeginRecordedBuffer(VkCommandBuffer command, VkPipelineLayout layout,
                                 VkDescriptorSet textures, VkDescriptorSet memory,
                                 uint32_t dynamicOffsets)
        {
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
                LOGW("renderer: a frame's command buffer would not begin");
            const VkDescriptorSet sets[2] = { textures, memory };
            const uint32_t nowhere = 0;
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    bindings::kTextureSet, 2, sets, dynamicOffsets, &nowhere);
        }

        void EndRecordedPass(VkCommandBuffer command)
        {
            if (!recorderInPass) return;
            recorderInPass = false;
            if (vk::pipeline::LegacyMode())
                vkCmdEndRenderPass(command);
            else
                dispatch.endRendering(command);
            // What a render pass's dependency out to the outside was: the
            // copies, clears, passes and sampling after it see its writes.
            VkMemoryBarrier after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            after.dstAccessMask = kColourAccess | kDepthAccess | VK_ACCESS_TRANSFER_READ_BIT |
                                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command, kColourStages | kDepthStages,
                                 kColourStages | kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &after, 0, nullptr, 0, nullptr);
        }

        // The pending boundary, if there is one, settled: as a split, the
        // buffer so far is the submission's next and the segment's draws are
        // the start of the one after.
        void SettleGate(bool split)
        {
            if (!g.gate) return;
            pending.store(nullptr, std::memory_order_release);
            // Already a split, if a thread waiting for the recorder made it
            // one (ReleaseGate).
            uint32_t undecided = kPending;
            if (!g.gate->compare_exchange_strong(undecided, split ? kSplit : kJoin,
                                                 std::memory_order_acq_rel))
                split = true;
            g.gate->notify_one();
            g.gate = nullptr;
            if (split)
            {
                g.ended.push_back(g.command);
                g.command = g.gateNext;
                g.segment++;
                vk::record::Into(g.command);
            }
            else g.segmentsJoined++;
        }
    }

    bool& RecorderInPass() { return recorderInPass; }

    void ReleaseGate()
    {
        if (consumer) { SettleGate(true); return; }
        // Another thread -- the one that ends the run, once the consumer has
        // stopped -- has only the recorder to set going: the consumer's own
        // account of the split is made when it next settles (SettleGate).
        if (std::atomic<uint32_t>* gate = pending.load(std::memory_order_acquire))
        {
            uint32_t undecided = kPending;
            if (gate->compare_exchange_strong(undecided, kSplit, std::memory_order_acq_rel))
                gate->notify_one();
        }
    }

    // Destroying an image the GPU may still use. Waiting for the device is not
    // enough on its own: it covers work already submitted, and the command buffer
    // being recorded -- a render pass open on the very view about to go, or a
    // copy out of it -- has not been. Destroying under it leaves that buffer
    // pointing at freed memory, and submitting it has the GPU write through
    // addresses nothing backs: the page faults every run of this runtime logged
    // until one of them hung the driver and took the machine down. So the open
    // buffer is finished and waited for first, and a fresh one is begun if the
    // caller was in the middle of recording. Commands the recorder thread has
    // not made yet name it too, so it is waited for as well.
    void RetireBeforeDestroy()
    {
        const bool wasRecording = g.recording;
        if (wasRecording) Submit(true);
        vk::record::Drain();
        // The queue is in order, so nothing is outstanding afterwards.
        vkDeviceWaitIdle(g.device);
        for (State::Slot& slot : g.slots) slot.inFlight = false;
        if (wasRecording) BeginFrame();
    }

    bool BeginFrame()
    {
        if (g.recording) return true;

        // Round to the next slot and wait only if the GPU has not finished with
        // it. With three of them and a wait at the present, that is normally
        // already true and this costs nothing.
        g.slot = (g.slot + 1) % kFrameSlots;
        State::Slot& slot = g.slots[g.slot];
        if (!WaitForSlot(slot, "a frame slot's fence")) return false;
        NoteCompleted();
        vk::gpu_time::Collect();
        g.arenaBase = slot.arenaBase;
        vk::textures::BeginUploads(g.slot);
        g.ended.clear();
        g.segment = 0;
        g.gatesUsed = 0;
        consumer = true;
        vk::record::BeforeWaiting(ReleaseGate);
        g.command = slot.buffers[0];
        vk::record::Into(g.command);
        const VkDevice device = g.device;
        const VkCommandPool pool = slot.commands;
        Record([=](VkCommandBuffer) {
            vkResetCommandPool(device, pool, 0);
            // The slot's buffers are named afresh: none stands for another.
            vk::record::Redirect(VK_NULL_HANDLE, VK_NULL_HANDLE);
        });
        BeginSegmentBuffer();
        PassTimesBegin(g.slot);
        g.arenaUsed = 0;
        BeginKeptVertices();
        g.indexStream = g.tableStream = {};
        g.drawTableOpen = false;
        g.pixelWindow.valid = false;
        for (State::ConstantBlock& block : g.constantBlocks) block.valid = false;
        g.constantTable.valid = false;
        std::memset(g.constantTableWords, 0, sizeof g.constantTableWords);
        g.fetchBlock.valid = g.booleans.valid = g.loops.valid = false;
        g.recording = true;
        return true;
    }

    // Begins g.command. Sets 1 and 2 -- every texture, and the arena -- are
    // the same for every draw, so they are bound once a command buffer
    // instead of eight million times (bindings.h). Where the pixel constants
    // are a uniform block, set 2 has their place as a dynamic offset: none
    // yet, and a draw that reads them binds the set again at its own
    // (State::Bound::pixelConstants).
    void BeginSegmentBuffer()
    {
        const VkPipelineLayout layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());
        static_assert(bindings::kArenaSet == bindings::kTextureSet + 1);
        const VkDescriptorSet sets[2] = { static_cast<VkDescriptorSet>(vk::textures::Set()),
                                          g.slots[g.slot].memorySet };
        const uint32_t dynamicOffsets = vk::pipeline::UniformPixelConstants() ? 1 : 0;
        Record([=, textures = sets[0], memory = sets[1]](VkCommandBuffer command) {
            BeginRecordedBuffer(command, layout, textures, memory, dynamicOffsets);
        });
        // A fresh command buffer remembers nothing.
        g.bound = State::Bound{};
        g.currentColour = g.currentDepth = VK_NULL_HANDLE;
        g.passUnsure = false;
        g.segmentDraws = false;
    }

    // The command processor is about to report the draws so far done
    // (vk::renderer::BeforeCompletion): the segment's textures are read for
    // the last time, and what is read after this reaches only the draws
    // after it. A segment's uploads -- a texture first bound in it, one the
    // title rewrote under it -- run ahead of its draws and after the draws
    // of the segments before, so its draws have to begin a command buffer
    // when it has any.
    //
    // Whether it has any is known at its end, and one segment in twenty-five
    // does (MW2, S.S.D.D.). So the boundary before a segment is left to the
    // recorder, which waits there for the segment's end to say which it is
    // (the gate):
    //   - a join, when nothing was uploaded for the segment: its textures
    //     are what the draws before it sampled, and its draws go on in the
    //     same command buffer and the same rendering instance. Nothing
    //     stands between them: a copy, resolve, clear or change of target
    //     ends the instance itself wherever it comes (EndPass), boundary or
    //     not, and the consumer sets every piece of state again at the
    //     segment's first draw, as it does in a fresh buffer.
    //   - a split, when something was: the instance and the buffer end at
    //     the boundary, the uploads go between, and the segment's draws,
    //     queued for the old buffer, are redirected into the next
    //     (vk::record::Redirect), where its first draw opens the instance
    //     again (ResumeRendering).
    // Not left to the recorder, and so split as before: with no recorder
    // thread, without dynamic rendering, inside the title's occlusion
    // bracket (a query ends in the instance it began in), and with
    // MW2_JOIN_SEGMENTS=0. On the AYN Thor MW2's S.S.D.D. opening went from
    // 47 rendering instances a frame over its scene targets to 15.
    void CloseSegment()
    {
        const bool defer = JoiningSegments() && vk::record::Threaded() &&
                           !vk::pipeline::LegacyMode() && !g.guestCounting && g.gatesUsed < kGates;
        if (!defer)
        {
            EndPass();
            PassTimesBetween();
        }
        void* uploads = vk::textures::EndSegment();
        SettleGate(uploads != nullptr);
        if (uploads) g.ended.push_back(static_cast<VkCommandBuffer>(uploads));
        g.segments++;
        State::Slot& slot = g.slots[g.slot];
        if (g.segment + 1 >= slot.buffers.size())
        {
            // The recorder may be making calls into the pool's other buffers:
            // it has to be done with them before the pool is touched here.
            vk::record::Drain();
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool = slot.commands;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = kSegmentBuffers;
            const size_t had = slot.buffers.size();
            slot.buffers.resize(had + kSegmentBuffers);
            if (vkAllocateCommandBuffers(g.device, &allocate, slot.buffers.data() + had) != VK_SUCCESS)
                LOGE("renderer: no command buffers for another segment");
        }
        if (defer)
        {
            std::atomic<uint32_t>* const gate = &gates[g.slot][g.gatesUsed++];
            gate->store(kPending, std::memory_order_release);
            g.gate = gate;
            pending.store(gate, std::memory_order_release);
            g.gateNext = slot.buffers[g.segment + 1];
            const VkPipelineLayout layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());
            const VkDescriptorSet textures = static_cast<VkDescriptorSet>(vk::textures::Set());
            const VkDescriptorSet memory = slot.memorySet;
            const uint32_t dynamicOffsets = vk::pipeline::UniformPixelConstants() ? 1 : 0;
            Record([=, queued = g.command, next = g.gateNext](VkCommandBuffer command) {
                uint32_t decision;
                while ((decision = gate->load(std::memory_order_acquire)) == kPending)
                    gate->wait(kPending, std::memory_order_acquire);
                if (decision == kJoin) return;
                EndRecordedPass(command);
                if (vkEndCommandBuffer(command) != VK_SUCCESS)
                    LOGW("renderer: a segment's command buffer would not end");
                vk::record::Redirect(queued, next);
                BeginRecordedBuffer(next, layout, textures, memory, dynamicOffsets);
            });
            // What a fresh command buffer would not remember is set again
            // either way, and the instance asked for again at the next draw.
            g.bound = State::Bound{};
            g.passUnsure = g.currentColour || g.currentDepth;
            g.segmentDraws = false;
            return;
        }
        g.ended.push_back(g.command);
        Record([](VkCommandBuffer command) {
            if (vkEndCommandBuffer(command) != VK_SUCCESS)
                LOGW("renderer: a segment's command buffer would not end");
        });
        g.command = slot.buffers[++g.segment];
        vk::record::Into(g.command);
        BeginSegmentBuffer();
        PassTimesMark("texture uploads at a segment's end");
    }

    // The title brackets the draws it wants counted with two EVENT_WRITE_ZPD
    // events, which can be minutes of draws apart in command-buffer terms and
    // can cross any number of render passes. A Vulkan occlusion query has to
    // begin and end inside one render pass instance, so the bracket becomes one
    // query per pass it covers and the counts are added up.
    void OpenGuestQuery()
    {
        if (!g.guestCounting || g.guestQueryOpen) return;
        if (!g.guestQueries || g.guestQueryCount >= kMaxGuestQueries) return;
        const VkQueryPool pool = g.guestQueries;
        const uint32_t query = g.guestQueryFirst + g.guestQueryCount;
        const VkQueryControlFlags flags =
            vk::pipeline::PreciseOcclusionQueries() ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
        g.guestQueryScale[query % kGuestQueryRing] = uint8_t(g.currentScale);
        g.guestQueryShare[query % kGuestQueryRing] = g.currentShare;
        Record([=](VkCommandBuffer command) { vkCmdBeginQuery(command, pool, query, flags); });
        g.guestQueryOpen = true;
    }

    void CloseGuestQuery()
    {
        if (!g.guestQueryOpen) return;
        const VkQueryPool pool = g.guestQueries;
        const uint32_t query = g.guestQueryFirst + g.guestQueryCount++;
        Record([=](VkCommandBuffer command) { vkCmdEndQuery(command, pool, query); });
        g.guestQueryOpen = false;
    }

    void EndPass()
    {
        CloseGuestQuery();
        if (g.currentColour || g.currentDepth)
            Record([](VkCommandBuffer command) { EndRecordedPass(command); });
        PassTimesClose();
        g.currentColour = g.currentDepth = VK_NULL_HANDLE;
        g.passUnsure = false;
    }

    // `wait` when the CPU is about to read what the submission produced -- a
    // frame read back, an occlusion query's count -- or free what it uses.
    // Otherwise it is queued and the caller carries straight on, with up to
    // kFrameSlots submissions on the GPU at once. The window thread's copy of a
    // finished frame goes into the same queue after it, and the frame's last
    // barrier orders the two.
    //
    // The submission itself is the recorder thread's, after the commands before
    // it; the serial is the consumer's, counted here.
    bool Submit(bool wait)
    {
        Stopwatch watch(g.submitNanoseconds);
        if (!g.recording) return true;
        EndPass();
        PassTimesBetween();
        if (g.arenaUsed > g.arenaPeak) g.arenaPeak = g.arenaUsed;
        g.recording = false;

        State::Slot& slot = g.slots[g.slot];
        // The last segment's textures are read for the last time, and its
        // uploads go just ahead of its draws, after the segments before.
        void* uploads = vk::textures::EndSubmission();
        SettleGate(uploads != nullptr);
        std::vector<VkCommandBuffer> buffers;
        buffers.swap(g.ended);
        if (uploads) buffers.push_back(static_cast<VkCommandBuffer>(uploads));
        buffers.push_back(g.command);
        const VkDevice device = g.device;
        const VkQueue queue = g.queue;
        const VkFence fence = slot.fence;
        Record([=, buffers = std::move(buffers)](VkCommandBuffer command) {
            const bool ended = vkEndCommandBuffer(command) == VK_SUCCESS;
            if (!ended) LOGW("renderer: a frame's command buffer would not end");
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = uint32_t(buffers.size());
            submit.pCommandBuffers = buffers.data();
            std::lock_guard lock(vk::pipeline::QueueMutex());
            vkResetFences(device, 1, &fence);
            // Whatever happens the fence is signalled, or whoever waits for
            // this slot waits for ever.
            if (!ended || vk::pipeline::Failed(vk::gpu_time::Submit(queue, submit, fence,
                                                                    vk::gpu_time::Source::kFrame),
                                               "a frame's submission"))
            {
                VkSubmitInfo emptySubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                vkQueueSubmit(queue, 1, &emptySubmit, fence);
            }
            else
            {
                // Let the warmup thread know the GPU is being used for rendering
                // so it can throttle pipeline compilation (backpressure).
                NoteRenderSubmit();
            }
        });
        // The GPU gets it now, not when more calls have gathered behind it.
        vk::record::Flush();
        slot.inFlight = true;
        slot.serial = ++g.submissionSerial;
        slot.queuedAt = vk::record::Position();
        g.submits++;
        if (!wait) return true;

        if (!WaitForSlot(slot, "a frame slot's fence")) return false;
        // One queue, submitted in order: this one being done says all of them are.
        for (State::Slot& other : g.slots) other.inFlight = false;
        return true;
    }

    bool WaitForSlot(State::Slot& slot, const char* what)
    {
        if (!slot.inFlight) return true;
        Stopwatch waited(g.fenceNanoseconds);
        stutters::Timed timed(stutters::kGpuWaits);
        vk::record::WaitFor(slot.queuedAt);
        report::Timed spent(report::kGpuWait);
        // Waits for as long as it takes: the slot's command buffers are reused
        // once this returns, and that is undefined while they still execute.
        // The first two seconds are waited apart only so a long wait is logged.
        VkResult res = vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, 2000000000ull);
        if (res == VK_TIMEOUT)
        {
            LOGW("renderer: waiting for %s has taken over 2 seconds; still waiting", what);
            res = vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        }
        if (vk::pipeline::Failed(res, what))
            return false;
        slot.inFlight = false;
        return true;
    }

    // Asked once a submission rather than whenever it is wanted: a fence's
    // status is a system call, and the shadow wanted it for every draw that
    // touched a page the title keeps rewriting -- 7% of the consumer thread. A
    // value from a little earlier only makes the answer more cautious.
    //
    // Only the shadow's copies are read by the GPU, so only a page the shadow
    // served asks (memory_watch.cpp, Ready): without a shadow -- every Turnip,
    // whose storage buffers reach 128 MB -- nothing reads the answer, and the
    // call is not free: Turnip builds on Mesa 26.3 (PurpleVK T30) sleep in it
    // until the kernel's next tick, 2.9 ms a frame on the AYN Thor.
    void NoteCompleted()
    {
        if (!g.shadowBase) return;
        uint64_t oldestRunning = UINT64_MAX;
        for (State::Slot& slot : g.slots)
            if (slot.inFlight && slot.serial < oldestRunning &&
                (!vk::record::Done(slot.queuedAt) ||
                 vkGetFenceStatus(g.device, slot.fence) != VK_SUCCESS))
                oldestRunning = slot.serial;
        g.completedSerial = oldestRunning == UINT64_MAX ? g.submissionSerial : oldestRunning - 1;
    }

    uint64_t CompletedSerial() { return g.completedSerial; }

    // The frame's image is complete in `present`: submit it, move the frame
    // counters on, look at it if asked to, and hand it to the window.
    void FinishFrame(VkImage present)
    {
        // The frame is queued, not waited for: the window thread's blit goes into
        // the same queue behind it. Only a frame something reads back has to be
        // finished first.
        const FrameReadBacks plan = PlanReadBacks();
        if (plan.Any()) RecordReadBack(present);
        if (!Submit(plan.Any())) { Skip("frame submission failed"); return; }
        g.frames++;
        vk::textures::NewFrame();

        // The title does not redraw the world every time it presents -- better than
        // half the frames of the training level are the interface over whatever the
        // last world pass left in EDRAM. The world pass is the only one drawn
        // multisampled (2x, 4x on the pass beside it; the interface is 1x) in both
        // executables, so a handful of multisampled draws says it happened. 200
        // draws into any target is the fallback for a device drawing at 1x.
        constexpr uint64_t kWorldDraws = 200;
        constexpr uint64_t kMultisampledDraws = 8;
        bool worldDrawn = false;
        for (auto& [key, target] : g.targets)
        {
            if (!target.depth && (target.colourDrawsThisFrame >= kWorldDraws ||
                                  (key.samples > 0 &&
                                   target.colourDrawsThisFrame >= kMultisampledDraws)))
                worldDrawn = true;
            target.colourDrawsThisFrame = 0;
        }
        pacing::Note(pacing::kFrame, worldDrawn ? 1 : 0);
        stutters::Finished(g.frames, worldDrawn);
        // What the watch over a run's first frames tells a loading screen by (picture_probe.h).
        static uint64_t drawsBefore = 0;
        vk::probe::Drawn(g.draws >= drawsBefore ? g.draws - drawsBefore : g.draws);
        drawsBefore = g.draws;
        report::Done({ g.draws, g.segments, g.arenaWritten[kArenaConstants],
                       g.arenaWritten[kArenaVertices], g.arenaWritten[kArenaIndices] });
        report::Frame(worldDrawn);
        LogFrame(worldDrawn);
        NoteFrame(worldDrawn);
        SceneFrame(worldDrawn);
        adaptive::Frame(worldDrawn, SceneNow());
        SetCutsceneActive(!worldDrawn);
        if (worldDrawn)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!g.worldStreak++) g.streakFrom = now;
            if (g.worldFrames)
            {
                g.lastWorldAt = now;
                g.worldFrames++;
            }
            else if (g.worldStreak == State::kPlayStreak)
            {
                g.firstWorldAt = g.streakFrom;
                g.lastWorldAt = now;
                g.worldFrames = State::kPlayStreak;
            }
        }
        else g.worldStreak = 0;
        EndFrameDraws(g.frames, worldDrawn);
        // A capture ends and begins between submissions as the GPU gets them,
        // and the window must not copy the frame out before it is submitted:
        // both come after the submission, on the recorder thread.
        const uint64_t frame = g.frames;
        vk::record::Host([=] { vk::capture::FrameBoundary(worldDrawn, frame); });
        g.presentSerial[g.presentIndex] = frame;
        g.presentIndex = (g.presentIndex + 1) % kPresentImages;
        ExamineFrame(worldDrawn, plan);
        const uint32_t width = g.presentWidth, height = g.presentHeight;
        vk::record::Host([=] { vk::ShowImage(present, width, height, frame); });
    }
}

namespace vk::renderer::detail
{
    bool SubmissionFinished(uint32_t slotIndex, uint64_t serial)
    {
        // Still in the command buffer being recorded.
        if (serial > g.submissionSerial) return false;
        const State::Slot& slot = g.slots[slotIndex];
        if (slot.serial != serial || !slot.inFlight) return true;
        // Queued, and not yet handed to the GPU by the recorder thread.
        return vk::record::Done(slot.queuedAt) &&
               vkGetFenceStatus(g.device, slot.fence) == VK_SUCCESS;
    }
}

using namespace vk::renderer::detail;

namespace
{
    bool Finished(const State::PendingQuery& query)
    {
        return SubmissionFinished(query.slot, query.serial);
    }

    // Asked only of a submission that has finished (Finished, or a wait for
    // its slot), and never waited for: the counts are there by then, and a
    // query that still says otherwise never will, so it counts nothing.
    // Qualcomm's driver on an Adreno 740 leaves one or two a run that way,
    // while a first visit's pipelines are still being compiled: each had a
    // draw, its submission's fence had signalled, and it had no count 30 ms
    // later nor after the next submission. Waited for
    // (VK_QUERY_RESULT_WAIT_BIT), one such query held the consumer in the
    // driver for the rest of the run, with the title's back end awake
    // asking for a count nobody was left to hand over and its main thread
    // waiting for the back end: Call of Duty 4's The Bog on an AYN Thor,
    // still for good some 90 to 110 s into every first visit.
    void Read(State::PendingQuery& query)
    {
        stutters::Timed timed(stutters::kQueryReads);
        struct Counted { uint64_t samples, available; };
        Counted counts[kMaxGuestQueries]{};
        query.read = true;
        query.samples = 0;
        const VkResult result = vkGetQueryPoolResults(
            g.device, g.guestQueries, query.first, query.count, sizeof(Counted) * query.count, counts,
            sizeof(Counted), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) return;
        // The title compares the count with numbers of its own pixels, and
        // each query counted the pixels of its own pass's scale, or of the
        // part of the scene that pass drew.
        for (uint32_t i = 0; i < query.count; i++)
        {
            const uint32_t at = (query.first + i) % kGuestQueryRing;
            const uint64_t scale = g.guestQueryScale[at];
            if (!counts[i].available)
            {
                if (!g.guestQueriesUnanswered++)
                    LOGW("renderer: an occlusion query of a finished submission has no count;"
                         " it counts nothing, and so does every one like it (said once)");
                continue;
            }
            const uint64_t counted = counts[i].samples / (scale * scale);
            const float share = g.guestQueryShare[at];
            query.samples += share > 0.0f && share < 1.0f ? uint64_t(std::llround(double(counted) / double(share)))
                                                          : counted;
        }
    }
}

using namespace vk::renderer::detail;

// The title's own occlusion query. Everything drawn between the two calls is
// counted, however many render passes and command buffers that takes.
void vk::renderer::BeginOcclusionQuery()
{
    if (!g.guestQueries) return;
    // Any query still open belongs to a bracket the title abandoned.
    EndPass();
    if (!BeginFrame()) return;
    if (g.guestQueryNext + kMaxGuestQueries > kGuestQueryRing) g.guestQueryNext = 0;
    const uint32_t first = g.guestQueryNext, last = first + kMaxGuestQueries;
    // A bracket still out on this stretch of the ring is counted now, before
    // the reset below reaches the GPU.
    for (State::PendingQuery& query : g.pendingQueries)
        if (!query.read && query.first < last && first < query.first + query.count)
        {
            // Not submitted yet: this recording holds it, and has to go first.
            if (query.serial > g.submissionSerial)
            {
                EndPass();
                if (!Submit(true) || !BeginFrame()) return;
            }
            State::Slot& slot = g.slots[query.slot];
            if (slot.serial == query.serial) WaitForSlot(slot, "an occlusion query's submission");
            Read(query);
        }
    const VkQueryPool pool = g.guestQueries;
    Record([=](VkCommandBuffer command) {
        vkCmdResetQueryPool(command, pool, first, kMaxGuestQueries);
    });
    g.guestQueryFirst = first;
    g.guestQueryCount = 0;
    g.guestCounting = true;
}

// Not submitted here: the count goes out with the rest of the frame and is
// handed over once the GPU has finished it, and the title polls the record
// until then. Submitting at every bracket ended the command buffer mid-frame
// -- the textures re-read, every binding made again in the next one -- for 8%
// of the consumer thread.
bool vk::renderer::EndOcclusionQuery(uint32_t tag, uint64_t& samples)
{
    samples = 0;
    if (!g.guestQueries || !g.guestCounting) return true;
    EndPass();
    g.guestCounting = false;
    const uint32_t counted = g.guestQueryCount;
    g.guestQueryCount = 0;
    if (!counted) return true;
    g.guestQueryNext = g.guestQueryFirst + counted;
    // The submission that holds the bracket's last query: the one being
    // recorded, or the newest made when the bracket closes between two. Named
    // as the next one there, the count was read while its own was still on
    // the GPU.
    g.pendingQueries.push_back({ g.guestQueryFirst, counted, tag, g.slot,
                                 g.submissionSerial + (g.recording ? 1 : 0) });
    return false;
}

uint64_t vk::renderer::CompletedSubmission() { return CompletedSerial(); }

void vk::renderer::BeforeStreamWrite(uint32_t physical, uint32_t size)
{
    if (!g.device || !g.recording) return;
    if (!vk::textures::BoundInThisSegment(physical, size)) return;
    CloseSegment();
    g.streamWriteSegments++;
}

void vk::renderer::BeforeCompletion()
{
    g.completions++;
    if (!g.device || !g.recording || !g.segmentDraws) return;
    CloseSegment();
}

void vk::renderer::CollectOcclusionQueries(void (*deliver)(uint32_t tag, uint64_t samples))
{
    // In the order the title closed them; a later bracket is not handed over
    // before an earlier one.
    size_t done = 0;
    for (State::PendingQuery& query : g.pendingQueries)
    {
        if (!query.read && !Finished(query)) break;
        if (!query.read) Read(query);
        deliver(query.tag, query.samples);
        done++;
    }
    g.pendingQueries.erase(g.pendingQueries.begin(), g.pendingQueries.begin() + done);
}

#endif  // MW2_HAVE_VULKAN

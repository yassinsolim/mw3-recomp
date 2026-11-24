// Translated shaders, the pipelines built from them, and the shader cache that
// rebuilds a previous run's pipelines before the title asks for them.
#include "renderer_state.h"
#include "shader_build.h"
#include "../internal.h"
#include "../../diagnostics.h"
#include "../../log.h"
#include "../../platform.h"
#include "../../report.h"
#include "pipeline.h"

#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#ifdef MW2_ANDROID
#include <sys/resource.h>
#include "../../android/android.h"
#endif

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    std::mutex g_pipelinesMutex;
    namespace
    {
        std::mutex g_shadersMutex;
        // The run's list of pipelines (g.recorded) is added to by the
        // consumer and written out by the saver thread: a push, and the
        // saver's copy, hold this.
        std::mutex g_recordedMutex;
        // Set on the threads that build pipelines beside the consumer.
        thread_local bool t_warmupThread = false;
        // How many entries g.pipelines has been given, counted after each is
        // in the map. A draw waiting for a worker (PipelineWhenReady)
        // looks in the map again only when this has moved.
        std::atomic<uint64_t> g_pipelinesPublished{ 0 };
        // The pipelines the workers built because a draw was waiting for them.
        std::atomic<uint64_t> g_pipelinesBuiltForDraws{ 0 };
        // The pipelines a worker is building now (g_pipelinesMutex): the
        // same one can be in the warm-up's queue and asked for by a draw.
        std::unordered_set<PipelineKey, WordsHash> g_building;

        PipelineKey KeyOf(const Recorded& recorded)
        {
            PipelineKey key;
            key.vertexShader = recorded.vertexShader;
            key.pixelShader = recorded.pixelShader;
            key.colourFormat = recorded.colourFormat;
            key.depthFormat = recorded.depthFormat;
            key.topology = recorded.topology;
            key.blendControl = recorded.blendControl;
            key.colourMask = recorded.colourMask;
            key.samples = recorded.samples;
            if (vk::pipeline::LegacyMode())
            {
                key.depthControl = recorded.depthControl;
                key.modeCntl = recorded.modeCntl;
            }
            return key;
        }
    }

    // A shader the title has not drawn with before here: taken from the ones
    // compiled when it was loaded (TakePrepared), or translated and compiled
    // now -- a stall, which is why the load-time path exists.
    const Shader* ShaderFor(shader::Type type, uint64_t hash, const uint32_t* code, size_t words)
    {
        if (!code || !words) return nullptr;
        auto& last = g.lastShader[type == shader::Type::Pixel][hash % State::kRecentShaders];
        if (last.shader && last.hash == hash) return last.shader;
        {
            std::lock_guard lock(g_shadersMutex);
            auto found = g.shaders.find(hash);
            if (found != g.shaders.end())
            {
                if (!found->second.module) return nullptr;
                last = { hash, &found->second };
                return &found->second;
            }
        }

        static const bool traced = diag::Flag("MW2_TRACE_SHADER_LOADS");
        if (traced)
            LOGI("shader drawn: %s %016llX", type == shader::Type::Pixel ? "pixel" : "vertex", (unsigned long long)hash);
        Shader entry;
        if (!TakePrepared(hash, entry))
        {
            stutters::Timed timed(stutters::kShaders);
            const auto translateStart = std::chrono::steady_clock::now();
            entry = BuildShader(type, code, words);
            const uint64_t took = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - translateStart).count());
            g.translateMicroseconds += took;
            report::Add(report::kShader, took);
            g.shadersBuiltAtDraw++;
        }
        const bool ok = entry.module != VK_NULL_HANDLE;
        if (!ok)
            LOGW("renderer: %s shader %016llX not usable: %s",
                 type == shader::Type::Vertex ? "vertex" : "pixel",
                 (unsigned long long)hash,
                 entry.translation.ok ? "driver refused the module"
                                      : entry.translation.error.c_str());
        std::lock_guard lock(g_shadersMutex);
        auto& stored = g.shaders.emplace(hash, std::move(entry)).first->second;
        return ok ? &stored : nullptr;
    }

    VkCompareOp CompareOp(uint32_t function)
    {
        switch (function & 7)
        {
        case 0: return VK_COMPARE_OP_NEVER;
        case 1: return VK_COMPARE_OP_LESS;
        case 2: return VK_COMPARE_OP_EQUAL;
        case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4: return VK_COMPARE_OP_GREATER;
        case 5: return VK_COMPARE_OP_NOT_EQUAL;
        case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    // Xenos's stencil operations are Vulkan's, in the same order.
    VkStencilOp StencilOp(uint32_t op)
    {
        constexpr VkStencilOp kOps[8] = {
            VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
            VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
            VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP,
        };
        return kOps[op & 7];
    }

    VkBlendFactor BlendFactor(uint32_t factor)
    {
        switch (factor)
        {
        case 0:  return VK_BLEND_FACTOR_ZERO;
        case 1:  return VK_BLEND_FACTOR_ONE;
        case 4:  return VK_BLEND_FACTOR_SRC_COLOR;
        case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6:  return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8:  return VK_BLEND_FACTOR_DST_COLOR;
        case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp BlendOp(uint32_t op)
    {
        switch (op & 7)
        {
        case 1:  return VK_BLEND_OP_SUBTRACT;
        case 2:  return VK_BLEND_OP_MIN;
        case 3:  return VK_BLEND_OP_MAX;
        case 4:  return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
        }
    }

    VkPrimitiveTopology TopologyFor(uint32_t primitive)
    {
        switch (primitive)
        {
        case 1:  return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 2:  return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 3:  return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 4:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 5:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 6:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        // A rectangle list is expanded into two triangles a rectangle before the
        // draw, so by here it is an ordinary triangle list.
        case 8:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 12: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;      // line loop
        default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        }
    }

    DepthTests DepthTestsFor(uint32_t control)
    {
        const gpu::DepthControl depthControl{ control };
        DepthTests tests;
        tests.depthTest = depthControl.ZEnable();
        tests.depthWrite = depthControl.ZWrite();
        tests.depthCompare = CompareOp(depthControl.ZFunc());
        // The reference and the masks are set on their own. Back faces share
        // the front's operations unless the title gives them their own.
        tests.stencilTest = depthControl.StencilEnable();
        tests.front = { StencilOp(depthControl.StencilFail()), StencilOp(depthControl.StencilZPass()),
                        StencilOp(depthControl.StencilZFail()), CompareOp(depthControl.StencilFunc()),
                        0, 0, 0 };
        tests.back = tests.front;
        if (depthControl.BackfaceEnable())
            tests.back = { StencilOp(depthControl.StencilFailBack()),
                           StencilOp(depthControl.StencilZPassBack()),
                           StencilOp(depthControl.StencilZFailBack()),
                           CompareOp(depthControl.StencilFuncBack()), 0, 0, 0 };
        return tests;
    }

    void FacesFor(uint32_t modeCntl, VkCullModeFlags& cull, VkFrontFace& front)
    {
        const gpu::ModeCntl mode{ modeCntl };
        // A whole class of geometry missing and nothing else wrong is what a
        // winding read backwards looks like, so it is worth being able to take
        // the culling away in one run.
        static const bool noCull = diag::Flag("MW2_NO_CULL");
        cull = noCull ? 0u
                      : ((mode.CullFront() ? VK_CULL_MODE_FRONT_BIT : 0u) |
                         (mode.CullBack() ? VK_CULL_MODE_BACK_BIT : 0u));
        front = mode.FrontIsClockwise() ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    PipelineState::PipelineState(const PipelineKey& key)
    {
        // Xenos has no fixed-function vertex input at all: the shader fetches its
        // own attributes out of set 2.
        assembly.topology = VkPrimitiveTopology(key.topology);
        static constexpr VkViewport kDummyViewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        static constexpr VkRect2D kDummyScissor{ {0, 0}, {1, 1} };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        viewport.pViewports = &kDummyViewport;
        viewport.pScissors = &kDummyScissor;

        raster = vk::shaders::Rasterisation();
        // A bias without a depth attachment means nothing, and a render pass
        // without one is not to be given it.
        raster.depthBiasEnable = (key.depthFormat != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE;

        multisample.rasterizationSamples = VkSampleCountFlagBits(std::max(key.samples, 1u));
        // The tests themselves are set per draw (DepthTestsFor); `depth` stays empty.

        if (key.colourFormat != VK_FORMAT_UNDEFINED)
        {
            const gpu::BlendControl blend{ key.blendControl };
            attachment.blendEnable = !blend.IsPassThrough();
            attachment.srcColorBlendFactor = BlendFactor(blend.ColorSource());
            attachment.dstColorBlendFactor = BlendFactor(blend.ColorDestination());
            attachment.colorBlendOp = BlendOp(blend.ColorOp());
            attachment.srcAlphaBlendFactor = BlendFactor(blend.AlphaSource());
            attachment.dstAlphaBlendFactor = BlendFactor(blend.AlphaDestination());
            attachment.alphaBlendOp = BlendOp(blend.AlphaOp());
            // Four bits per target, ARGB on the console and RGBA here.
            const uint32_t mask = key.colourMask & 0xF;
            attachment.colorWriteMask =
                ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0u);
            blending.attachmentCount = 1;
            blending.pAttachments = &attachment;
        }
        else
        {
            blending.attachmentCount = 0;
            blending.pAttachments = nullptr;
        }

        // Without extended dynamic state, culling, winding and the depth and
        // stencil tests are part of the pipeline (and of its key).
        static constexpr VkDynamicState kDynamicLegacy[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE };

        if (vk::pipeline::LegacyMode())
        {
            dynamic.dynamicStateCount = uint32_t(std::size(kDynamicLegacy));
            dynamic.pDynamicStates = kDynamicLegacy;

            FacesFor(key.modeCntl, raster.cullMode, raster.frontFace);

            if (key.depthFormat != VK_FORMAT_UNDEFINED)
            {
                const DepthTests tests = DepthTestsFor(key.depthControl);
                depth.depthTestEnable = tests.depthTest ? VK_TRUE : VK_FALSE;
                depth.depthWriteEnable = tests.depthWrite ? VK_TRUE : VK_FALSE;
                depth.depthCompareOp = tests.depthCompare;
                depth.stencilTestEnable = tests.stencilTest ? VK_TRUE : VK_FALSE;
                depth.front.failOp = tests.front.failOp;
                depth.front.passOp = tests.front.passOp;
                depth.front.depthFailOp = tests.front.depthFailOp;
                depth.front.compareOp = tests.front.compareOp;
                depth.back.failOp = tests.back.failOp;
                depth.back.passOp = tests.back.passOp;
                depth.back.depthFailOp = tests.back.depthFailOp;
                depth.back.compareOp = tests.back.compareOp;
            }
        }
        else
        {
            dynamic.dynamicStateCount = vk::shaders::kDynamicStateCount;
            dynamic.pDynamicStates = vk::shaders::kDynamicStates;

            // Dynamic rendering: the formats of the targets stand where a render
            // pass would.
            colourFormat = VkFormat(key.colourFormat);
            rendering.colorAttachmentCount = (key.colourFormat != VK_FORMAT_UNDEFINED) ? 1 : 0;
            rendering.pColorAttachmentFormats = (key.colourFormat != VK_FORMAT_UNDEFINED) ? &colourFormat : nullptr;
            rendering.depthAttachmentFormat = VkFormat(key.depthFormat);
            rendering.stencilAttachmentFormat = VkFormat(key.depthFormat);
        }
    }

    // The whole pipeline in one call, both shaders compiled for it: what a
    // device without pipeline libraries gets.
    VkPipeline BuildPipeline(const PipelineKey& key, const Shader& vertex, const Shader& pixel)
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex.module;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pixel.module;

        const PipelineState state(key);
        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = vk::pipeline::LegacyMode() ? nullptr : &state.rendering;

        gpu::detail::SetCurrentDrawStage("Draw_EnsurePipeline_GetRenderPass");
        VkRenderPass renderPass = vk::pipeline::LegacyMode()
            ? GetRenderPass(VkFormat(key.colourFormat), VkFormat(key.depthFormat),
                            VkSampleCountFlagBits(std::max(key.samples, 1u)))
            : VK_NULL_HANDLE;

        if (vk::pipeline::LegacyMode() && !renderPass)
        {
            LOGE("renderer: legacy mode has no renderPass for pipeline %016llX/%016llX (c=%d, d=%d, s=%u)",
                 (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
                 key.colourFormat, key.depthFormat, key.samples);
            return VK_NULL_HANDLE;
        }

        info.renderPass = renderPass;
        info.subpass = 0;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &state.input;
        info.pInputAssemblyState = &state.assembly;
        info.pViewportState = &state.viewport;
        info.pRasterizationState = &state.raster;
        info.pMultisampleState = &state.multisample;
        info.pDepthStencilState = (key.depthFormat != VK_FORMAT_UNDEFINED) ? &state.depth : nullptr;
        info.pColorBlendState = (key.colourFormat != VK_FORMAT_UNDEFINED) ? &state.blending : nullptr;
        info.pDynamicState = &state.dynamic;
        info.layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());

        info.flags |= vk::pipeline::CaptureFlags();

        gpu::detail::SetCurrentDrawStage("Draw_EnsurePipeline_vkCreateGraphicsPipelines");
        const auto compileStart = std::chrono::steady_clock::now();
        LOGI("renderer: building pipeline %016llX/%016llX (c=%d, d=%d, topo=%u, s=%u, blend=%08X, mask=%X, depth=%08X, mode=%08X, pass=%p)...",
             (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
             key.colourFormat, key.depthFormat, key.topology, key.samples,
             key.blendControl, key.colourMask, key.depthControl, key.modeCntl,
             (void*)renderPass);

        VkPipeline built = VK_NULL_HANDLE;
        int fromCache = -1;
        const VkResult res = VkResult(vk::pipeline::CreateGraphicsPipeline(&info, &built, &fromCache));
        const uint64_t tookMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - compileStart).count());
        gpu::detail::SetCurrentDrawStage("idle");
        if (res != VK_SUCCESS)
        {
            LOGE("renderer: vkCreateGraphicsPipelines failed: %d (colour=%d, depth=%d, topo=%u, samples=%u) in %llu ms",
                 res, key.colourFormat, key.depthFormat, key.topology, key.samples, (unsigned long long)tookMs);
            return VK_NULL_HANDLE;
        }
        vk::pipeline::LogShaderStats((void*)built, key.vertexShader, key.pixelShader);
        report::PipelineBuilt(fromCache);
        LOGI("renderer: built pipeline %016llX/%016llX in %llu ms%s (handle=%p)",
             (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
             (unsigned long long)tookMs, fromCache == 1 ? ", out of the pipeline cache" : fromCache == 0 ? ", compiled" : "",
             (void*)built);
        return built;
    }

    VkPipeline EnsurePipeline(const PipelineKey& key, const Shader& vertex, const Shader& pixel,
                              bool* made)
    {
        TakeOptimisedPipelines();
        // The last answer is the consumer's alone: the warm-up worker neither
        // reads nor replaces it.
        if (!t_warmupThread && g.lastPipeline && g.lastPipelineSwaps == g.pipelinesOptimised &&
            key == g.lastPipelineKey)
            return g.lastPipeline;
        {
            std::lock_guard lock(g_pipelinesMutex);
            auto found = g.pipelines.find(key);
            if (found != g.pipelines.end())
            {
                if (!t_warmupThread)
                {
                    g.lastPipelineKey = key;
                    g.lastPipeline = found->second;
                    g.lastPipelineSwaps = g.pipelinesOptimised;
                }
                return found->second;
            }
        }
        if (made) *made = true;
        LOGI("renderer: pipeline miss for %016llX/%016llX (c=%d, d=%d, topo=%u, blend=%08X, mask=%X, depth=%08X, mode=%08X)",
             (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
             key.colourFormat, key.depthFormat, key.topology,
             key.blendControl, key.colourMask, key.depthControl, key.modeCntl);

        const auto compileStart = std::chrono::steady_clock::now();
        // A stutter's cost only on the consumer: the worker's compiling holds
        // no frame up.
        std::optional<stutters::Timed> timed;
        if (!t_warmupThread) timed.emplace(stutters::kPipelines);
        // Linked from the two shaders' libraries, compiled when the title
        // loaded them, in well under a millisecond; or the whole pipeline
        // compiled here, on a device that cannot.
        VkPipeline built = vertex.library && pixel.library ? LinkPipeline(key, vertex, pixel)
                                                           : BuildPipeline(key, vertex, pixel);
        const uint64_t took = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - compileStart).count());
        g.compileMicroseconds += took;
        report::Add(report::kPipeline, took);
        if (!built)
            LOGW("renderer: pipeline for %016llX/%016llX rejected",
                 (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader);

        {
            std::lock_guard lock(g_pipelinesMutex);
            g.pipelines[key] = built;   // a failure is remembered, so it fails once
        }
        // After the map has it, and the handle is whole before either: a
        // draw that sees the count moved finds a pipeline it can record.
        g_pipelinesPublished.fetch_add(1, std::memory_order_release);
        if (built) NoteUnsaved();
        return built;
    }

    namespace warmup
    {
        struct QueueItem
        {
            Recorded recorded;
        };

        std::mutex g_warmupMutex;
        std::condition_variable g_warmupCv;
        // A previous run's pipelines, built when there is time for them.
        std::deque<QueueItem> g_warmupQueue;
        // The pipelines draws are being skipped for (PipelineWhenReady), in
        // the order they were first asked for.
        std::deque<QueueItem> g_demandQueue;
        // One thread for the warm-up and CompileThreads for the draws. A
        // thread that has lowered its priority cannot be given it back
        // (platform.h), and a draw is lost every frame its pipeline is not
        // there: the demand queue's threads keep the priority they were made
        // with, on the performance cores.
        std::thread g_warmupThread;
        std::vector<std::thread> g_demandThreads;
        std::atomic<bool> g_warmupRunning{ false };
        std::atomic<bool> g_warmupStopping{ false };
        std::atomic<bool> g_cutsceneActive{ false };

        std::atomic<uint32_t> g_statQueued{ 0 };
        std::atomic<uint32_t> g_statCompleted{ 0 };
        std::atomic<uint32_t> g_statMisses{ 0 };
        std::atomic<uint32_t> g_statLastCompileMs{ 0 };

        // Millisecond timestamp (steady_clock epoch) of the last render-thread
        // vkQueueSubmit. The warmup worker reads this to back off when the GPU
        // is busy serving render frames -- preventing the queue-saturation stalls
        // that trigger the device's ANR watchdog (Oppo Athena).
        std::atomic<int64_t> g_lastRenderSubmitMs{ 0 };

        // Records the render thread's submission time. Called from renderer_frame.cpp.
        void NoteRenderSubmit()
        {
            using namespace std::chrono;
            const int64_t ms = duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count();
            g_lastRenderSubmitMs.store(ms, std::memory_order_relaxed);
        }

        // True if the render thread has submitted a frame recently enough that the
        // GPU is likely still busy with rendering work.
        bool RenderActive()
        {
            using namespace std::chrono;
            const int64_t nowMs = duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count();
            const int64_t lastMs = g_lastRenderSubmitMs.load(std::memory_order_relaxed);
            // 150 ms: about 9 frames at 60 FPS. If we haven't heard from the
            // render thread in that window, assume it is idling (cutscene load).
            return (nowMs - lastMs) < 150;
        }

        // How many threads compile the pipelines draws are waiting for. The
        // consumer compiles none of them (PipelineWhenReady), so how long a
        // first visit's surfaces stay undrawn is how fast these get through
        // them: one thread took a first Seatown match's 440 pipelines at 25
        // to 35 ms each, eleven seconds and more of them. Where threads are
        // held to cores (a phone), all but one of the performance and efficiency
        // cores, six at most; elsewhere a quarter of the processor's
        // threads, three at most; one where the driver takes one creation at
        // a time anyway. MW2_COMPILE_THREADS=<n> says otherwise.
        uint32_t CompileThreads()
        {
            static const uint32_t threads = [] {
                if (const uint64_t asked = env::Number("MW2_COMPILE_THREADS"))
                    return uint32_t(std::min<uint64_t>(asked, 8));
                if (vk::pipeline::OneCreation()) return 1u;
                if (const uint32_t cores = platform::CoreCount(platform::ThreadAffinityRole::SpareCore))
                    return std::clamp(cores - 1, 1u, 6u);
                return std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
            }();
            return threads;
        }

        // A creation the driver answers out of its cache takes well under
        // this; one it compiles takes ten times it and more.
        constexpr uint32_t kCompiledMs = 3;

        // The warm-up's run since its queue was last empty: how many
        // pipelines, how many of them the driver compiled, how many are
        // being built this moment, and from when. Under g_warmupMutex.
        uint32_t g_run = 0, g_runCompiled = 0, g_runBuilding = 0;
        std::chrono::steady_clock::time_point g_runStart{};

        void Worker(bool demanded)
        {
            t_warmupThread = true;
            // The warm-up on the little cores, out of the title's way, but at
            // the priority it was made with: it takes the locks the consumer
            // takes at every change of shader or pipeline, and held back with
            // one -- the little cores are busy too, and a lowered thread is
            // the last they run -- it held the consumer for as long. A ring
            // batch of a hundred dwords took up to three seconds that way.
            //
            // The pipelines draws wait for on the performance and the
            // efficiency cores alike. Held to the performance cores, which
            // the title's own threads are held to, two of them compiling a
            // first visit's pipelines left the title a core short where a
            // level cuts to a new view: Modern Warfare 3's Mind the Gap on a
            // Retroid Pocket Flip 2 (three performance cores) ran at 41 to 45
            // frames a second for five seconds there, 66 frames late, and
            // with six threads on all seven cores at 58 to 60, 9 to 12 late,
            // its surfaces 3.6 s in coming for 3.0 (docs/android.md).
            platform::SetThreadAffinity(demanded ? platform::ThreadAffinityRole::SpareCore
                                                 : platform::ThreadAffinityRole::WorkerCore);
            LOGI("warmup: %s worker thread started", demanded ? "demand" : "scheduler");
            // Whether the pipeline in hand came off the warm-up's queue.
            bool warming = false;
            // While the world is not drawn -- a menu, a loading screen -- the
            // draws' threads have nothing to do, and take the warm-up's queue
            // with it: a recorded list the driver's cache has nothing of (a
            // new driver chosen, a cache thrown away, a driver that keeps
            // none) was one thread's work on a little core, and a match
            // started before it was through drew its first seconds with the
            // surfaces coming in one by one, though every pipeline of them
            // was known.
            const auto helps = [demanded] {
                return demanded && g_cutsceneActive.load(std::memory_order_relaxed) && !g_warmupQueue.empty();
            };
            while (!g_warmupStopping.load(std::memory_order_relaxed))
            {
                QueueItem item{};
                {
                    std::unique_lock lock(g_warmupMutex);
                    if (warming) g_runBuilding--;
                    if (g_warmupQueue.empty() && g_run && !g_runBuilding)
                    {
                        LOGI("warmup: %u recorded pipelines ready in %lld ms, %u of them compiled by the driver",
                             g_run, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - g_runStart).count(), g_runCompiled);
                        g_run = g_runCompiled = 0;
                    }
                    g_warmupCv.wait(lock, [&] {
                        return g_warmupStopping.load(std::memory_order_relaxed) ||
                               !(demanded ? g_demandQueue : g_warmupQueue).empty() || helps();
                    });

                    if (g_warmupStopping.load(std::memory_order_relaxed))
                        break;

                    // A draw's pipeline before any nothing is waiting for.
                    warming = !demanded || g_demandQueue.empty();
                    std::deque<QueueItem>& queue = warming ? g_warmupQueue : g_demandQueue;
                    item = queue.front();
                    queue.pop_front();
                    if (warming)
                    {
                        if (!g_run && !g_runBuilding) g_runStart = std::chrono::steady_clock::now();
                        g_runBuilding++;
                    }
                }

                const PipelineKey key = KeyOf(item.recorded);

                // Built already, or being built by another thread, whose
                // publishing it is what a draw waiting for it sees.
                {
                    std::lock_guard lock(g_pipelinesMutex);
                    if (g.pipelines.contains(key) || !g_building.insert(key).second)
                        continue;
                }

                // The shaders where they are, not copies made under the
                // lock: none leaves g.shaders before Shutdown, which stops
                // these threads first, and a copy was the microcode and the
                // translation of both with the consumer waiting.
                const Shader* vertex = nullptr;
                const Shader* pixel = nullptr;
                {
                    std::lock_guard lock(g_shadersMutex);
                    auto vIt = g.shaders.find(key.vertexShader);
                    auto pIt = g.shaders.find(key.pixelShader);
                    if (vIt != g.shaders.end() && vIt->second.module &&
                        pIt != g.shaders.end() && pIt->second.module)
                    {
                        vertex = &vIt->second;
                        pixel = &pIt->second;
                    }
                }

                const auto start = std::chrono::steady_clock::now();
                bool made = false;
                const VkPipeline built = vertex ? EnsurePipeline(key, *vertex, *pixel, &made) : VK_NULL_HANDLE;
                const uint32_t tookMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
                {
                    std::lock_guard lock(g_pipelinesMutex);
                    g_building.erase(key);
                }
                if (!built || !made) continue;
                g_statCompleted.fetch_add(1, std::memory_order_relaxed);
                g_statLastCompileMs.store(tookMs, std::memory_order_relaxed);

                // No pause after a pipeline a draw is waiting for: its
                // surface is undrawn every frame until it is there. The
                // pauses below pace the ones nothing is waiting for.
                if (!warming)
                {
                    g_pipelinesBuiltForDraws.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Nor after one the driver had in its cache, which is every
                // one of a level played before: it cost a millisecond and
                // nothing of the GPU's. Paced like a compile, 200 ms apart in
                // play, a multiplayer's 1500 recorded pipelines took five
                // minutes to be there, and the draws that came first each
                // waited for theirs.
                {
                    std::lock_guard lock(g_warmupMutex);
                    g_run++;
                    if (tookMs >= kCompiledMs) g_runCompiled++;
                }
                // Nor on the draws' threads, which are here only while no
                // world is drawn and leave the moment one is.
                if (demanded || tookMs < kCompiledMs) continue;

                // Backpressure: when the render thread is actively submitting
                // frames the GPU command queue is shared and can saturate,
                // causing vkQueueSubmit on the render thread to block for 100-
                // 700 ms -- which trips the OS ANR watchdog on this device.
                //
                // Strategy:
                //  - Cutscene / loading (no render submissions): compile fast,
                //    sleep only 10 ms between pipelines.
                //  - Active gameplay (render submitting within last 150 ms):
                //    sleep at least 200 ms regardless of compile time, to give
                //    the GPU time to drain its render work between compilations.
                //    If the last compilation was especially heavy (>150 ms on
                //    the CPU side -- a sign the GPU was busy too), add an extra
                //    cooldown proportional to that time.
                //  - Default (render not recently active but not in cutscene):
                //    sleep 35 ms.
                int sleepMs;
                if (warmup::g_cutsceneActive.load(std::memory_order_relaxed))
                {
                    sleepMs = 10;
                }
                else if (RenderActive())
                {
                    // Base 200 ms + half the compile time so a 500 ms compile
                    // gives 450 ms of recovery rather than hammering immediately.
                    sleepMs = 200 + int(tookMs / 2);
                }
                else
                {
#ifdef MW2_ANDROID
                    // On mobile SoCs, allow 120 ms thermal dissipation between cold compilations
                    // to prevent aggressive OS thermal throttling (e.g. MediaTek/Realme horae at 44C+).
                    sleepMs = 120;
#else
                    sleepMs = 35;
#endif
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
            LOGI("warmup: %s worker thread exiting", demanded ? "demand" : "scheduler");
        }
    }

    // The recorded pipelines and the driver's cache, written from a thread
    // of their own while something in them is new. They were written by
    // whoever made the twenty-fifth new pipeline -- the consumer, in the
    // middle of a frame, a megabyte of shaders and several of cache to the
    // phone's storage -- and otherwise only at a clean end, which a game
    // swiped away or killed by the system for its memory never reaches: now
    // a run that ends any way has lost kSaveEvery of what it compiled at most.
    namespace saver
    {
        constexpr auto kSaveEvery = std::chrono::seconds(10);
        std::mutex g_lock;
        std::condition_variable g_wake;
        std::thread g_thread;
        bool g_stopping = false;
        std::atomic<bool> g_started{ false };
        std::atomic<bool> g_unsaved{ false };
        // One writer of the two files at a time: the thread, or whoever
        // says the game is going away (SaveCaches).
        std::mutex g_writing;

        // `whole`: the list is written whether or not it has changed (the
        // run's end). The driver's cache is asked every time: it knows
        // itself whether it has grown, and what was found in its closed
        // files is counted there whatever was built.
        void Save(bool whole = false)
        {
            std::lock_guard writing(g_writing);
            if (!g.ready && !whole) return;
            if (g_unsaved.exchange(false, std::memory_order_acq_rel) || whole) WriteShaderCache();
            vk::pipeline::SaveCache();
        }

        void Work()
        {
            // At the priority it was made with: it holds the shaders' and
            // the list's locks while it copies them, and a thread held back
            // with one of those is the consumer held back.
            platform::SetThreadName("cache saver");
            std::unique_lock lock(g_lock);
            while (!g_wake.wait_for(lock, kSaveEvery, [] { return g_stopping; }))
            {
                lock.unlock();
                Save();
                lock.lock();
            }
        }

        void Stop()
        {
            {
                std::lock_guard lock(g_lock);
                g_stopping = true;
            }
            g_wake.notify_all();
            if (g_thread.joinable()) g_thread.join();
            std::lock_guard lock(g_lock);
            g_stopping = false;
            g_started.store(false, std::memory_order_release);
        }
    }

    void NoteUnsaved()
    {
        saver::g_unsaved.store(true, std::memory_order_release);
        if (saver::g_started.load(std::memory_order_acquire)) return;
        std::lock_guard lock(saver::g_lock);
        if (saver::g_started.exchange(true, std::memory_order_acq_rel)) return;
        saver::g_thread = std::thread(saver::Work);
    }

    void RecordPipeline(const Recorded& recorded)
    {
        {
            std::lock_guard lock(g_recordedMutex);
            g.recorded.push_back(recorded);
        }
        NoteUnsaved();
    }

    void StartWarmupScheduler()
    {
        std::lock_guard lock(warmup::g_warmupMutex);
        if (warmup::g_warmupRunning.load()) return;
        warmup::g_warmupStopping.store(false);
        warmup::g_warmupRunning.store(true);
        warmup::g_warmupThread = std::thread(warmup::Worker, false);
        if (!AsyncPipelines()) return;
        const uint32_t threads = warmup::CompileThreads();
        for (uint32_t i = 0; i < threads; i++) warmup::g_demandThreads.emplace_back(warmup::Worker, true);
        LOGI("warmup: %u thread%s for the pipelines draws wait for", threads, threads == 1 ? "" : "s");
    }

    // MW2_ASYNC_PIPELINES: the pipeline for a draw if it is built, and
    // otherwise null with `compiling` set, once the workers have been asked
    // for it. Null without it is a pipeline the driver refused. The
    // consumer never builds one itself: it did once a draw had been skipped
    // for twenty frames, one a frame, and a first visit's hundreds were then
    // 25 to 35 ms of every frame for as long as they lasted, behind the
    // worker compiling the same ones -- 193 of an AYN Thor's 201 logged
    // stutters in a multiplayer match, back to back. The surfaces stayed
    // undrawn for as long either way; how soon they come is the workers'
    // number (CompileThreads). The consumer's alone, and on its path for
    // every draw: a run of draws with a built pipeline stops at the first
    // comparison, a run waiting for one at the second, and only a change of
    // pipeline takes the lock.
    VkPipeline PipelineWhenReady(const PipelineKey& key, uint32_t depthControl, uint32_t modeCntl,
                                 bool& compiling)
    {
        compiling = false;
        TakeOptimisedPipelines();
        if (g.lastPipeline && g.lastPipelineSwaps == g.pipelinesOptimised && key == g.lastPipelineKey)
            return g.lastPipeline;
        State::RecentPipeline& recent = g.recentPipelines[
            ((key.vertexShader ^ std::rotl(key.pixelShader, 21) ^ key.blendControl ^ (uint64_t(key.colourMask) << 32) ^
              (uint64_t(key.topology) << 40) ^ (uint64_t(key.colourFormat) << 44) ^ (uint64_t(key.samples) << 52)) *
             0x9E3779B97F4A7C15ull) >> 57];
        static_assert(State::kRecentPipelines == 1u << 7, "the place is the hash's top seven bits");
        if (recent.pipeline && recent.swaps == g.pipelinesOptimised && recent.key == key)
        {
            g.lastPipelineKey = key;
            g.lastPipeline = recent.pipeline;
            g.lastPipelineSwaps = g.pipelinesOptimised;
            return recent.pipeline;
        }

        State::Awaited& awaited = g.awaited;
        // Read before the map is: a pipeline published after this is either
        // found below or moves the count past what is remembered here.
        const uint64_t published = g_pipelinesPublished.load(std::memory_order_acquire);
        if (!awaited.lastValid || awaited.lastPublished != published || key != awaited.lastKey)
        {
            {
                std::lock_guard lock(g_pipelinesMutex);
                if (awaited.settledAt != published)
                {
                    // Something was published: the waits that ended with it.
                    awaited.settledAt = published;
                    for (auto waiting = awaited.since.begin(); waiting != awaited.since.end();)
                    {
                        if (!g.pipelines.contains(waiting->first)) { ++waiting; continue; }
                        g.longestPipelineWait = std::max(g.longestPipelineWait, g.frames - waiting->second);
                        waiting = awaited.since.erase(waiting);
                    }
                }
                auto found = g.pipelines.find(key);
                if (found != g.pipelines.end())
                {
                    g.lastPipelineKey = key;
                    g.lastPipeline = found->second;
                    g.lastPipelineSwaps = g.pipelinesOptimised;
                    recent = { key, found->second, g.pipelinesOptimised };
                    return found->second;
                }
            }
            const bool first = awaited.since.emplace(key, g.frames).second;
            if (first)
            {
                const Recorded recorded{ key.vertexShader, key.pixelShader, key.colourFormat, key.depthFormat,
                                         key.topology, depthControl, key.blendControl, key.colourMask,
                                         modeCntl, key.samples };
                if (ShaderCachePath())
                {
                    // Once in the cache each, the ones a previous run left
                    // there and the worker has not reached included.
                    if (!awaited.recordedSeeded)
                    {
                        awaited.recordedSeeded = true;
                        for (const Recorded& known : g.recorded) awaited.recorded.insert(KeyOf(known));
                    }
                    if (awaited.recorded.insert(key).second) RecordPipeline(recorded);
                }
                {
                    std::lock_guard lock(warmup::g_warmupMutex);
                    warmup::g_demandQueue.push_back({ recorded });
                    warmup::g_statQueued.fetch_add(1, std::memory_order_relaxed);
                }
                StartWarmupScheduler();
                warmup::g_warmupCv.notify_all();
            }
            awaited.lastKey = key;
            awaited.lastPublished = published;
            awaited.lastValid = true;
        }
        compiling = true;
        g.drawsAwaitingPipelines++;
        return VK_NULL_HANDLE;
    }

    void ReportAsyncPipelines()
    {
        if (!AsyncPipelines()) return;
        LOGI("renderer: MW2_ASYNC_PIPELINES: %llu pipelines built in the background on %u thread%s, %llu draws"
             " skipped while waiting for one, the longest wait %llu frames",
             (unsigned long long)g_pipelinesBuiltForDraws.load(std::memory_order_relaxed),
             warmup::CompileThreads(), warmup::CompileThreads() == 1 ? "" : "s",
             (unsigned long long)g.drawsAwaitingPipelines, (unsigned long long)g.longestPipelineWait);
    }

    void StopWarmupScheduler()
    {
        // First, and whether or not the workers ever ran: what it would
        // write is written whole by the caller once everything has stopped.
        saver::Stop();
        {
            std::lock_guard lock(warmup::g_warmupMutex);
            if (!warmup::g_warmupRunning.load()) return;
            warmup::g_warmupStopping.store(true);
            warmup::g_warmupCv.notify_all();
        }
        if (warmup::g_warmupThread.joinable())
            warmup::g_warmupThread.join();
        for (std::thread& thread : warmup::g_demandThreads) thread.join();
        warmup::g_demandThreads.clear();
        warmup::g_warmupRunning.store(false);
    }

    void SetCutsceneActive(bool active)
    {
        if (warmup::g_cutsceneActive.exchange(active, std::memory_order_relaxed) == active || !active) return;
        // The world has just stopped being drawn: the draws' threads may
        // take the warm-up's queue (warmup::Worker). Under the lock they
        // wait with, or one about to wait would not hear it.
        std::lock_guard lock(warmup::g_warmupMutex);
        warmup::g_warmupCv.notify_all();
    }

    void NoteRenderSubmit()
    {
        warmup::NoteRenderSubmit();
    }

    WarmupStats GetWarmupStats()
    {
        WarmupStats s;
        s.queued = warmup::g_statQueued.load(std::memory_order_relaxed);
        s.completed = warmup::g_statCompleted.load(std::memory_order_relaxed);
        s.misses = warmup::g_statMisses.load(std::memory_order_relaxed);
        s.lastCompileMs = warmup::g_statLastCompileMs.load(std::memory_order_relaxed);
        s.cutsceneActive = warmup::g_cutsceneActive.load(std::memory_order_relaxed);
        return s;
    }

    // MW2_SHADER_CACHE=<file> records every pipeline a run needed and rebuilds
    // them at start-up on the next -- the "compiling shaders" pass the console
    // never needed. It cannot be built from the shaders alone: a pipeline is a
    // shader pair *and* the state it is drawn with, and a previous run is the only
    // place that state exists.
    const char* ShaderCachePath()
    {
        static const char* path = env::Text("MW2_SHADER_CACHE");
        return path;
    }

    constexpr uint32_t kCacheMagic = 0x32574D53;   // 'SMW2'
    constexpr uint32_t kCacheVersion = 5;   // 5: a pixel program runs once for
                                            // each record its subgroup holds

    // Any thread, one at a time (saver::Save, and Shutdown once the saver
    // has stopped): the list and the shaders are copied under their locks
    // and written without them, so the consumer waits for neither the
    // storage nor a megabyte of words. A shader is never taken out of
    // g.shaders before Shutdown and its words never change, so the pointers
    // stay good.
    void WriteShaderCache()
    {
        const char* path = ShaderCachePath();
        if (!path) return;
        std::vector<Recorded> recorded;
        {
            std::lock_guard lock(g_recordedMutex);
            recorded = g.recorded;
        }
        if (recorded.empty()) return;
        std::vector<std::pair<uint64_t, const Shader*>> shaders;
        {
            std::lock_guard lock(g_shadersMutex);
            shaders.reserve(g.shaders.size());
            for (const auto& [hash, shader] : g.shaders) shaders.emplace_back(hash, &shader);
        }
        const std::string tmpPath = std::string(path) + ".tmp";
        std::FILE* file = std::fopen(tmpPath.c_str(), "wb");
        if (!file) return;

        bool writeOk = true;
        const uint32_t header[4] = { kCacheMagic, kCacheVersion,
                                     uint32_t(shaders.size()), uint32_t(recorded.size()) };
        if (std::fwrite(header, sizeof header, 1, file) != 1) writeOk = false;
        for (const auto& [hash, shader] : shaders)
        {
            if (!writeOk) break;
            const std::vector<uint32_t>& code = shader->code;
            const uint32_t entry[2] = { uint32_t(shader->type == shader::Type::Pixel), uint32_t(code.size()) };
            if (std::fwrite(&hash, sizeof hash, 1, file) != 1 ||
                std::fwrite(entry, sizeof entry, 1, file) != 1 ||
                (code.size() && std::fwrite(code.data(), 4, code.size(), file) != code.size()))
            {
                writeOk = false;
                break;
            }
        }
        if (writeOk && std::fwrite(recorded.data(), sizeof(Recorded), recorded.size(), file) != recorded.size())
            writeOk = false;

        if (std::fclose(file) != 0) writeOk = false;

        if (writeOk)
        {
            if (std::rename(tmpPath.c_str(), path) == 0)
            {
                LOGI("renderer: wrote %zu shaders and %zu pipelines to %s",
                     shaders.size(), recorded.size(), path);
            }
            else
            {
                LOGW("renderer: failed to commit shader cache to %s", path);
                std::remove(tmpPath.c_str());
            }
        }
        else
        {
            LOGW("renderer: failed writing shader cache, removing temporary file");
            std::remove(tmpPath.c_str());
        }
    }

    // Work the draw path would otherwise do mid-frame, which is where a hitch
    // comes from -- the total is small, the moment it is paid is the problem.
    void PrewarmFromCache()
    {
        const char* path = ShaderCachePath();
        if (!path) return;
        std::FILE* file = std::fopen(path, "rb");
        if (!file) return;

        std::fseek(file, 0, SEEK_END);
        const long fileSize = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (fileSize < 16) { std::fclose(file); return; }

        uint32_t header[4]{};
        if (std::fread(header, sizeof header, 1, file) != 1 ||
            header[0] != kCacheMagic || header[1] != kCacheVersion)
        {
            std::fclose(file);
            LOGW("renderer: %s is not a shader cache for this build, ignoring it", path);
            return;
        }

        // Sanity bound: max 50,000 shaders or pipelines
        if (header[2] > 50000 || header[3] > 50000)
        {
            std::fclose(file);
            LOGW("renderer: %s has invalid header counts (%u, %u), ignoring it", path, header[2], header[3]);
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < header[2]; i++)
        {
            const long curPos = std::ftell(file);
            if (curPos < 0 || curPos + (long)(sizeof(uint64_t) + sizeof(uint32_t) * 2) > fileSize)
            {
                std::fclose(file);
                LOGW("renderer: %s truncated while reading shader header %u", path, i);
                return;
            }
            uint64_t hash = 0;
            uint32_t entry[2]{};
            if (std::fread(&hash, sizeof hash, 1, file) != 1 ||
                std::fread(entry, sizeof entry, 1, file) != 1) { std::fclose(file); return; }
            if (entry[1] > 256 * 1024) { std::fclose(file); return; } // Max 1 MB shader
            const long dataBytes = (long)entry[1] * 4;
            if (std::ftell(file) + dataBytes > fileSize)
            {
                std::fclose(file);
                LOGW("renderer: %s truncated payload for shader %u", path, i);
                return;
            }
            std::vector<uint32_t> code(entry[1]);
            if (entry[1] && std::fread(code.data(), 4, code.size(), file) != code.size())
            { std::fclose(file); return; }
            ShaderFor(entry[0] ? shader::Type::Pixel : shader::Type::Vertex,
                      hash, code.data(), code.size());
        }

        const long recBytes = (long)header[3] * (long)sizeof(Recorded);
        if (std::ftell(file) + recBytes > fileSize)
        {
            std::fclose(file);
            LOGW("renderer: %s truncated recorded pipeline records", path);
            return;
        }

        std::vector<Recorded> wanted(header[3]);
        if (header[3] && std::fread(wanted.data(), sizeof(Recorded), wanted.size(), file)
                             != wanted.size())
        { std::fclose(file); return; }
        std::fclose(file);

        g.prewarmWanted = uint32_t(wanted.size());
        const bool hasFastLibraries = vk::pipeline::PipelineLibraries();
        // The whole recorded set, synchronously, when the player asked for it
        // in Settings (MW2_SHADER_PREWARM_ALL): slow boot, no warmup debt
        // afterwards. Otherwise the instant-boot budget, sized for this
        // device (android::perf::PrewarmBudget).
        const bool prewarmAll = env::Flag("MW2_SHADER_PREWARM_ALL");
        uint32_t maxSync = 15, deadlineMs = 250;
#ifdef MW2_ANDROID
        if (!prewarmAll) android::perf::PrewarmBudget(maxSync, deadlineMs);
#endif
        const auto syncDeadline = start + std::chrono::milliseconds(deadlineMs);
        uint32_t last = 0;
        LOGI("renderer: prewarming %u recorded pipelines%s", uint32_t(wanted.size()),
             prewarmAll ? ", all of them synchronously" : "");

        const auto shadersOf = [](const Recorded& want, const Shader*& vertex, const Shader*& pixel) {
            const auto v = g.shaders.find(want.vertexShader);
            const auto p = g.shaders.find(want.pixelShader);
            if (v == g.shaders.end() || p == g.shaders.end() || !v->second.module || !p->second.module)
                return false;
            vertex = &v->second;
            pixel = &p->second;
            return true;
        };
        // The list whole before the first of them is built: the saver
        // writes what is in it, and a list written halfway through this
        // would have lost the rest for good.
        {
            std::lock_guard lock(g_recordedMutex);
            const Shader* vertex = nullptr;
            const Shader* pixel = nullptr;
            for (const Recorded& want : wanted)
                if (shadersOf(want, vertex, pixel)) g.recorded.push_back(want);
        }

        for (size_t i = 0; i < wanted.size(); i++)
        {
            const Recorded& want = wanted[i];
            const Shader* vertex = nullptr;
            const Shader* pixel = nullptr;
            if (!shadersOf(want, vertex, pixel)) continue;
            const PipelineKey key = KeyOf(want);

            const bool syncCandidate = prewarmAll || ((i < maxSync) &&
                                       (hasFastLibraries || std::chrono::steady_clock::now() < syncDeadline));
            if (syncCandidate)
            {
                if (EnsurePipeline(key, *vertex, *pixel)) g.prewarmed++;
                const uint32_t percent = 100 * uint32_t(i + 1) / uint32_t(wanted.size());
                const uint32_t step = prewarmAll ? 10 : 25;
                if (percent >= last + step) { LOGI("compiling shaders: %u%%", percent); last = percent; }
            }
            else
            {
                // Enqueue for background warmup scheduler
                std::lock_guard lock(warmup::g_warmupMutex);
                warmup::g_warmupQueue.push_back({ want });
                warmup::g_statQueued.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (!warmup::g_warmupQueue.empty())
        {
            LOGI("warmup: queued %u pipelines for background warmup during menus/cutscenes",
                 warmup::g_statQueued.load(std::memory_order_relaxed));
            StartWarmupScheduler();
            warmup::g_warmupCv.notify_all();
        }

        warmup::g_statCompleted.store(g.prewarmed, std::memory_order_relaxed);

        g.prewarmMicroseconds = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        LOGI("renderer: prewarmed %u of %u pipelines over %zu shaders in %llu us",
             g.prewarmed, g.prewarmWanted, g.shaders.size(),
             (unsigned long long)g.prewarmMicroseconds);
    }

    void SaveCachesAtEnd() { saver::Save(true); }
}

void vk::renderer::SaveCaches() { detail::saver::Save(); }

#endif  // MW2_HAVE_VULKAN

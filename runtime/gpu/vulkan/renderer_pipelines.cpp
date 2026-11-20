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
        // Set on the warm-up worker, which builds pipelines beside the consumer.
        thread_local bool t_warmupThread = false;
        // How many entries g.pipelines has been given, counted after each is
        // in the map. A draw waiting for the worker (PipelineWhenReady)
        // looks in the map again only when this has moved.
        std::atomic<uint64_t> g_pipelinesPublished{ 0 };
        // The pipelines the worker built because a draw was waiting for them.
        std::atomic<uint64_t> g_pipelinesBuiltForDraws{ 0 };

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
        auto& last = g.lastShader[type == shader::Type::Pixel];
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

        if (vk::pipeline::ShaderStats()) info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

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
        // One thread for each queue. A thread that has lowered its priority
        // cannot be given it back (platform.h), and a draw is lost every
        // frame its pipeline is not there: the demand queue's thread keeps
        // the priority it was made with, on a performance core.
        std::thread g_warmupThread, g_demandThread;
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

        void Worker(bool demanded)
        {
            t_warmupThread = true;
            if (demanded)
                platform::SetThreadAffinity(platform::ThreadAffinityRole::PerformanceCore);
            else
            {
                platform::LowerThreadPriority();
                platform::SetThreadAffinity(platform::ThreadAffinityRole::WorkerCore);
            }
            std::deque<QueueItem>& queue = demanded ? g_demandQueue : g_warmupQueue;
            LOGI("warmup: %s worker thread started", demanded ? "demand" : "scheduler");
            while (!g_warmupStopping.load(std::memory_order_relaxed))
            {
                QueueItem item{};
                {
                    std::unique_lock lock(g_warmupMutex);
                    g_warmupCv.wait(lock, [&queue] {
                        return g_warmupStopping.load(std::memory_order_relaxed) || !queue.empty();
                    });

                    if (g_warmupStopping.load(std::memory_order_relaxed))
                        break;

                    item = queue.front();
                    queue.pop_front();
                }

                const PipelineKey key = KeyOf(item.recorded);

                // Check if already created
                {
                    std::lock_guard lock(g_pipelinesMutex);
                    if (g.pipelines.find(key) != g.pipelines.end())
                        continue;
                }

                Shader vertexCopy{};
                Shader pixelCopy{};
                bool foundShaders = false;
                {
                    std::lock_guard lock(g_shadersMutex);
                    auto vIt = g.shaders.find(key.vertexShader);
                    auto pIt = g.shaders.find(key.pixelShader);
                    if (vIt != g.shaders.end() && vIt->second.module &&
                        pIt != g.shaders.end() && pIt->second.module)
                    {
                        vertexCopy = vIt->second;
                        pixelCopy = pIt->second;
                        foundShaders = true;
                    }
                }

                if (!foundShaders)
                    continue;

                const auto start = std::chrono::steady_clock::now();
                bool made = false;
                VkPipeline built = EnsurePipeline(key, vertexCopy, pixelCopy, &made);
                const uint32_t tookMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());

                // No pause after a pipeline a draw is waiting for, and the
                // driver's cache is not written out between them: the
                // consumer would have compiled it at this same moment itself.
                // The pauses below pace the ones nothing is waiting for.
                if (demanded)
                {
                    if (built && made)
                    {
                        g_pipelinesBuiltForDraws.fetch_add(1, std::memory_order_relaxed);
                        g_statCompleted.fetch_add(1, std::memory_order_relaxed);
                        g_statLastCompileMs.store(tookMs, std::memory_order_relaxed);
                        LOGI("warmup: pipeline %016llX/%016llX a draw waited for built in %u ms",
                             (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
                             tookMs);
                    }
                    continue;
                }

                if (built && made)
                {
                    const uint32_t c = g_statCompleted.fetch_add(1, std::memory_order_relaxed) + 1;
                    g_statLastCompileMs.store(tookMs, std::memory_order_relaxed);
                    LOGI("warmup: precompiled pipeline %016llX/%016llX in %u ms (%u of %u ready)",
                         (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader,
                         tookMs, c, g_statQueued.load(std::memory_order_relaxed));

                    static uint32_t s_lastSave = 0;
                    if (c - s_lastSave >= 5)
                    {
                        s_lastSave = c;
                        vk::pipeline::SaveCache();
                    }
                }

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
                if (sleepMs >= 200)
                    LOGI("warmup: render-active backpressure -- slept %d ms after %u ms compile",
                         sleepMs, tookMs);
            }
            LOGI("warmup: %s worker thread exiting", demanded ? "demand" : "scheduler");
        }
    }

    void StartWarmupScheduler()
    {
        std::lock_guard lock(warmup::g_warmupMutex);
        if (warmup::g_warmupRunning.load()) return;
        warmup::g_warmupStopping.store(false);
        warmup::g_warmupRunning.store(true);
        warmup::g_warmupThread = std::thread(warmup::Worker, false);
        if (AsyncPipelines()) warmup::g_demandThread = std::thread(warmup::Worker, true);
    }

    // How many frames a draw is skipped for before the consumer builds its
    // pipeline itself. The phones' drivers are given one creation at a time
    // (BuildPipeline), so the worker gets through a first visit's hundred
    // and more pipelines one after another: at 50 ms each on an Adreno 650
    // the last of them left its walls undrawn for 288 frames, ten seconds
    // of looking through them (Retroid Pocket Flip 2, a first Seatown
    // match). A frame held up is the lesser fault from here on. Where the
    // worker keeps up, a wait is over well inside this and nothing changes.
    constexpr uint64_t kLongestSkip = 20;

    // MW2_ASYNC_PIPELINES: the pipeline for a draw if it is built, and
    // otherwise null with `compiling` set, once the worker has been asked
    // for it, or with `overdue` set once its draws have been skipped for
    // kLongestSkip frames: then the caller builds it. Null with neither is a
    // pipeline the driver refused. The consumer's alone, and on its path for
    // every draw: a run of draws with a
    // built pipeline stops at the first comparison, a run waiting for one at
    // the second, and only a change of pipeline takes the lock.
    VkPipeline PipelineWhenReady(const PipelineKey& key, uint32_t depthControl, uint32_t modeCntl,
                                 bool& compiling, bool& overdue)
    {
        compiling = overdue = false;
        TakeOptimisedPipelines();
        if (g.lastPipeline && g.lastPipelineSwaps == g.pipelinesOptimised && key == g.lastPipelineKey)
            return g.lastPipeline;

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
                    return found->second;
                }
            }
            const auto [waiting, first] = awaited.since.emplace(key, g.frames);
            awaited.lastSince = waiting->second;
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
                    if (awaited.recorded.insert(key).second)
                    {
                        g.recorded.push_back(recorded);
                        if (++awaited.unsaved >= 25) { awaited.unsaved = 0; WriteShaderCache(); }
                    }
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
        if (g.frames - awaited.lastSince >= kLongestSkip)
        {
            // Asked again at its next draw, which finds it built.
            awaited.lastValid = false;
            g.pipelinesOverdue++;
            overdue = true;
            return VK_NULL_HANDLE;
        }
        compiling = true;
        g.drawsAwaitingPipelines++;
        return VK_NULL_HANDLE;
    }

    void ReportAsyncPipelines()
    {
        if (!AsyncPipelines()) return;
        LOGI("renderer: MW2_ASYNC_PIPELINES: %llu pipelines built in the background, %llu draws skipped"
             " while waiting for one, the longest wait %llu frames; %llu built at their draw after"
             " %llu frames of waiting",
             (unsigned long long)g_pipelinesBuiltForDraws.load(std::memory_order_relaxed),
             (unsigned long long)g.drawsAwaitingPipelines, (unsigned long long)g.longestPipelineWait,
             (unsigned long long)g.pipelinesOverdue, (unsigned long long)kLongestSkip);
    }

    void StopWarmupScheduler()
    {
        {
            std::lock_guard lock(warmup::g_warmupMutex);
            if (!warmup::g_warmupRunning.load()) return;
            warmup::g_warmupStopping.store(true);
            warmup::g_warmupCv.notify_all();
        }
        if (warmup::g_warmupThread.joinable())
            warmup::g_warmupThread.join();
        if (warmup::g_demandThread.joinable())
            warmup::g_demandThread.join();
        warmup::g_warmupRunning.store(false);
    }

    void SetCutsceneActive(bool active)
    {
        warmup::g_cutsceneActive.store(active, std::memory_order_relaxed);
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

    void WriteShaderCache()
    {
        const char* path = ShaderCachePath();
        if (!path || g.recorded.empty()) return;
        const std::string tmpPath = std::string(path) + ".tmp";
        std::FILE* file = std::fopen(tmpPath.c_str(), "wb");
        if (!file) return;

        bool writeOk = true;
        const uint32_t header[4] = { kCacheMagic, kCacheVersion,
                                     uint32_t(g.shaders.size()), uint32_t(g.recorded.size()) };
        if (std::fwrite(header, sizeof header, 1, file) != 1) writeOk = false;
        for (const auto& [hash, shader] : g.shaders)
        {
            if (!writeOk) break;
            const uint32_t entry[2] = { uint32_t(shader.type == shader::Type::Pixel),
                                        uint32_t(shader.code.size()) };
            if (std::fwrite(&hash, sizeof hash, 1, file) != 1 ||
                std::fwrite(entry, sizeof entry, 1, file) != 1 ||
                (shader.code.size() && std::fwrite(shader.code.data(), 4, shader.code.size(), file) != shader.code.size()))
            {
                writeOk = false;
                break;
            }
        }
        if (writeOk && !g.recorded.empty())
        {
            if (std::fwrite(g.recorded.data(), sizeof(Recorded), g.recorded.size(), file) != g.recorded.size())
                writeOk = false;
        }

        if (std::fclose(file) != 0) writeOk = false;

        if (writeOk)
        {
            if (std::rename(tmpPath.c_str(), path) == 0)
            {
                LOGI("renderer: wrote %zu shaders and %zu pipelines to %s",
                     g.shaders.size(), g.recorded.size(), path);
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

        // Commit driver's persistent pipeline cache alongside recipes
        vk::pipeline::SaveCache();
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

        for (size_t i = 0; i < wanted.size(); i++)
        {
            const Recorded& want = wanted[i];
            auto vertex = g.shaders.find(want.vertexShader);
            auto pixel = g.shaders.find(want.pixelShader);
            if (vertex == g.shaders.end() || pixel == g.shaders.end() ||
                !vertex->second.module || !pixel->second.module)
                continue;
            const PipelineKey key = KeyOf(want);

            g.recorded.push_back(want);

            const bool syncCandidate = prewarmAll || ((i < maxSync) &&
                                       (hasFastLibraries || std::chrono::steady_clock::now() < syncDeadline));
            if (syncCandidate)
            {
                if (EnsurePipeline(key, vertex->second, pixel->second)) g.prewarmed++;
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
}

#endif  // MW2_HAVE_VULKAN

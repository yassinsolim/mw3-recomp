// MW2_SCENE_DYNAMIC: the scene drawn smaller while the graphics chip is late.
//
// The title makes its surfaces once, so a scene that follows the load cannot
// be the title's: its surfaces keep their size and the renderer draws into a
// part of them, from the first pixel. Every pass on a surface of the scene's
// pitch has its viewport and scissor taken to that part, and a resolve out
// of such a surface stretches the part over the rectangle the title names,
// so every texture the title reads back is the size it expects and nothing
// after the scene knows. The frame, with the HUD and the menus, is another
// surface and keeps its pixels.
//
// The share is stepped once a window of frames by what those frames took of
// the graphics chip, by its own clock (gpu_time.h): down when a frame takes
// more than the display gives it and the chip was at work for most of the
// window (not when the processor is late), up again when the step above
// would fit with room to spare. A Retroid Pocket Flip 2 (Adreno 650) in Modern Warfare
// 2's flight to the Gulag draws the 832x480 scene in 17 to 22 ms where the
// display gives 16.7 (46 to 58 frames a second), and a 640x368 one in 15 to
// 18 (55 to 60): the scene's pixels are what is left to give there
// (docs/android.md). It was on for that class of chip alone; under Android it
// is on for every chip now, since what decides it is the chip's time and not
// its name, and a chip with time in hand never leaves the first step. Where
// the sun's shadows are the runtime's to choose they go before the first
// pixel does (adaptive.h).
#include "renderer_state.h"
#include "gpu_time.h"
#include "../../adaptive.h"
#include "../../frame_rate.h"
#include "../../log.h"
#include "../../render_size.h"
#include "../../report.h"
#ifdef MW2_ANDROID
#include "../../android/android.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    namespace
    {
        // A window is long enough for the chip's times to have come back
        // (they are read a frame or two after the frame) and short enough to
        // follow a view that turns towards a city: a sixth of a second at 60.
        constexpr uint32_t kWindow = 10;
        // Frames left out after a step, whose times are still the old size's.
        constexpr uint32_t kSettle = 4;
        // Windows in a row with room for the step above before it is taken:
        // a second and a half, so the size does not pump at a view's edge.
        constexpr uint32_t kCalm = 9;
        // The share of a window's time the chip must be at work for its
        // frames to be the chip's (adaptive::Governor::kChipLate's share).
        constexpr double kChipFull = adaptive::Governor::kChipLate;

        void Draw(uint32_t level)
        {
            State::Scene& scene = g.scene;
            scene.level = level;
            const float share = State::Scene::kShares[level];
            // Even, as the title's own sizes are.
            scene.drawnWidth = level ? uint32_t(std::lround(float(scene.width) * share / 2.0f)) * 2 : scene.width;
            scene.drawnHeight = level ? uint32_t(std::lround(float(scene.height) * share / 2.0f)) * 2 : scene.height;
        }

        // What a frame is given: the display's period, or the limit's where
        // that is longer.
        double FrameNanoseconds()
        {
            const double hz = frame_rate::DisplayHz();
            const double limit = double(frame_rate::Limit());
            double rate = limit > 0 && (hz <= 0 || limit < hz) ? limit : hz;
            return 1e9 / (rate > 0 ? rate : double(frame_rate::kConsole));
        }

        double Pixels(uint32_t level)
        {
            const float share = State::Scene::kShares[level];
            return double(share) * double(share);
        }
    }

    bool Stretches(VkFormat format, bool depth)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(g.physical, format, &properties);
        VkFormatFeatureFlags wanted = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        // Depth goes through as it is; colour is filtered.
        if (!depth) wanted |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (properties.optimalTilingFeatures & wanted) == wanted;
    }

    bool StartScene()
    {
        State::Scene& scene = g.scene;
        const char* const asked = env::Text("MW2_SCENE_DYNAMIC");
        bool on = asked ? env::Flag("MW2_SCENE_DYNAMIC") : false;
        // Wherever the run follows what the device sustains: under Android.
        if (!asked) on = adaptive::On();
        // Diagnostic, MW2_SCENE_SHARE=<percent>[,<frames>]: that share for the
        // whole run, or it and the whole scene in turn, so many frames each
        // -- two pictures of one view, to compare.
        unsigned fixed = 0, turn = 0;
        if (const char* const share = env::Text("MW2_SCENE_SHARE")) std::sscanf(share, "%u,%u", &fixed, &turn);
        if (!on && !fixed) return false;
        const render::Size size = render::SceneAsked(), frame = render::Frame();
        // A surface is a whole number of EDRAM tiles wide, and the scene's
        // are told from the frame's by that width.
        const uint32_t pitch = (size.width + 79) / 80 * 80;
        if (pitch == (frame.width + 79) / 80 * 80 || g.scale != 1)
        {
            LOGI("renderer: the scene keeps its size: %s", g.scale != 1 ? "MW2_SCALE draws it larger"
                                                                         : "its surfaces are as wide as the frame's");
            return false;
        }
        scene.on = true;
        scene.pitch = pitch;
        scene.width = size.width;
        scene.height = size.height;
        Draw(0);
        if (fixed)
        {
            uint32_t level = State::Scene::kLevels - 1;
            while (level && State::Scene::kShares[level] * 100.0f < float(fixed)) level--;
            scene.fixed = level;
            scene.turn = turn;
            scene.stepping = false;
            Draw(level);
        }
        LOGI("renderer: the %ux%u scene is drawn smaller while the graphics chip is late, down to %ux%u%s;"
             " MW2_SCENE_DYNAMIC=0 keeps its size",
             scene.width, scene.height,
             uint32_t(std::lround(float(scene.width) * State::Scene::kShares[State::Scene::kLevels - 1] / 2.0f)) * 2,
             uint32_t(std::lround(float(scene.height) * State::Scene::kShares[State::Scene::kLevels - 1] / 2.0f)) * 2,
             scene.stepping ? "" : " (MW2_SCENE_SHARE says when instead)");
        return true;
    }

    void SceneFrame(bool world)
    {
        State::Scene& scene = g.scene;
        if (!scene.on) return;
        if (!world)
        {
            // A menu or a loading screen says nothing of the scene.
            scene.frames = 0;
            scene.settle = kSettle;
            return;
        }
        scene.framesAt[scene.level]++;
        report::Scene(uint32_t(std::lround(State::Scene::kShares[scene.level] * 100.0f)));
        if (!scene.stepping)
        {
            if (scene.turn && ++scene.frames >= scene.turn)
            {
                scene.frames = 0;
                Draw(scene.level ? 0 : scene.fixed);
            }
            return;
        }
        if (scene.settle)
        {
            if (!--scene.settle)
            {
                scene.busyAt = vk::gpu_time::Busy();
                scene.startedAt = std::chrono::steady_clock::now();
                scene.frames = 0;
            }
            return;
        }
        if (++scene.frames < kWindow) return;
        const uint64_t busy = vk::gpu_time::Busy();
        const auto now = std::chrono::steady_clock::now();
        const double taken = double(busy - scene.busyAt) / double(kWindow);
        // The share of the window's own time the chip was at work. A chip
        // with time to spare between frames is not what they waited for,
        // however long each one took it, and fewer pixels bring them no
        // sooner: Black Ops III's The Giant on an AYN Thor at 120 came at
        // 104 to 118 frames a second with the chip at work about 70% of
        // the time (6.3 to 6.6 ms a frame), and the scene stepped down to
        // 58% of its width while the chip's time a frame went from 6.6 ms
        // at 84% to 6.3 at 58%.
        const double wall = std::chrono::duration<double, std::nano>(now - scene.startedAt).count();
        const bool chipFull = wall > 0 && double(busy - scene.busyAt) >= kChipFull * wall;
        scene.busyAt = busy;
        scene.startedAt = now;
        scene.frames = 0;

        const double given = FrameNanoseconds();
        // What the frame would take with the chip's clock at its fastest: the
        // governor holds the clock down while there is time in hand, and a
        // frame that only looks long for that is not a reason to draw less.
        double atFull = taken;
#ifdef MW2_ANDROID
        atFull *= android::perf::AdrenoClockShare();
#endif
        const uint32_t last = State::Scene::kLevels - 1;
        if ((taken > 0.97 * given || atFull > 0.90 * given) && chipFull && scene.level < last)
        {
            // The sun's shadows before the scene's pixels, where they are the
            // runtime's choice and on: the title takes a few frames to stop
            // drawing them, and the windows after that say what is left.
            if (adaptive::GiveUpShadows())
            {
                scene.settle = 3 * kSettle;
                scene.calm = 0;
                return;
            }
            // Two steps where one would still leave it late.
            Draw(std::min(last, scene.level + (atFull > 1.12 * given ? 2u : 1u)));
            scene.settle = kSettle;
            scene.calm = 0;
            return;
        }
        if (!scene.level) return;
        // The step above, were everything the frame takes its pixels: at
        // most half of it is (the rest is vertices and the frame's own
        // passes), so this is on the careful side. Measured against the
        // frames' own period where the chip had time to spare in it: with
        // the processor late the chip has that much more time, and The
        // Giant on the Thor stayed at 58% for seconds at 91 to 100 frames a
        // second with its chip at 7.0 to 7.7 ms a frame, 1.0 ms less than at
        // the whole scene.
        const double room = chipFull ? given : std::max(given, wall / double(kWindow));
        const double above = atFull * (0.5 + 0.5 * Pixels(scene.level - 1) / Pixels(scene.level));
        scene.calm = above < 0.82 * room ? scene.calm + 1 : 0;
        if (scene.calm >= kCalm)
        {
            Draw(scene.level - 1);
            scene.settle = kSettle;
            scene.calm = 0;
        }
    }

    adaptive::Scene SceneNow()
    {
        const State::Scene& scene = g.scene;
        adaptive::Scene now;
        // A fixed share (MW2_SCENE_SHARE) follows nothing.
        now.dynamic = scene.on && scene.stepping;
        now.level = scene.level;
        now.levels = State::Scene::kLevels;
        now.percent = scene.on ? uint32_t(std::lround(State::Scene::kShares[scene.level] * 100.0f)) : 100;
        return now;
    }

    void SceneReport()
    {
        const State::Scene& scene = g.scene;
        uint64_t frames = 0;
        for (const uint64_t at : scene.framesAt) frames += at;
        if (!scene.on || !frames) return;
        char line[State::Scene::kLevels * 32];
        int length = 0;
        for (uint32_t level = 0; level < State::Scene::kLevels; level++)
            length += std::snprintf(line + length, sizeof line - size_t(length), "%s%.0f%% of its width in %.1f%%",
                                    level ? ", " : "", double(State::Scene::kShares[level]) * 100.0,
                                    100.0 * double(scene.framesAt[level]) / double(frames));
        LOGI("renderer: the %ux%u scene was drawn at %s of the world's %llu frames", scene.width, scene.height,
             line, (unsigned long long)frames);
    }

    void ToDrawnScene(VkViewport& viewport, VkRect2D& scissor)
    {
        const float across = float(g.scene.drawnWidth) / float(g.scene.width);
        const float down = float(g.scene.drawnHeight) / float(g.scene.height);
        viewport.x *= across;
        viewport.width *= across;
        viewport.y *= down;
        viewport.height *= down;
        // Outwards: a scissor of the whole scene is the whole part.
        const int32_t right = int32_t(std::ceil(float(scissor.offset.x + int64_t(scissor.extent.width)) * across));
        const int32_t bottom = int32_t(std::ceil(float(scissor.offset.y + int64_t(scissor.extent.height)) * down));
        scissor.offset.x = int32_t(std::floor(float(scissor.offset.x) * across));
        scissor.offset.y = int32_t(std::floor(float(scissor.offset.y) * down));
        scissor.extent.width = uint32_t(std::max(0, right - scissor.offset.x));
        scissor.extent.height = uint32_t(std::max(0, bottom - scissor.offset.y));
    }

    VkImageBlit SceneBlit(const Target& from, VkImageAspectFlags aspect, VkOffset2D at, VkOffset2D into,
                          uint32_t width, uint32_t height)
    {
        const float across = float(from.drawnWidth) / float(g.scene.width);
        const float down = float(from.drawnHeight) / float(g.scene.height);
        // A rectangle that reaches the scene's edge stops a pixel short of
        // the part's: the filter reads a pixel past where it is told to, and
        // what lies past the part is an older frame's.
        const auto end = [](uint32_t titles, uint32_t scenes, uint32_t drawn, float share) {
            return titles >= scenes ? int32_t(drawn) - 1 : int32_t(std::ceil(float(titles) * share));
        };
        VkImageBlit blit{};
        blit.srcSubresource = { aspect, 0, 0, 1 };
        blit.srcOffsets[0] = { int32_t(std::floor(float(at.x) * across)), int32_t(std::floor(float(at.y) * down)), 0 };
        blit.srcOffsets[1] = { end(uint32_t(at.x) + width, g.scene.width, from.drawnWidth, across),
                               end(uint32_t(at.y) + height, g.scene.height, from.drawnHeight, down), 1 };
        blit.srcOffsets[1].x = std::max(blit.srcOffsets[1].x, blit.srcOffsets[0].x + 1);
        blit.srcOffsets[1].y = std::max(blit.srcOffsets[1].y, blit.srcOffsets[0].y + 1);
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = { into.x, into.y, 0 };
        blit.dstOffsets[1] = { into.x + int32_t(width), into.y + int32_t(height), 1 };
        return blit;
    }
}

#endif  // MW2_HAVE_VULKAN

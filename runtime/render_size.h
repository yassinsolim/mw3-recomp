#pragma once
// The sizes the title draws at: its frame, which it shows -- the menus and the
// HUD are drawn into it -- and its scene, the 3D view it draws first and copies
// into the frame. The console's are a 1280x720 frame and a 1024x600 scene; a
// frame of another shape gives the picture that shape, and the title follows
// it, its menus and HUD included (render_size.cpp). MW2_RENDER=<w>x<h> is the
// frame, and the renderer draws it MW2_SCALE times wider and taller. The
// runtime and the launcher both work the sizes out here.
#include <cstdint>
#include <cstdio>
#include <optional>

namespace render
{
    struct Size
    {
        uint32_t width = 0, height = 0;
        bool operator==(const Size&) const = default;
    };

    constexpr Size kConsoleFrame{ 1280, 720 };
#if defined(BO3_TITLE)
    // Black Ops III's scene is as tall as its frame and 880 wide.
    constexpr Size kConsoleScene{ 880, 720 };
#else
    constexpr Size kConsoleScene{ 1024, 600 };
#endif

    // The title draws into EDRAM: 2048 tiles of 80x16 samples. A surface is a
    // whole number of tiles wide, and as many rows of samples tall as its
    // pixels times its samples (a 2x surface has two rows a pixel).
    constexpr uint32_t Tiles(Size size, uint32_t samples)
    {
        return (size.width + 79) / 80 * ((size.height * samples + 15) / 16);
    }

    // The frame is drawn at 1x, its colour and its depth in EDRAM together.
    constexpr bool FrameFits(Size frame) { return 2 * Tiles(frame, 1) <= 2048; }

    // The scene is drawn at the console's 2x, colour and depth together, and
    // takes no more tiles than the console's does: what the title keeps in
    // the rest beside it is not known.
    constexpr bool SceneFits(Size scene) { return Tiles(scene, 2) <= Tiles(kConsoleScene, 2); }

    // A frame the title can be given: as large as the console's at most in
    // either direction, and no smaller than its standard-definition 640 wide,
    // a whole number of 16 pixels wide and 4 tall, and fitting EDRAM.
    constexpr bool FrameValid(Size frame)
    {
        return frame.width >= 640 && frame.height >= 360 && frame.width <= 1280 && frame.height <= 800 &&
               frame.width % 16 == 0 && frame.height % 4 == 0 && FrameFits(frame);
    }

    // The scene that goes with a frame: the console's with the console's
    // frame; else the frame itself when it fits, or the largest of its shape,
    // 16 pixels narrower at a time, that does.
    constexpr Size Scene(Size frame)
    {
        if (frame == kConsoleFrame) return kConsoleScene;
        for (uint32_t width = frame.width / 16 * 16; width >= 16; width -= 16)
        {
            const Size scene{ width, width * frame.height / frame.width / 2 * 2 };
            if (SceneFits(scene)) return scene;
        }
        return kConsoleScene;
    }

    // A scene asked for by itself (MW2_SCENE), under a frame that keeps its
    // size: no larger than the one that goes with the frame, no smaller than
    // a quarter of the console's, a whole number of 16 pixels wide and 2 tall.
    // Its shape is free: the title stretches the scene over the frame, as it
    // does the console's 1024x600 over 1280x720.
    constexpr bool SceneValid(Size scene, Size frame)
    {
        const Size most = Scene(frame);
        return scene.width >= 512 && scene.height >= 300 && scene.width <= most.width &&
               scene.height <= most.height && scene.width % 16 == 0 && scene.height % 2 == 0;
    }

    // The scene an Adreno 6xx under Android starts Modern Warfare 2 with
    // (render_size.cpp): two thirds of the console's pixels, its shape.
    constexpr Size kAdreno6Scene{ 832, 480 };
    // Whether a frame is wider than 4:3. The video mode says the TV is
    // widescreen when it is, and the title takes its 4:3 layout when not, as
    // on a console set to a 4:3 TV.
    constexpr bool Widescreen(Size frame) { return frame.width * 3 > frame.height * 4; }

    // "960x600"; nullopt for anything else.
    inline std::optional<Size> Parse(const char* text)
    {
        unsigned width = 0, height = 0;
        char rest = 0;
        if (!text || std::sscanf(text, "%ux%u%c", &width, &height, &rest) != 2) return std::nullopt;
        return Size{ width, height };
    }

    // The runtime's: the frame MW2_RENDER asks for, or the console's when it
    // asks for none, for one FrameValid refuses, or when this build of the
    // title cannot be given one (render_size.cpp).
    Size Frame();

    // The runtime's: the scene the title draws under that frame, MW2_SCENE's
    // when it asks for one SceneValid takes, else the one that goes with the
    // frame.
    Size SceneAsked();
}

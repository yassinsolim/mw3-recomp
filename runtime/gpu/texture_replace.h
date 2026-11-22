#pragma once
// Texture replacement: a player's own picture uploaded in place of one of the
// title's textures (docs/textures.md).
//
// A texture has no name by the time it reaches the GPU -- a fetch constant is
// an address, a format and a size -- so a replacement is named by what the
// texture holds: a 64-bit key over its untiled bytes. The same picture has the
// same key in every run, wherever the title's allocator happened to put it,
// which is what lets a file made from one run's dump be found in the next.
//
// MW2_TEXTURE_REPLACE=<dir>, or `replace` in the game folder when it is there,
// holds the replacements: `<key>.png` or `<key>.pnm`, the key as the sixteen
// hexadecimal digits the dump names its files with, in the folder or any
// folder below it. MW2_DUMP_TEXTURES=<dir> writes every texture read as
// `<key>.png` there, with a line for each in `textures.txt`.
//
// A replacement may move: frames `<key>.0.png`, `<key>.1.png`, ... or numbered
// pictures in a folder named by the key's sixteen digits, all of one size,
// shown in turn by the clock at the `fps=<n>` of `<key>.txt`, or twelve a
// second. Every frame is decoded when the first is, so showing the next is a
// copy into the image the texture already has (Due).
//
// A texture may also be switchable: the title names one image for something
// the port has several pictures of (MW3's Gold camouflage, which the custom
// camouflages are drawn through, mw3_custom_camos.cpp). Its image is always
// made as a replacement's, of the title's own picture until one is chosen,
// and a choice reaches it as an animation's next frame does.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include "texture.h"

namespace gpu::replace
{
    // The frame of a replacement that has only the one picture.
    constexpr uint32_t kStill = ~uint32_t(0);

    // Whether there is anything to do: replacements were found, or textures
    // are being dumped. A run with neither never hashes a texture.
    bool On();

    // Makes the texture of `key` switchable, as one of `group`. Before the
    // texture is first read: an image already made stays as it was made.
    void Switchable(uint32_t group, uint64_t key);
    // How one still picture of a switchable texture moves: frames made from
    // it when it is chosen, shown in turn as a file animation's are. Scroll
    // slides it across itself diagonally (the look of a scrolling camouflage
    // material, as Sloth's Animated Camos Mod gives Gold on PC), Shimmer runs
    // a band of light over it, and Hue turns its colours round the wheel.
    // Each loops without a jump over kEffectFrames frames.
    enum class Effect : uint8_t { None, Scroll, Shimmer, Hue };
    constexpr uint32_t kEffectFrames = 48;

    // What the textures of `group` show from now on: the frames in order, at
    // `fps` when there are several, each made the size of the title's own
    // picture; or with no frames, the folder's replacement where there is one
    // and the title's own picture where there is not. One frame with an
    // effect is kEffectFrames of them at `fps`. Decodes on the calling
    // thread. Callable from any thread.
    void Select(uint32_t group, const std::vector<std::filesystem::path>& frames, double fps,
                Effect effect = Effect::None);

    // The level the key is taken over: the lowest the sampler may use, which
    // is the base unless the title has yet to stream the base in. A streamed
    // texture so has a key for each state it is drawn in.
    uint32_t KeyedLevel(const TextureData& data);

    // The key of a texture as ReadTexture returned it: that level's bytes,
    // with the format, the level's size and how many levels there are from it
    // down, so the same bytes in another shape are another texture. Never
    // zero for a texture that was read; zero for one that was not.
    //
    // FROZEN: every replacement anyone has made is named by this. Changing
    // what goes into it, or how, orphans them all.
    uint64_t KeyOf(const TextureData& data, uint32_t format);

    // The replacement for `key`, decoded: RGBA8 in the host layout of the
    // original's format, with a full chain of levels, as a texture of format
    // 8_8_8_8 that needs no further reading. `gamma` says the original is
    // linearised when sampled, so the levels are averaged in linear light.
    // Null when there is none, when its file would not decode, or when
    // `original` is a cube, a volume or an array, which are not replaced.
    //
    // For an animation that is the frame the clock is at, and `frame` receives
    // its number; kStill otherwise.
    //
    // Decoded on first use and kept, the least recently used going once the
    // decoded pictures pass a bound; an animation's frames are decoded
    // together, as many as the bound has room for. Callable from any thread.
    std::shared_ptr<const TextureData> Load(uint64_t key, const TextureData& original,
                                            const TextureFetch& fetch, bool gamma,
                                            uint32_t* frame = nullptr);

    // The frame the animation `key` is at now, when that is not `shown`, which
    // then becomes it; null when the image need not change. Never reads a
    // file: frames that have been let go stay gone until Load is next asked.
    // The same size and layout as the frame Load gave. Callable from any
    // thread.
    std::shared_ptr<const TextureData> Due(uint64_t key, uint32_t& shown);

    // The caller could not use what Load gave it: counted as failed, logged
    // with `why`, and not offered again.
    void Refuse(uint64_t key, const char* why);
    // An image was made from a replacement.
    void Applied();

    // MW2_DUMP_TEXTURES: writes the keyed level as `<key>.png` and its line in
    // `textures.txt`, the first time a key is seen. A texture nothing has
    // written yet -- all zero -- is left for when it has been. Callable from
    // any thread.
    void Dump(uint64_t key, const TextureData& data, const TextureFetch& fetch);

    // The end-of-run line: replacements indexed, how many of them animated,
    // applied and failed, and how many times an animation moved on a frame.
    void Report();
}

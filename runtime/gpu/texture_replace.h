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
#include <cstdint>
#include <memory>

#include "texture.h"

namespace gpu::replace
{
    // Whether there is anything to do: replacements were found, or textures
    // are being dumped. A run with neither never hashes a texture.
    bool On();

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
    // Decoded on first use and kept, the least recently used going once the
    // decoded pictures pass a bound. Callable from any thread.
    std::shared_ptr<const TextureData> Load(uint64_t key, const TextureData& original,
                                            const TextureFetch& fetch, bool gamma);

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

    // The end-of-run line: replacements indexed, applied and failed.
    void Report();
}

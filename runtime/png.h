#pragma once
// PNG, as far as the texture dump and the texture replacements need it
// (gpu/texture_replace.h): pictures a player's image editor reads and writes,
// alpha included, which PNM has no room for.
//
// Reading takes what editors save: 8 and 16 bits a channel (the high byte is
// kept), grey, grey with alpha, RGB, RGBA and palettes with their
// transparency, at any bit depth the format allows. Interlaced files are
// refused. Chunk checksums are not verified: a damaged file fails to inflate
// or comes out damaged, and neither is worse than refusing it.
//
// Writing stores the pixels uncompressed. No compressor is linked into every
// build of the runtime, a dump is opened once and saved again by an editor
// that does compress, and a stored file is written as fast as the disk takes
// it, which matters to a run that dumps thousands.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace png
{
    // `file` is the whole file. `rgba` comes out four bytes a pixel, rows top
    // to bottom. False with `error` set to a phrase for the log.
    bool Read(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba, uint32_t& width,
              uint32_t& height, const char*& error);

    // `rgba` is `width * height` pixels of four bytes, rows top to bottom.
    bool Write(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
               uint32_t height);
}

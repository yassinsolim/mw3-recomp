#pragma once
// The numbers Advanced Warfare's Xbox 360 multiplayer (title update 17) gives
// the built-in functions and methods the port's own scripts call
// (aw_zombie_bots.cpp). gsc-tool has S1's Xbox 360 instructions and none of
// these; the executable has no names for them either.
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace awgsc
{
    inline constexpr std::array<std::pair<uint16_t, std::string_view>, 0> kFunctions{ {} };
    inline constexpr std::array<std::pair<uint16_t, std::string_view>, 0> kMethods{ {} };
}

#pragma once
#include <cstdint>

// What a host may change about its matches in Modern Warfare 3's multiplayer
// (match_options.cpp): gravity, movement speed, jump height, unlimited sprint
// and fall damage, plus the private match menus the disc hides.
namespace match_options
{
    // A console line the runtime answers itself instead of the title: `match`,
    // `match gravity <n>`, `match speed <percent>`, `match jump <n>`,
    // `match sprint <0|1>`, `match falldamage <0|1>`. True when the line was one
    // of them; the host's server acts on it on its next frame. Any thread.
    bool ConsoleLine(const char* text);

    // The client's set-dvar server command: true when the name is the one a
    // host of this port sends its jump height under, which no dvar has.
    bool FromHost(const char* name, const char* value);

    // Once a frame, from the title's main thread.
    void Frame(uint8_t* base);
}

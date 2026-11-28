#pragma once
// The numbers Advanced Warfare's Xbox 360 multiplayer (title update 17) gives
// the built-in functions and methods, and the names, that the port's own
// script calls (aw_zombie_bots.cpp). gsc-tool has S1's Xbox 360 instructions
// and none of these; the executable has no names for them either.
//
// How each was found: the game's own scripts of an Exo Zombies match, written
// out by MW2_SCRIPT_DUMP (docs/mods.md), laid beside the same files of the PC
// game as the public S1 dumps name them, function by function and token by
// token. Every number below came out the same each time it was met, never
// with a second name (the count is how many times). The names are the PC's
// gsc-tool names but one: 0x8132 is called setweaponammostock there, and the
// game's hasfullammo() compares what it returns with weaponmaxammo(), so it
// is the getter.
//
// Only what the bots' script needs is here: a name missing from these lists
// is compiled as a string, which this build reads as a different name.
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace awgsc
{
    inline constexpr std::array<std::pair<uint16_t, std::string_view>, 11> kFunctions{ {
        { 0x01C, "getdvar" },                   // 7
        { 0x020, "gettime" },                   // 302
        { 0x046, "int" },                       // 109
        { 0x056, "distance" },                  // 27
        { 0x058, "distancesquared" },           // 143
        { 0x0A5, "weaponmaxammo" },             // 3
        { 0x0EA, "getentarray" },               // 73
        { 0x0ED, "spawnstruct" },               // 72
        { 0x0EF, "isalive" },                   // 58
        { 0x161, "getnodesinradiussorted" },    // 8
        { 0x1F7, "isbot" },                     // 17
    } };

    inline constexpr std::array<std::pair<uint16_t, std::string_view>, 8> kMethods{ {
        { 0x80C9, "iprintlnbold" },             // 18
        { 0x80F8, "getcurrentweapon" },         // 21
        { 0x80F9, "getcurrentprimaryweapon" },  // 14
        { 0x8132, "getweaponammostock" },       // 14
        { 0x8145, "getweaponslistprimaries" },  // 17
        { 0x8350, "botsetscriptgoal" },         // 5
        { 0x8369, "botlookatpoint" },           // 10
        { 0x837A, "botpressbutton" },           // 17
    } };

    // Names of functions and fields that the game's scripts refer to by
    // number. main and init are the two a script's entry is looked up by
    // (mods.cpp; the game type's main is 0x26F in GScr_LoadGameTypeScript,
    // title.h).
    inline constexpr std::array<std::pair<uint32_t, std::string_view>, 18> kTokens{ {
        { 0x01EB, "health" },                       // 62
        { 0x020D, "init" },                         // 68
        { 0x0251, "laststand" },                    // 11
        { 0x026F, "main" },                         // 58
        { 0x0279, "maxhealth" },                    // 32
        { 0x02AF, "name" },                         // 27
        { 0x02EA, "origin" },                       // 1235
        { 0x032C, "players" },                      // 225
        { 0x0A98, "action_thread" },                // 13
        { 0x18F4, "bot_abort_tactical_goal" },      // 2
        { 0x1941, "bot_defend_player_guarding" },   // 23
        { 0x1942, "bot_defend_stop" },              // 4
        { 0x19FC, "bot_new_tactical_goal" },        // 7
        { 0x342D, "end_thread" },                   // 7
        { 0x64DF, "object" },                       // 85
        { 0x7BA5, "script_goal_radius" },           // 12
        { 0x85CE, "should_abort" },                 // 11
        { 0xA400, "weaponname" },                   // 18
    } };
}

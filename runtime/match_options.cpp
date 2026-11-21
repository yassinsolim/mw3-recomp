// Match options for Modern Warfare 3's multiplayer: what a host can change
// about the matches it hosts, beyond the rules Game Setup already offers.
//
// The private match menus the disc hides. Game Setup's Mode menu has an
// Alternate Modes entry (Drop Zone, Team Juggernaut, Juggernaut, Gun Game,
// Infected, One in the Chamber) and its Options menu two class menus, each
// shown by an integer dvar the executable registers at 0; the console turned
// them on from its playlist data after release. The disc's files carry all of
// it -- the six recipes (mp/recipes/*.recipe in common_mp.ff), scripts and
// menus -- so the dvars are set here, once they exist, for everybody.
//
// IW5 dropped the dvars the mods of the earlier games changed movement with
// (g_gravity, g_speed, jump_height, player_sprintUnlimited): the numbers are
// in the code. Each option goes where the title keeps the number:
//
//   gravity    ClientEndFrame stores 800 in the player state every server
//              frame; the hook stores the host's after it.
//   speed      the player state's moveSpeedScaleMultiplier, which the same
//              function copies from what the scripts' setMoveSpeedScale left
//              (a Juggernaut's 0.65 and the like): the hook stores that times
//              the host's.
//   sprint     the sprint time left is the whole time for a player with
//              specialty_marathon, which no MW3 class has; the hook sets its
//              bit in the player state's perks.
//   fall       G_Damage is not called for MOD_FALLING.
//   jump       the height is one constant the jump code loads from the image;
//              it is rewritten there.
//
// The first three are fields of the player state, which the server sends and a
// client predicts with, so they are right for every player, a console's
// included; they run on the server only, which is the host. The jump height is
// not sent by the title. This port's host sends it to every player as a
// set-dvar server command under a name no dvar has, and this port's client
// (command_dvars_from_host.cpp) takes it from there, so both sides jump alike;
// it goes back to 39 when the client is in nobody's match. A console in such a
// match predicts 39 and is corrected by the server's next snapshot.
//
// MW2_CUT_MODE names a game mode whose script is on the disc with no recipe
// and no menu entry (arena, oneflag, gtnw): the menu's StartServer sets
// g_gametype from ui_gametype, and the hook hands it the cut mode there. They
// were left unfinished, and the lobby still names the mode Game Setup chose.
// ctfpro and vip, the other two on the disc, do not start.
//
// The settings come from MW2_MATCH_* and the console's `match` line; only
// numbers are stored, and the server reads them on its next frame.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "match_options.h"
#include "console.h"
#include "diagnostics.h"
#include "guest.h"
#include "env.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#ifdef T_ClientEndFrame

namespace
{
    constexpr int kSlots = 18;                  // the most clients a server has
    constexpr int kGravity = 800, kSpeed = 100, kJump = 39;     // the title's
    constexpr const char* kJumpName = "mw3_jump_height";
    constexpr uint32_t kNoEntity = 0x7FF;
    constexpr uint32_t kConnected = 5;          // the client's connection state from which it is in a server

    struct Options
    {
        std::atomic<int> gravity{ kGravity };   // units/s^2
        std::atomic<int> speed{ kSpeed };       // percent
        std::atomic<int> jump{ kJump };         // units
        std::atomic<bool> sprint{ false };      // unlimited
        std::atomic<bool> fallDamage{ true };
        // 0 until something was asked for, and then another number at every change.
        std::atomic<uint32_t> serial{ 0 };
        const char* cutMode = nullptr;

        void Say(const char* why) const
        {
            LOGI("match: %s: gravity %d, speed %d%%, jump height %d, sprint %s, fall damage %s", why, gravity.load(),
                 speed.load(), jump.load(), sprint.load() ? "unlimited" : "as the title's", fallDamage.load() ? "on" : "off");
        }

        Options()
        {
            gravity = Clamped("MW2_MATCH_GRAVITY", kGravity, 50, 3000);
            speed = Clamped("MW2_MATCH_SPEED", kSpeed, 25, 400);
            jump = Clamped("MW2_MATCH_JUMP", kJump, 10, 1000);
            sprint = env::Flag("MW2_MATCH_SPRINT");
            fallDamage = env::Number("MW2_MATCH_FALL_DAMAGE", 1) != 0;
            if (gravity != kGravity || speed != kSpeed || jump != kJump || sprint || !fallDamage) {
                serial = 1;
                Say("in the matches this copy hosts");
            }
            if (const char* mode = env::Text("MW2_CUT_MODE")) {
                for (const char* known : { "arena", "oneflag", "gtnw" })
                    if (!strcasecmp(mode, known)) cutMode = known;
                if (cutMode) LOGI("match: the matches this copy starts from its menu are %s, a mode the disc left unfinished", cutMode);
                else LOGW("match: MW2_CUT_MODE is arena, oneflag or gtnw, not %s", mode);
            }
        }

        static int Clamped(const char* name, int fallback, int least, int most)
        {
            const int asked = int(env::Number(name, uint64_t(fallback)));
            const int value = std::clamp(asked, least, most);
            if (value != asked) LOGW("match: %s is %d to %d, not %d", name, least, most, asked);
            return value;
        }
    };
    Options& Wanted()
    {
        static Options options;
        return options;
    }

    float Real(uint8_t* base, uint32_t address)
    {
        const uint32_t bits = PPC_LOAD_U32(address);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    void PutReal(uint8_t* base, uint32_t address, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        PPC_STORE_U32(address, bits);
    }

    // The jump code's constant. Written only when it differs: the server stores
    // it for every player every frame.
    void SetJump(uint8_t* base, int height)
    {
        if (Real(base, T_DATA_JumpHeight) != float(height)) PutReal(base, T_DATA_JumpHeight, float(height));
    }

    // Server thread only.
    struct Told { uint32_t server = 0, gamestate = 0, serial = 0; };
    Told g_told[kSlots];
    uint32_t g_announced = 0;       // the server id the log last named the options for

    // Sends a player the jump height once for each gamestate it was given and
    // again when the height changes. A bot's address type is 0; nothing is
    // sent to one.
    void TellJump(PPCContext& ctx, uint8_t* base, uint32_t slot, uint32_t serial, int height)
    {
        const uint32_t client = PPC_LOAD_U32(T_DATA_Svs + T_SVS_CLIENTS) + slot * T_SVCLIENT_SIZE;
        if (PPC_LOAD_U32(client) != 5 || PPC_LOAD_U32(client + T_SVCLIENT_ADDRESS) == 0) return;

        const Told now{ PPC_LOAD_U32(T_DATA_ServerId), PPC_LOAD_U32(client + T_SVCLIENT_GAMESTATE_MSG), serial };
        Told& told = g_told[slot];
        if (told.server == now.server && told.gamestate == now.gamestate && told.serial == now.serial) return;
        told = now;

        // The text goes below the stack pointer, and the call's own frame below
        // that, as console.cpp does for a command line.
        char line[64];
        const int length = std::snprintf(line, sizeof line, "q %s \"%d\"", kJumpName, height);
        const PPCContext saved = ctx;
        const uint32_t text = (ctx.r1.u32 - 0x100) & ~0xFu;
        std::memcpy(base + text, line, size_t(length) + 1);
        ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFull;
        ctx.r3.u64 = slot;
        ctx.r4.u64 = 1;             // reliable
        ctx.r5.u64 = text;
        GUEST_FUNC(T_SV_GameSendServerCommand)(ctx, base);
        ctx = saved;
    }

    // MW2_TRACE_MATCH: how high, how long and how fast each player's jump was,
    // when it lands -- what gravity, speed and jump height come to, in the
    // title's units and the server's time -- and where it landed, which is
    // how a test with a real mouse knows where it stands and where to aim.
    struct Leap { bool up = false; float from = 0, peak = 0, speed = 0; int32_t since = 0; };
    Leap g_leaps[kSlots];

    void TraceLeap(uint8_t* base, uint32_t slot, uint32_t state)
    {
        static const bool traced = diag::Flag("MW2_TRACE_MATCH");
        if (!traced) return;
        Leap& leap = g_leaps[slot];
        const bool airborne = PPC_LOAD_U32(state + T_PS_GROUND_ENTITY) == kNoEntity;
        const float height = Real(base, state + T_PS_ORIGIN + 8);
        const int32_t time = int32_t(PPC_LOAD_U32(T_DATA_Svs + T_SVS_TIME));
        if (airborne && !leap.up) {
            const float x = Real(base, state + T_PS_VELOCITY), y = Real(base, state + T_PS_VELOCITY + 4);
            leap = { true, height, height, std::sqrt(x * x + y * y), time };
        } else if (airborne) {
            leap.peak = std::max(leap.peak, height);
        } else if (leap.up) {
            leap.up = false;
            LOGI("match: %s was in the air %d ms and rose %.0f units, moving at %.0f a second, facing %.1f and looking %.1f down, at %.0f %.0f %.0f",
                 GuestPtr<const char>(state + T_GCLIENT_NAME), time - leap.since, leap.peak - leap.from, leap.speed,
                 Real(base, state + T_PS_VIEW_ANGLES + 4), Real(base, state + T_PS_VIEW_ANGLES),
                 Real(base, state + T_PS_ORIGIN), Real(base, state + T_PS_ORIGIN + 4), height);
        }
    }
}

GUEST_HOOK(T_ClientEndFrame)
{
    const uint32_t entity = ctx.r3.u32;
    GUEST_ORIG(T_ClientEndFrame)(ctx, base);

    const uint32_t state = PPC_LOAD_U32(entity + T_GENTITY_CLIENT);
    if (!state) return;
    const uint32_t slot = PPC_LOAD_U32(entity);
    if (slot >= kSlots) return;
    TraceLeap(base, slot, state);

    Options& options = Wanted();
    const uint32_t serial = options.serial;
    if (!serial) return;            // nothing was asked for: the match is the title's

    if (const uint32_t server = PPC_LOAD_U32(T_DATA_ServerId); server != g_announced) {
        g_announced = server;
        options.Say("hosting a level");
    }

    PPC_STORE_U32(state + T_PS_GRAVITY, uint32_t(options.gravity.load()));
    if (const int speed = options.speed; speed != kSpeed)
        PutReal(base, state + T_PS_SPEED_SCALE, Real(base, state + T_GCLIENT_SPEED_SCALE) * float(speed) / 100.0f);
    // Turned off, the bit stays until the scripts clear the player's perks at
    // the next spawn: whether a script set it cannot be told from here.
    if (options.sprint) PPC_STORE_U32(state + T_PS_PERKS, PPC_LOAD_U32(state + T_PS_PERKS) | T_PERK_MARATHON);

    const int jump = options.jump;
    SetJump(base, jump);
    TellJump(ctx, base, slot, serial, jump);
}

GUEST_HOOK(T_G_Damage)
{
    if (ctx.r10.u32 == T_MOD_FALLING && !Wanted().fallDamage) return;
    GUEST_ORIG(T_G_Damage)(ctx, base);
}

GUEST_HOOK(T_Dvar_SetStringByName)
{
    const char* mode = Wanted().cutMode;
    if (mode && ctx.lr == T_RET_StartServerGametype) {
        // Dvar_SetString copies the value, so it only has to last the call.
        const PPCContext saved = ctx;
        const uint32_t text = (ctx.r1.u32 - 0x100) & ~0xFu;
        std::memcpy(base + text, mode, std::strlen(mode) + 1);
        ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFull;
        ctx.r4.u64 = text;
        LOGI("match: starting %s in place of %s", mode, reinterpret_cast<const char*>(base + saved.r4.u32));
        GUEST_ORIG(T_Dvar_SetStringByName)(ctx, base);
        ctx = saved;
        return;
    }
    GUEST_ORIG(T_Dvar_SetStringByName)(ctx, base);
}

bool match_options::ConsoleLine(const char* text)
{
    while (*text == ' ' || *text == '\t') ++text;
    if (strncasecmp(text, "match", 5) != 0) return false;
    const char* rest = text + 5;
    if (*rest && *rest != ' ' && *rest != '\t' && *rest != '\n' && *rest != '\r') return false;
    while (*rest == ' ' || *rest == '\t') ++rest;

    Options& options = Wanted();
    const auto value = [&rest](size_t skip) { return int(std::strtol(rest + skip, nullptr, 10)); };
    bool changed = true;
    if (!strncasecmp(rest, "gravity", 7)) options.gravity = std::clamp(value(7), 50, 3000);
    else if (!strncasecmp(rest, "speed", 5)) options.speed = std::clamp(value(5), 25, 400);
    else if (!strncasecmp(rest, "jump", 4)) options.jump = std::clamp(value(4), 10, 1000);
    else if (!strncasecmp(rest, "sprint", 6)) options.sprint = value(6) != 0;
    else if (!strncasecmp(rest, "falldamage", 10)) options.fallDamage = value(10) != 0;
    else changed = false;
    if (changed) ++options.serial;
    options.Say(changed ? "changed" : "now");
    return true;
}

bool match_options::FromHost(const char* name, const char* value)
{
    if (strcasecmp(name, kJumpName) != 0) return false;
    const int height = std::clamp(int(std::strtol(value, nullptr, 10)), 10, 1000);
    uint8_t* base = guest::Base();
    if (Real(base, T_DATA_JumpHeight) != float(height)) LOGI("match: the host's jump height is %d", height);
    SetJump(base, height);
    return true;
}

void match_options::Frame(uint8_t* base)
{
    // The hidden menus, once: the dvars are registered in one function, and
    // the last of them is there when all are.
    static bool shown = false;
    if (!shown && PPC_LOAD_U32(T_DATA_LastGameSetupDvar)) {
        shown = true;
        for (const char* name : { "pm_gamesetup_mode_altmodes", "pm_gamesetup_mode_altmodes_dropzone",
                                  "pm_gamesetup_mode_altmodes_teamjug", "pm_gamesetup_mode_altmodes_jug",
                                  "pm_gamesetup_mode_altmodes_gungame", "pm_gamesetup_mode_altmodes_infected",
                                  "pm_gamesetup_mode_altmodes_oneinthechamber", "pm_gamesetup_options_createdefaultclass",
                                  "pm_gamesetup_options_customclassrestrictions" }) {
            char line[96];
            std::snprintf(line, sizeof line, "set %s 1", name);
            console::RunNow(line);
        }
        LOGI("match: Game Setup shows its Alternate Modes and class options");
    }

    // In nobody's match, the jump is the title's again: the height a host sent
    // was that host's, and this copy's own is stored by its server when it runs.
    if (PPC_LOAD_U32(T_DATA_ClientConnState) < kConnected) SetJump(base, kJump);
}

#endif

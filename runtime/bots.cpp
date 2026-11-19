// Bots for Modern Warfare 3's multiplayer: MW2_BOTS=<n> keeps n of them in
// every match this copy hosts.
//
// The executable still has the server half of IW's test clients (title.h, "Bots"):
// a client whose address type is 0 is exempt from the challenge and the
// registration check in SV_DirectConnect, nothing addressed to it reaches a
// socket, and SV_BotUserMoves thinks for it every server frame. The function
// that put one there, SV_AddTestClient, was compiled out. This file is that
// function again, and a replacement for the stock think, which spins on the
// spot and fires at random.
//
// Adding a bot:
//   1. `connect bot<n> "<userinfo>"` goes into the server's argv and
//      SV_DirectConnect is called with {type 0, IP 0, port n}. The userinfo
//      carries the protocol, fastfile checksum and stats version read from the
//      title's own functions, so the checks a player's connect passes are
//      passed and not skipped. The port is different for every bot: the
//      function's first loop takes an equal address and port for a reconnect
//      and ignores it for sv_reconnectlimit seconds.
//   2. A player the party does not know gets the first free slot at or above a
//      dvar's value. The value is replaced for the call so the bot lands in the
//      highest free slot: a party member's slot is its index in the party,
//      counted from 0, and SV_DirectConnect throws out whoever sits in it.
//   3. The slot is at state 3, waiting for the client to ask for the gamestate
//      and send "begin". SV_SendClientGameState would drop a client that sent
//      no stats (EXE_NEEDSTATS), so its three stores are made here and
//      SV_ClientEnterWorld is called directly.
//   4. The stock scripts leave a new player in front of the team menu. The bot
//      answers it and the class menu as a client does, with the `mr <server id>
//      <menu> <response>` client command; the menu's number is its place in the
//      configstrings, read at run time.
//
// The think (every server frame, 50 ms): the nearest enemy inside the view cone
// that G_SightTrace reaches becomes the target; the bot turns to it at a capped
// rate with an error that shrinks while it tracks, fires in bursts once its
// reaction time has passed and the aim is close, strafes and keeps its
// distance. With no target it walks towards the nearest enemy, feeling out eight
// directions with sight traces three times a second and taking the best, and
// tries a jump and another way when it has made no progress. Multiplayer maps
// carry no path data, so that is all the route finding there is.
//
// Everything runs on the server's thread inside the two hooks. A console line
// or a setting only stores a number, which the next server frame reads.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "bots.h"
#include "diagnostics.h"
#include "env.h"
#include "log.h"
#include "map_vote.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <strings.h>

#ifdef T_SV_BotUserMoves

namespace
{
    constexpr int kSlots = 18;                 // the most clients a server has
    constexpr int kMostBots = kSlots - 1;      // the host keeps a slot
    constexpr int32_t kSettle = 2000;          // ms of a new level before a bot joins: its scripts must be running
    constexpr int32_t kFrame = 50;             // ms, the server's frame

    // usercmd: +0 server time, +4 buttons, +8 pitch, +0xC yaw (16-bit angles,
    // added to the player state's delta_angles), +0x14 weapon, +0x1C forward,
    // +0x1D right. The first three buttons are the ones the stock think presses;
    // the jump was found by pressing it (a bot leaves the ground at 209 units/s).
    constexpr uint32_t kCmdSize = 0x2C;
    constexpr uint32_t kButtonAttack = 0x1;
    constexpr uint32_t kButtonUseReload = 0x20;
    constexpr uint32_t kButtonCrouch = 0x200;
    constexpr uint32_t kButtonJump = 0x400;

    // What the script's sighttracepassed passes when characters do not block.
    constexpr uint32_t kSightMask = 0x00801803;
    constexpr uint32_t kNoEntity = 0x7FF;

    constexpr uint32_t kSessionPlaying = 0;    // gclient +0x3300, the session state
    constexpr uint32_t kTeamAxis = 1, kTeamAllies = 2;

    constexpr float kPi = 3.14159265f;

    struct Skill
    {
        const char* name;
        int32_t reaction;       // ms from seeing a target to the first shot
        float error;            // degrees of aim error when a target is new
        float turn;             // degrees a second
        float cone;             // degrees of view a new target must be inside
        int32_t memory;         // ms a target out of sight is still gone after
        int32_t pause;          // ms between bursts
        bool strafes, crouches;
    };
    constexpr Skill kSkills[4] = {
        { "Recruit",  600, 5.0f, 180.0f,  90.0f, 1000, 500, false, false },
        { "Regular",  400, 3.0f, 270.0f, 120.0f, 2000, 350, true,  false },
        { "Hardened", 250, 1.5f, 400.0f, 150.0f, 3000, 250, true,  true  },
        { "Veteran",  120, 0.5f, 700.0f, 180.0f, 4000, 150, true,  true  },
    };

    const char* const kNames[kSlots] = {
        "Bot Atlas", "Bot Birch", "Bot Cobalt", "Bot Dune", "Bot Ember", "Bot Flint",
        "Bot Gale", "Bot Harbor", "Bot Indigo", "Bot Juniper", "Bot Kestrel", "Bot Lumen",
        "Bot Marrow", "Bot Nettle", "Bot Onyx", "Bot Pike", "Bot Quartz", "Bot Rook",
    };

    struct Settings
    {
        std::atomic<int> count;
        std::atomic<int> skill;
        bool against;           // every bot on the side the players are not on

        Settings()
        {
            count = int(std::min<uint64_t>(env::Number("MW2_BOTS"), kMostBots));
            skill = int(std::min<uint64_t>(env::Number("MW2_BOT_SKILL", 1), 3));
            const char* teams = env::Text("MW2_BOT_TEAMS");
            against = teams && (!strcasecmp(teams, "against") || !strcmp(teams, "1"));
            if (count > 0)
                LOGI("bots: MW2_BOTS: %d in the matches this copy hosts, %s, %s", count.load(),
                     kSkills[skill].name, against ? "against the players" : "filling both teams");
        }
    };
    Settings& Wanted()
    {
        static Settings settings;
        return settings;
    }

    bool Traced()
    {
        static const bool on = diag::Flag("MW2_TRACE_BOTS");
        return on;
    }

    struct Vec { float x = 0, y = 0, z = 0; };

    float Wrap(float degrees)
    {
        degrees = std::fmod(degrees, 360.0f);
        if (degrees > 180.0f) degrees -= 360.0f;
        if (degrees < -180.0f) degrees += 360.0f;
        return degrees;
    }

    constexpr int kTrail = 8;

    struct Bot
    {
        uint16_t port = 0;          // which bot holds the slot: its address's port
        bool spawned = false;       // has been alive since it joined
        bool wasAlive = false;
        int32_t nextMenu = 0, downSince = 0;
        int menuStep = 0;

        bool aimSet = false;
        float yaw = 0, pitch = 0;   // where it looks, which its view follows

        int target = -1;
        bool visible = false;
        int32_t firstSeen = 0, lastSeen = 0, nextLook = 0;
        Vec lastKnown;
        float errorYaw = 0, errorPitch = 0;
        int32_t nextError = 0;
        int32_t burstUntil = 0, pauseUntil = 0, lastShot = 0;
        int strafe = 1;
        int32_t nextStrafe = 0, crouchUntil = 0;
        bool reload = false;

        float heading = 0;          // where it wants to go with no target
        float course = 0;           // where it walks: the heading, as far as the walls allow
        int32_t courseUntil = 0;    // a course it was given for being stuck holds until then
        int32_t nextHeading = 0;
        Vec trail[kTrail];          // where it has been, one every 1.5 s
        int trailCount = 0, trailNext = 0;
        int32_t nextCrumb = 0;
        Vec progressFrom;
        int32_t nextProgress = 0;
        int jump = 0;
    };

    // Server thread only.
    Bot g_bots[kSlots];
    bool g_engaged = false;         // a bot was asked for at some point in this run
    uint32_t g_serverId = 0;
    int32_t g_levelStart = 0, g_lastTime = 0, g_nextAdd = 0;
    uint16_t g_nextPort = 1;
    int g_teamMenu = -1, g_classMenu = -1;
    bool g_menusRead = false;
    std::minstd_rand g_random{ 0x5EED };

    int Random(int low, int high)
    {
        return low + int(g_random() % uint32_t(high - low + 1));
    }
    float Random(float low, float high)
    {
        return low + (high - low) * float(g_random() % 10001) / 10000.0f;
    }

    // One server frame's view of the title, with a guest stack frame of its own
    // below the caller's for the calls it makes and the buffers they take.
    struct Server
    {
        PPCContext& ctx;
        uint8_t* base;
        uint32_t text, cmd, start, end, hit;    // guest scratch
        int32_t time = 0;
        int count = 0;
        uint32_t clients = 0;

        Server(PPCContext& context, uint8_t* memory) : ctx(context), base(memory)
        {
            // A callee saves its arguments just above the stack pointer it is
            // given, so the buffers start well clear of it.
            const uint32_t sp = (ctx.r1.u32 - 0x1800) & ~0xFu;
            ctx.r1.u64 = sp;
            text = sp + 0x200;      // 0x800 bytes
            cmd = sp + 0xA00;
            start = sp + 0xA40;
            end = sp + 0xA50;
            hit = sp + 0xA60;
        }

        uint32_t U32(uint32_t address) const { return PPC_LOAD_U32(address); }
        void Put32(uint32_t address, uint32_t value) const { PPC_STORE_U32(address, value); }
        float Real(uint32_t address) const
        {
            const uint32_t bits = U32(address);
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }
        void PutReal(uint32_t address, float value) const
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            Put32(address, bits);
        }
        Vec Vector(uint32_t address) const { return { Real(address), Real(address + 4), Real(address + 8) }; }
        void PutVector(uint32_t address, const Vec& v) const
        {
            PutReal(address, v.x); PutReal(address + 4, v.y); PutReal(address + 8, v.z);
        }
        uint32_t PutText(const char* value) const
        {
            std::memcpy(base + text, value, std::strlen(value) + 1);
            return text;
        }

        uint32_t Client(int slot) const { return clients + uint32_t(slot) * T_SVCLIENT_SIZE; }
        uint32_t State(int slot) const { return U32(Client(slot)); }
        bool IsBot(int slot) const { return U32(Client(slot) + T_SVCLIENT_ADDRESS) == 0; }
        uint16_t Port(int slot) const { return PPC_LOAD_U16(Client(slot) + T_SVCLIENT_ADDRESS + 8); }
        uint32_t Entity(int slot) const { return U32(Client(slot) + T_SVCLIENT_GENTITY); }
        uint32_t GameClient(int slot) const
        {
            const uint32_t entity = Entity(slot);
            return entity ? U32(entity + T_GENTITY_CLIENT) : 0;
        }
        const char* Name(int slot) const
        {
            const uint32_t client = GameClient(slot);
            return client ? reinterpret_cast<const char*>(base + client + T_GCLIENT_NAME) : "?";
        }
        // In the match and on its feet: not a spectator, dead, or watching a killcam.
        bool Alive(int slot) const
        {
            if (State(slot) != 5) return false;
            const uint32_t entity = Entity(slot);
            const uint32_t client = GameClient(slot);
            return client && int32_t(U32(entity + T_GENTITY_HEALTH)) > 0
                && U32(client + T_GCLIENT_SESSION_STATE) == kSessionPlaying
                && U32(client + T_GCLIENT_ARCHIVE_TIME) == 0;
        }

        bool Read()
        {
            time = int32_t(U32(T_DATA_Svs + T_SVS_TIME));
            count = std::min(int(U32(T_DATA_Svs + T_SVS_CLIENT_COUNT)), kSlots);
            clients = U32(T_DATA_Svs + T_SVS_CLIENTS);
            return clients && count > 0;
        }

        uint32_t Call0(PPCFunc* function)
        {
            function(ctx, base);
            return ctx.r3.u32;
        }

        bool SightClear(const Vec& from, const Vec& to, uint32_t pass0, uint32_t pass1)
        {
            PutVector(start, from);
            PutVector(end, to);
            Put32(hit, 0);
            ctx.r3.u64 = hit;
            ctx.r4.u64 = start;
            ctx.r5.u64 = end;
            ctx.r6.u64 = pass0;
            ctx.r7.u64 = pass1;
            ctx.r8.u64 = kSightMask;
            GUEST_FUNC(T_G_SightTrace)(ctx, base);
            return U32(hit) == 0;
        }

        void ClientCommand(int slot, const char* line)
        {
            ctx.r3.u64 = Client(slot);
            ctx.r4.u64 = PutText(line);
            ctx.r5.u64 = 1;
            ctx.r6.u64 = 0;
            GUEST_FUNC(T_SV_ExecuteClientCommand)(ctx, base);
        }

        // Out of the server at once: a dropped client waits as a zombie so its
        // last packets can be resent, and a bot has none.
        void Drop(int slot, const char* why)
        {
            LOGI("bots: %s leaves slot %d (%s)", Name(slot), slot, why);
            ctx.r3.u64 = Client(slot);
            ctx.r4.u64 = PutText("EXE_DISCONNECTED");
            ctx.r5.u64 = 1;
            GUEST_FUNC(T_SV_DropClient)(ctx, base);
            if (State(slot) == 1) Put32(Client(slot), 0);
            g_bots[slot] = {};
            for (Bot& other : g_bots)
                if (other.target == slot) other.target = -1;
        }

        // The menus' names are configstrings, and `mr` takes a menu by its place
        // among them.
        void ReadMenus()
        {
            g_menusRead = true;
            g_teamMenu = g_classMenu = -1;
            for (int i = 0; i < 32; ++i) {
                ctx.r3.u64 = T_CS_MENUS + uint32_t(i);
                ctx.r4.u64 = text;
                ctx.r5.u64 = 0x100;
                GUEST_FUNC(T_SV_GetConfigstring)(ctx, base);
                const char* name = reinterpret_cast<const char*>(base + text);
                if (!*name) continue;
                if (Traced()) LOGI("bots: menu %d is \"%s\"", i, name);
                if (g_teamMenu < 0 && !strncasecmp(name, "team_", 5)) g_teamMenu = i;
                if (!strcasecmp(name, "changeclass")) g_classMenu = i;
            }
            if (g_teamMenu < 0 || g_classMenu < 0)
                LOGW("bots: this game type has no %s menu; a bot cannot choose it",
                     g_teamMenu < 0 ? "team" : "class");
        }

        void Join(int slot);
        void Enter(int slot);
        void Menus(int slot, Bot& bot);
        void Think(int slot, Bot& bot);
        void Fight(int slot, Bot& bot, const Skill& skill, const Vec& eye, uint32_t& buttons, int& forward, int& right);
        void Wander(int slot, Bot& bot, const Vec& origin, uint32_t& buttons, int& forward);
        int FindTarget(int slot, Bot& bot, const Skill& skill, const Vec& eye, uint32_t team);
        void Frame();
    };

    void Server::Join(int slot)
    {
        const uint32_t protocol = Call0(GUEST_FUNC(T_ProtocolVersion));
        const uint32_t checksum = Call0(GUEST_FUNC(T_FF_Checksum));
        const int32_t statVersion = int32_t(Call0(GUEST_FUNC(T_Stats_Version)));
        const uint32_t statChecksum = Call0(GUEST_FUNC(T_Stats_FormatChecksum));

        const uint16_t port = g_nextPort;
        g_nextPort = uint16_t(g_nextPort == 0xFFFF ? 1 : g_nextPort + 1);

        char line[512];
        std::snprintf(line, sizeof line,
                      "connect bot%u \"\\name\\%s\\protocol\\%u\\checksum\\%u\\statver\\%d %u"
                      "\\challenge\\0\\xuid\\0\\invited\\0\\onlineStats\\0\\migrating\\0\"",
                      unsigned(port), kNames[slot], protocol, checksum, statVersion, statChecksum);
        ctx.r3.u64 = PutText(line);
        GUEST_FUNC(T_SV_Cmd_TokenizeString)(ctx, base);

        const uint32_t dvar = U32(T_DATA_FirstPublicSlotDvar);
        const uint32_t first = dvar ? U32(dvar + 8) : 0;
        if (dvar) Put32(dvar + 8, uint32_t(slot));
        ctx.r3.u64 = 0;                         // type 0, IP 0
        ctx.r4.u64 = uint64_t(port) << 48;      // the port, then the local network id
        GUEST_FUNC(T_SV_DirectConnect)(ctx, base);
        if (dvar) Put32(dvar + 8, first);

        GUEST_FUNC(T_SV_Cmd_EndTokenizedString)(ctx, base);

        if (State(slot) == 3 && IsBot(slot) && Port(slot) == port) {
            g_bots[slot] = {};
            g_bots[slot].port = port;
            LOGI("bots: %s joined in slot %d", kNames[slot], slot);
            g_nextAdd = time + 250;
        } else {
            LOGW("bots: the server refused %s (slot %d is at state %u)", kNames[slot], slot, State(slot));
            g_nextAdd = time + 5000;
        }
    }

    void Server::Enter(int slot)
    {
        const uint32_t client = Client(slot);
        Put32(client, 4);
        Put32(client + T_SVCLIENT_GAMESTATE_ACK, 0);
        Put32(client + T_SVCLIENT_GAMESTATE_MSG, U32(client + T_SVCLIENT_NETCHAN));

        std::memset(base + cmd, 0, kCmdSize);
        ctx.r3.u64 = client;
        ctx.r4.u64 = cmd;
        GUEST_FUNC(T_SV_ClientEnterWorld)(ctx, base);

        Bot& bot = g_bots[slot];
        const uint16_t port = Port(slot);
        bot = {};
        bot.port = port;
        bot.nextMenu = time + 1000;
        LOGI("bots: %s entered the match", Name(slot));
    }

    // The team menu, then the class menu, a second apart, until the bot is alive.
    void Server::Menus(int slot, Bot& bot)
    {
        if (time < bot.nextMenu) return;
        bot.nextMenu = time + 1000;
        if (!g_menusRead) ReadMenus();
        if (g_teamMenu < 0 || g_classMenu < 0) return;

        const uint32_t serverId = U32(T_DATA_ServerId);
        char line[96];
        if (bot.menuStep == 0) {
            const char* team = "autoassign";
            if (Wanted().against) {
                // The side of the first player who has one; until then the bot waits.
                uint32_t players = 0;
                for (int i = 0; i < count && !players; ++i) {
                    if (State(i) != 5 || IsBot(i) || !GameClient(i)) continue;
                    const uint32_t side = U32(GameClient(i) + T_GCLIENT_TEAM);
                    if (side == kTeamAxis || side == kTeamAllies) players = side;
                }
                if (!players) return;
                team = players == kTeamAxis ? "allies" : "axis";
            }
            std::snprintf(line, sizeof line, "mr %u %d %s", serverId, g_teamMenu, team);
        } else {
            std::snprintf(line, sizeof line, "mr %u %d class%d", serverId, g_classMenu, Random(0, 4));
        }
        if (Traced()) LOGI("bots: %s answers \"%s\" (team %u, session %u)", Name(slot), line,
                           U32(GameClient(slot) + T_GCLIENT_TEAM), U32(GameClient(slot) + T_GCLIENT_SESSION_STATE));
        ClientCommand(slot, line);
        bot.menuStep ^= 1;
    }

    // The nearest enemy the bot can see: the one it has, or a new one inside its
    // view cone (or close enough to be heard). At most two traces a call.
    int Server::FindTarget(int slot, Bot& bot, const Skill& skill, const Vec& eye, uint32_t team)
    {
        struct Candidate { int slot; float distance; };
        Candidate near[kSlots];
        int found = 0;
        for (int i = 0; i < count; ++i) {
            if (i == slot || !Alive(i)) continue;
            const uint32_t other = U32(GameClient(i) + T_GCLIENT_TEAM);
            // Team 0 is a free-for-all, where everyone is an enemy.
            if (team != 0 && other == team) continue;
            const Vec at = Vector(GameClient(i) + T_PS_ORIGIN);
            const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > 3500.0f) continue;
            if (i != bot.target && distance > 350.0f) {
                const float off = Wrap(std::atan2(dy, dx) * 180.0f / kPi - bot.yaw);
                if (std::fabs(off) > skill.cone * 0.5f) continue;
            }
            near[found++] = { i, distance };
        }
        std::sort(near, near + found, [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

        for (int i = 0; i < found && i < 2; ++i) {
            const uint32_t client = GameClient(near[i].slot);
            Vec head = Vector(client + T_PS_ORIGIN);
            head.z += Real(client + T_PS_VIEW_HEIGHT);
            if (SightClear(eye, head, uint32_t(slot), uint32_t(near[i].slot))) return near[i].slot;
        }
        return -1;
    }

    void Server::Fight(int slot, Bot& bot, const Skill& skill, const Vec& eye, uint32_t& buttons, int& forward, int& right)
    {
        const uint32_t client = GameClient(bot.target);
        Vec aim = bot.lastKnown;
        if (bot.visible) {
            aim = Vector(client + T_PS_ORIGIN);
            aim.z += Real(client + T_PS_VIEW_HEIGHT) * 0.75f;       // the chest
            bot.lastKnown = aim;
        }
        const float dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
        const float flat = std::sqrt(dx * dx + dy * dy);
        const float distance = std::sqrt(flat * flat + dz * dz);
        const float wantYaw = std::atan2(dy, dx) * 180.0f / kPi;
        const float wantPitch = -std::atan2(dz, flat) * 180.0f / kPi;

        // The error is drawn again a few times a second and shrinks to a third
        // over three seconds of tracking.
        if (time >= bot.nextError) {
            bot.nextError = time + 350;
            const float tracked = float(time - bot.firstSeen);
            const float size = skill.error * std::max(0.35f, 1.0f - tracked / 3000.0f);
            bot.errorYaw = Random(-size, size);
            bot.errorPitch = Random(-size, size) * 0.5f;
        }

        const float step = skill.turn * (kFrame / 1000.0f);
        bot.yaw = Wrap(bot.yaw + std::clamp(Wrap(wantYaw + bot.errorYaw - bot.yaw), -step, step));
        bot.pitch += std::clamp(wantPitch + bot.errorPitch - bot.pitch, -step, step);

        if (!bot.visible) {
            // Towards where it was last seen.
            forward = 127;
            return;
        }

        // Close enough when the aim is inside the target's width at that range.
        const float width = std::max(2.5f, std::atan2(26.0f, distance) * 180.0f / kPi);
        const bool onTarget = std::fabs(Wrap(wantYaw - bot.yaw)) < width && std::fabs(wantPitch - bot.pitch) < width * 1.5f;
        if (time - bot.firstSeen >= skill.reaction) {
            if (time >= bot.burstUntil && time >= bot.pauseUntil && onTarget) {
                bot.burstUntil = time + Random(250, 650);
                bot.pauseUntil = bot.burstUntil + skill.pause + Random(0, 200);
            }
            if (time < bot.burstUntil) {
                buttons |= kButtonAttack;
                bot.lastShot = time;
            }
        }
        bot.reload = true;

        if (distance > 700.0f) forward = 127;
        else if (distance < 200.0f) forward = -100;
        if (skill.strafes) {
            if (time >= bot.nextStrafe) {
                bot.nextStrafe = time + Random(500, 1500);
                bot.strafe = -bot.strafe;
                if (skill.crouches && Random(0, 4) == 0) bot.crouchUntil = time + Random(800, 1800);
            }
            right = bot.strafe * 127;
        }
        if (time < bot.crouchUntil) buttons |= kButtonCrouch;
    }

    // Towards the nearest enemy, wherever it is, or straight on when there is
    // none. Three times a second eight directions are felt out with sight traces
    // and the best becomes the course: open ahead, and beyond that open further,
    // towards the goal, not a sharp turn, and not back to where the bot has just
    // been -- which is what takes it out of a dead end the goal lies behind. No
    // progress in half a second tries a jump and a course picked at random.
    void Server::Wander(int slot, Bot& bot, const Vec& origin, uint32_t& buttons, int& forward)
    {
        if (time >= bot.nextHeading) {
            bot.nextHeading = time + Random(1500, 3000);
            const uint32_t team = U32(GameClient(slot) + T_GCLIENT_TEAM);
            float best = 1e12f;
            bool any = false, otherFloor = false;
            for (int i = 0; i < count; ++i) {
                if (i == slot || !Alive(i)) continue;
                if (team != 0 && U32(GameClient(i) + T_GCLIENT_TEAM) == team) continue;
                const Vec at = Vector(GameClient(i) + T_PS_ORIGIN);
                const float dx = at.x - origin.x, dy = at.y - origin.y;
                const float distance = dx * dx + dy * dy;
                if (distance >= best) continue;
                best = distance;
                bot.heading = std::atan2(dy, dx) * 180.0f / kPi;
                // Right above or below: walking at it leads nowhere, the way up
                // or down is somewhere else.
                otherFloor = distance < 500.0f * 500.0f && std::fabs(at.z - origin.z) > 70.0f;
                any = true;
            }
            if (!any) bot.heading = Wrap(bot.heading + Random(-60.0f, 60.0f));
            else if (otherFloor) { bot.heading = Random(-180.0f, 180.0f); bot.nextHeading = time + Random(3000, 5000); }
        }

        if (time >= bot.nextCrumb) {
            bot.nextCrumb = time + 1500;
            bot.trail[bot.trailNext] = origin;
            bot.trailNext = (bot.trailNext + 1) % kTrail;
            bot.trailCount = std::min(bot.trailCount + 1, kTrail);
        }

        if (time >= bot.nextProgress) {
            const float dx = origin.x - bot.progressFrom.x, dy = origin.y - bot.progressFrom.y;
            if (bot.nextProgress != 0 && dx * dx + dy * dy < 24.0f * 24.0f) {
                bot.course = Wrap(bot.course + Random(70.0f, 290.0f));
                bot.courseUntil = time + Random(700, 1500);
                bot.jump = 2;
            }
            bot.progressFrom = origin;
            bot.nextProgress = time + 600;
        }

        if (time >= bot.courseUntil && ((time / kFrame) + slot) % 6 == 0) {
            const Vec from{ origin.x, origin.y, origin.z + 40.0f };
            const auto clear = [&](float yaw, float range) {
                const float radians = yaw * kPi / 180.0f;
                const Vec to{ from.x + std::cos(radians) * range, from.y + std::sin(radians) * range, from.z };
                return SightClear(from, to, uint32_t(slot), kNoEntity);
            };
            const auto cosine = [](float degrees) { return std::cos(degrees * kPi / 180.0f); };
            float best = -1e9f, chosen = Wrap(bot.course + 180.0f);
            for (int k = 0; k < 8; ++k) {
                // The goal's own direction is one of the eight.
                const float yaw = Wrap(bot.heading + float(k) * 45.0f);
                if (!clear(yaw, 100.0f)) continue;
                float score = clear(yaw, 350.0f) ? 1.0f : 0.0f;
                score += cosine(yaw - bot.heading) + 0.8f * cosine(yaw - bot.course);
                for (int i = 0; i < bot.trailCount; ++i) {
                    const float dx = bot.trail[i].x - origin.x, dy = bot.trail[i].y - origin.y;
                    const float distance = dx * dx + dy * dy;
                    if (distance < 60.0f * 60.0f || distance > 500.0f * 500.0f) continue;
                    score -= 0.35f * std::max(0.0f, cosine(yaw - std::atan2(dy, dx) * 180.0f / kPi));
                }
                if (score > best) { best = score; chosen = yaw; }
            }
            bot.course = chosen;
        }

        const float step = 300.0f * (kFrame / 1000.0f);
        const float off = Wrap(bot.course - bot.yaw);
        bot.yaw = Wrap(bot.yaw + std::clamp(off, -step, step));
        bot.pitch += std::clamp(-bot.pitch, -step, step);
        // Turning on the spot first keeps it from walking a wide arc into the wall.
        forward = std::fabs(off) < 60.0f ? 127 : 0;
        if (bot.jump > 0) { --bot.jump; buttons |= kButtonJump; }
        if (bot.reload) { bot.reload = false; buttons |= kButtonUseReload; }
    }

    void Server::Think(int slot, Bot& bot)
    {
        const uint32_t client = Client(slot);
        const uint32_t entity = Entity(slot);
        const uint32_t game = GameClient(slot);
        if (!entity || !game) return;

        uint32_t buttons = 0;
        int forward = 0, right = 0;

        if (!Alive(slot)) {
            if (bot.wasAlive) bot.downSince = time;
            bot.wasAlive = false;
            bot.target = -1;
            bot.aimSet = false;
            // Down for longer than any respawn takes: the scripts made it a
            // spectator again (a new round, a team change), so the menus once more.
            if (bot.spawned && time - bot.downSince > 12000) { bot.spawned = false; bot.menuStep = 0; }
            if (!bot.spawned) {
                Menus(slot, bot);
            } else if ((time / kFrame) & 1) {
                // Dead or in a killcam: the stock think's way to respawn, a press of
                // use. Pressed every other frame so each is a new press.
                buttons |= kButtonUseReload;
            }
        } else {
            if (!bot.wasAlive) {
                bot.wasAlive = true;
                if (!bot.spawned) LOGI("bots: %s spawned (team %u)", Name(slot), U32(game + T_GCLIENT_TEAM));
                bot.spawned = true;
                bot.nextProgress = 0;
                bot.courseUntil = 0;
                bot.trailCount = 0;
            }
            const Vec origin = Vector(game + T_PS_ORIGIN);
            Vec eye = origin;
            eye.z += Real(game + T_PS_VIEW_HEIGHT);
            if (!bot.aimSet) {
                bot.aimSet = true;
                bot.pitch = 0;
                bot.yaw = bot.heading = bot.course = Real(game + T_PS_VIEW_ANGLES + 4);
            }
            const Skill& skill = kSkills[Wanted().skill.load()];

            if (bot.target >= 0 && !Alive(bot.target)) {
                if (Traced() && time - bot.lastShot < 1000)
                    LOGI("bots: %s was firing at %s, who went down", Name(slot), Name(bot.target));
                bot.target = -1;
            }
            // A look every other frame, the bots taking turns.
            if (time >= bot.nextLook) {
                bot.nextLook = time + 2 * kFrame;
                const int seen = FindTarget(slot, bot, skill, eye, U32(game + T_GCLIENT_TEAM));
                if (seen >= 0) {
                    if (seen != bot.target) { bot.target = seen; bot.firstSeen = time; bot.nextError = 0; }
                    bot.visible = true;
                    bot.lastSeen = time;
                } else {
                    bot.visible = false;
                    if (bot.target >= 0 && time - bot.lastSeen > skill.memory) bot.target = -1;
                }
            }

            if (bot.target >= 0) Fight(slot, bot, skill, eye, buttons, forward, right);
            else Wander(slot, bot, origin, buttons, forward);

            if (Traced() && time % 5000 == 0)
                LOGI("bots: %s at %.0f %.0f %.0f, yaw %.0f, %s%s", Name(slot), origin.x, origin.y, origin.z, bot.yaw,
                     bot.target >= 0 ? (bot.visible ? "sees " : "follows ") : std::fabs(Wrap(bot.course - bot.heading)) > 60.0f ? "going round" : "walking",
                     bot.target >= 0 ? Name(bot.target) : "");
        }

        const auto angle = [](float degrees) { return uint32_t(int32_t(std::lround(degrees * (65536.0f / 360.0f)))) & 0xFFFFu; };
        std::memset(base + cmd, 0, kCmdSize);
        // The stock think's time base: four frames behind the server.
        Put32(cmd + 0x0, uint32_t(time - 200));
        Put32(cmd + 0x4, buttons);
        Put32(cmd + 0x8, angle(bot.pitch - Real(game + T_PS_DELTA_ANGLES)));
        Put32(cmd + 0xC, angle(bot.yaw - Real(game + T_PS_DELTA_ANGLES + 4)));
        Put32(cmd + 0x14, U32(game + T_PS_WEAPON));
        PPC_STORE_U8(cmd + 0x1C, uint8_t(int8_t(forward)));
        PPC_STORE_U8(cmd + 0x1D, uint8_t(int8_t(right)));

        // As the stock think: every snapshot counts as acknowledged.
        Put32(client + T_SVCLIENT_DELTA_MESSAGE, U32(client + T_SVCLIENT_NETCHAN) - 1);
        ctx.r3.u64 = client;
        ctx.r4.u64 = cmd;
        GUEST_FUNC(T_SV_ClientThink)(ctx, base);
    }

    void Server::Frame()
    {
        GUEST_FUNC(T_SV_BotFrameBegin)(ctx, base);
        if (!Read()) return;

        // A new level: the bots that came through it start again, and none joins
        // until its scripts have run for a moment.
        const uint32_t serverId = U32(T_DATA_ServerId);
        if (serverId != g_serverId || time < g_lastTime) {
            g_serverId = serverId;
            g_levelStart = time;
            g_nextAdd = 0;
            g_menusRead = false;
            for (Bot& bot : g_bots) bot = {};
        }
        g_lastTime = time;
        const bool settled = time - g_levelStart >= kSettle;

        int players = 0, bots = 0, highestBot = -1, freeSlot = -1;
        for (int i = 0; i < count; ++i) {
            const uint32_t state = State(i);
            if (IsBot(i) && state == 1) { Put32(Client(i), 0); freeSlot = i; continue; }
            if (state == 0) { freeSlot = i; continue; }
            if (IsBot(i)) { ++bots; highestBot = i; }
            else ++players;
        }

        // A player always has a slot: the bots give way.
        const int wanted = std::min(Wanted().count.load(), count - players);
        if (bots > wanted && highestBot >= 0) {
            Drop(highestBot, "fewer bots are wanted");
        } else if (bots < wanted && freeSlot >= 0 && settled && time >= g_nextAdd) {
            Join(freeSlot);
        }

        for (int i = 0; i < count; ++i) {
            const uint32_t state = State(i);
            if (state < 3 || !IsBot(i)) continue;
            Bot& bot = g_bots[i];
            if (bot.port != Port(i)) { bot = {}; bot.port = Port(i); }
            // No packet ever comes from a bot, and the timeout check spares only
            // the host's own loopback client.
            Put32(Client(i) + T_SVCLIENT_LAST_PACKET, uint32_t(time));
            // Nor an acknowledgement: the reliable commands sent to it would
            // pile up until the server drops it ("Server command overflow").
            Put32(Client(i) + T_SVCLIENT_RELIABLE_ACKNOWLEDGE, U32(Client(i) + T_SVCLIENT_RELIABLE_SEQUENCE));
            if (state != 5) { if (settled) Enter(i); continue; }
            Think(i, bot);
        }
    }
}

bool bots::ConsoleLine(const char* text)
{
    while (*text == ' ' || *text == '\t') ++text;
    if (strncasecmp(text, "bots", 4) != 0) return false;
    const char* rest = text + 4;
    if (*rest && *rest != ' ' && *rest != '\t' && *rest != '\n' && *rest != '\r') return false;
    while (*rest == ' ' || *rest == '\t') ++rest;

    Settings& settings = Wanted();
    if (!strncasecmp(rest, "skill", 5)) {
        settings.skill = std::clamp(int(std::strtol(rest + 5, nullptr, 10)), 0, 3);
    } else if (*rest == '+') {
        settings.count = std::min(settings.count.load() + 1, kMostBots);
    } else if (*rest == '-') {
        settings.count = std::max(settings.count.load() - 1, 0);
    } else if (*rest >= '0' && *rest <= '9') {
        settings.count = std::clamp(int(std::strtol(rest, nullptr, 10)), 0, kMostBots);
    }
    LOGI("bots: %d wanted, %s", settings.count.load(), kSkills[settings.skill.load()].name);
    return true;
}

GUEST_HOOK(T_SV_BotUserMoves)
{
    // The one call the server makes every frame, so the vote between two
    // matches (map_vote.cpp) is run from here as well, bots or none.
    mapvote::ServerFrame(ctx, base);
    if (!g_engaged) {
        // Untouched until a bot is asked for: the stock think has nobody to move.
        if (Wanted().count.load() <= 0) { GUEST_ORIG(T_SV_BotUserMoves)(ctx, base); return; }
        g_engaged = true;
    }
    const PPCContext saved = ctx;
    Server(ctx, base).Frame();
    ctx = saved;
}

// (the address by value in r3:r4; the type is its first word.) A player who
// arrives at a full server would be told EXE_SERVERISFULL: a bot leaves first.
GUEST_HOOK(T_SV_DirectConnect)
{
    if (g_engaged && uint32_t(ctx.r3.u64 >> 32) != 0) {
        const PPCContext saved = ctx;
        Server server(ctx, base);
        if (server.Read()) {
            int bot = -1;
            bool room = false;
            for (int i = 0; i < server.count; ++i) {
                if (server.State(i) == 0) room = true;
                else if (server.IsBot(i)) bot = i;
            }
            if (!room && bot >= 0) server.Drop(bot, "a player needs the slot");
        }
        ctx = saved;
    }
    GUEST_ORIG(T_SV_DirectConnect)(ctx, base);
}

#endif

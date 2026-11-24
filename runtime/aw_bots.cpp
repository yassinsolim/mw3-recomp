// Bots for Advanced Warfare's multiplayer: MW2_BOTS=<n> keeps n of them in
// every match this copy hosts, as bots.cpp does for the earlier games, and
// with the same switches and console line. Nothing else is shared with it,
// because this title has bots of its own.
//
// The executable has the whole of them: SV_AddBot and SV_AddTestClient (the
// two users of the format `connect bot%d "%s\name\%s\qport\%d\ctz\%s"`), a
// think in the engine, path data in the maps, and the scripts of
// common_bots_mp.ff, which add and drop bots to match what a lobby's Game
// Setup says (Friendly Bots, Enemy Bots and a difficulty for each). What Game
// Setup says is five bytes of the lobby's match rules (title.h,
// T_MatchRules_BotDefaults), and this file writes those: the host's scripts do
// the rest, so the bots walk the maps' paths, boost-jump, aim down the sight
// and use what their class carries, and one leaves when a player needs its
// slot ("fill_open").
//
// A lobby starts from the rules' defaults, so the number is put there, and
// what the player then sets in Game Setup still decides. The console line
// changes the rules of the lobby this copy is in at once: the scripts read
// them all through a match.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "bots.h"
#include "env.h"
#include "guest.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#ifdef T_MatchRules_BotDefaults

namespace
{
    // A server has twelve slots on this console and the host keeps one.
    constexpr int kMostBots = 11;
    constexpr uint8_t kFillOpen = 1;    // the connect type: 0 "off", 1 "fill_open"
    constexpr const char* kSkills[4] = { "Recruit", "Regular", "Hardened", "Veteran" };

    struct Settings
    {
        std::atomic<int> count;
        std::atomic<int> skill;
        bool against;           // every bot on the side the host is not on

        Settings()
        {
            count = int(std::min<uint64_t>(env::Number("MW2_BOTS"), kMostBots));
            skill = int(std::min<uint64_t>(env::Number("MW2_BOT_SKILL", 1), 3));
            const char* teams = env::Text("MW2_BOT_TEAMS");
            against = teams && (!strcasecmp(teams, "against") || !strcmp(teams, "1"));
            if (count > 0)
                LOGI("bots: MW2_BOTS: %d in the matches this copy hosts, %s, %s", count.load(),
                     kSkills[skill], against ? "against the host's team" : "filling both teams");
        }
    };
    Settings& Wanted()
    {
        static Settings settings;
        return settings;
    }

    // The host is one of the friendly side, so with both teams filled the
    // smaller half of the bots joins him.
    void Store(uint8_t* rules)
    {
        const Settings& wanted = Wanted();
        const int count = wanted.count.load();
        const int friendly = wanted.against ? 0 : count / 2;
        const uint8_t skill = uint8_t(wanted.skill.load());
        rules[T_MATCHRULES_BOT_CONNECT] = count > 0 ? kFillOpen : 0;
        rules[T_MATCHRULES_BOT_DIFFICULTY] = skill;
        rules[T_MATCHRULES_BOT_DIFFICULTY + 1] = skill;
        rules[T_MATCHRULES_BOT_LIMIT] = uint8_t(friendly);
        rules[T_MATCHRULES_BOT_LIMIT + 1] = uint8_t(count - friendly);
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
    LOGI("bots: %d wanted, %s", settings.count.load(), kSkills[settings.skill.load()]);

    // The rules of the lobby this copy made, once they hold anything (their
    // first word): before that the defaults below are still to come.
    uint8_t* rules = guest::Base() + T_DATA_MatchRules;
    uint32_t valid;
    std::memcpy(&valid, rules, 4);
    if (valid) Store(rules);
    return true;
}

// (rules): the title's own defaults, from the dvars bot_AutoConnectDefault and
// bot_DifficultyDefault, which are "off" and "regular" unless a console set
// them. Left alone when no bot was asked for.
GUEST_HOOK(T_MatchRules_BotDefaults)
{
    const uint32_t rules = ctx.r3.u32;
    GUEST_ORIG(T_MatchRules_BotDefaults)(ctx, base);
    if (Wanted().count.load() > 0) Store(base + rules);
}

#endif

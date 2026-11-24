// The end-of-match map vote of Modern Warfare 3's multiplayer, and its map
// rotation: a System Link or private match this copy hosts goes from one map
// into the next with everyone in it, nobody returning to a menu.
//
//   MW2_MAP_VOTE=1                       three maps to choose from after a match
//   MW2_MAP_ROTATION="mp_dome:dom,mp_seatown"
//                                        the maps (and game types) to go through
//                                        in order; with the vote, what it offers
//   MW2_MAP_VOTE_MODES="war,dom,conf"    the game types the vote may offer
//   MW2_MAP_VOTE_SECONDS=20              the longest a vote is open
//
// A match ends when its script calls exitLevel: the server shuts down, every
// player is dropped with "match ended" and the host has to start the next
// match from its menus. The hook leaves the call out, and what the menu's
// StartServer queues is queued for the next map instead.
//
// The vote runs in the time the title spends between the last kill and that
// call: the winner's banner, the final killcam, the scoreboard. The scripts
// set the dvar scr_gameended when a match is over (3 here; 2 while a round's
// result is up and another round follows), which is looked for twice a second.
// The choices go out as the lines iPrintLn makes, in the message feed: the
// bold line is one line, drawn where the banner is. A player answers with the
// button he throws his tactical grenade, uses or throws his lethal grenade
// with -- each client draws its own, from the binding named in the line --
// which every pad, keyboard and touch layout has. A stick would not do: a
// frozen player's usercmds carry no movement. The vote closes when every
// player has answered, when the script calls exitLevel, or when its time is
// up; a tie, or no answer at all, is settled by chance.
// Bots do not vote. Neither the killcam nor the scoreboard draws the feed, so
// what is seen of a vote is its first seconds, and one that could only open at
// exitLevel (scripts that never set the dvar) ends as the one nobody answered.
//
// What is offered: with MW2_MAP_ROTATION, three of its entries. Without, three
// of the maps the menus list from the .arena files whose fastfile is in the
// game's folder: the first with the game type just played, the others with one
// of the game types the menus list. The names shown are the menus' own string
// references, so each player reads them in his language.
//
// A game type other than the one played takes its own rules: a private match
// carries match rules, which the scripts read before any dvar, so the chosen
// game type's defaults (mp/recipes/<game type>.recipe) replace them as the
// lobby's game type menu does.
//
// The map change. SV_SpawnServer is the title's code for it: with a server
// running it tells every client "loadingnewmap", and after the load connects
// each to the new level. On the console nothing reached that with players in,
// and it loses them: loading the map's fastfile frees the old map's memory,
// the server's clients in it, and only a server that is not running allocates
// them again (title.h). So for the SV_SpawnServer the vote queued the clients
// are copied out before the load, the server reads as not running across it,
// and they are put back into what SV_Startup then allocates. The rest is the
// title's: the players load the map and are sent its gamestate, and the bots
// (bots.cpp) enter it as they enter any new level.
//
// The host ends it all from the pause menu: End Game is the script menu
// response "endround" (or "endgame"), and a match ended that way ends as it
// always did.
//
// Everything runs on the server's thread.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "map_vote.h"
#include "env.h"
#include "log.h"
#include "kernel/kernel.h"
#include "kernel/volume.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef T_Scr_ExitLevel

namespace
{
    constexpr int kSlots = 18;                  // the most clients a server has
    constexpr size_t kChoices = 3;
    constexpr int kWatch = 10;                  // server frames between two looks at scr_gameended
    constexpr uint32_t kRoundOver = 2;          // its value while a round's result is up and another follows
    constexpr int32_t kQuiet = 1000;            // ms a new vote ignores the buttons: the players are not frozen yet, nor have they read it
    constexpr int32_t kShortest = 5000;         // ms a vote stays open at least, exitLevel or not
    constexpr int32_t kRepeat = 2500;           // ms between two prints of the choices, which the feed drops after a few seconds

    // The buttons that vote, in the order of the choices: a usercmd's bit for
    // each and the command it is bound to, which a client draws as its own
    // button (LB, X and RB on a pad as the console has them). The grenades and
    // use: buttons that are down only while held. Crouch and prone are states
    // in a usercmd, and a client presses jump by itself when the scoreboard
    // stands its player up.
    constexpr uint32_t kButtons[kChoices] = { 0x8000, 0x20, 0x4000 };
    constexpr const char* kBindings[kChoices] = { "[{+smoke}] ", "[{+usereload}] ", "[{+frag}] " };

    struct Choice
    {
        std::string map, mode;
    };

    std::string Lower(std::string text)
    {
        for (char& c : text) c = char(std::tolower(uint8_t(c)));
        return text;
    }

    bool Has(const std::vector<std::string>& words, const std::string& word)
    {
        return std::find(words.begin(), words.end(), word) != words.end();
    }

    // Names separated by commas or spaces.
    std::vector<std::string> Words(const char* text)
    {
        std::vector<std::string> words;
        std::string word;
        for (const char* p = text ? text : ""; ; ++p) {
            if (*p && *p != ',' && *p != ' ' && *p != '\t') { word += *p; continue; }
            if (!word.empty()) words.push_back(Lower(word));
            word.clear();
            if (!*p) break;
        }
        return words;
    }

    struct Settings
    {
        bool vote;
        std::vector<Choice> rotation;           // a mode left empty is the one being played
        std::vector<std::string> modes;         // empty: every one the menus list
        int32_t open;                           // ms

        Settings()
        {
            vote = env::Flag("MW2_MAP_VOTE");
            for (const std::string& entry : Words(env::Text("MW2_MAP_ROTATION"))) {
                const size_t colon = entry.find(':');
                Choice choice{ entry.substr(0, colon), colon == std::string::npos ? "" : entry.substr(colon + 1) };
                if (!choice.map.empty()) rotation.push_back(std::move(choice));
            }
            modes = Words(env::Text("MW2_MAP_VOTE_MODES"));
            open = int32_t(std::clamp<uint64_t>(env::Number("MW2_MAP_VOTE_SECONDS", 20), 5, 60)) * 1000;
            if (vote)
                LOGI("map vote: MW2_MAP_VOTE: a vote of up to %d s after every match this copy hosts%s",
                     open / 1000, rotation.empty() ? "" : ", among the maps of MW2_MAP_ROTATION");
            else if (!rotation.empty())
                LOGI("map vote: MW2_MAP_ROTATION: %zu maps in turn in the matches this copy hosts", rotation.size());
        }

        bool Active() const { return vote || !rotation.empty(); }
    };
    const Settings& Wanted()
    {
        static const Settings settings;
        return settings;
    }

    enum class Phase { Idle, Voting, Decided };

    Phase g_phase = Phase::Idle;
    uint32_t g_level = 0;                       // the server id of the match the vote follows
    bool g_held = false;                        // its script has called exitLevel: the level is up for the vote alone
    std::optional<uint32_t> g_playing;          // the server id of a match seen before its end
    std::optional<uint32_t> g_endedByHost;      // and of one the host ended from the pause menu
    int g_untilWatch = 0;
    int32_t g_opened = 0, g_closes = 0, g_nextPrint = 0;
    std::vector<Choice> g_choices;
    int g_votes[kSlots];                        // the choice of the player in each slot, or -1
    int32_t g_heard[kSlots];                    // the time of the newest usercmd looked at
    uint32_t g_down[kSlots];                    // and its buttons
    Choice g_next;
    size_t g_turn = 0;                          // where the rotation is

    // The clients of a server that changes map (title.h): what each one was
    // when the old level's scripts were shut down, to be put back once the
    // new map's memory is there.
    struct Kept
    {
        uint32_t slot;
        std::vector<uint8_t> bytes;
    };
    bool g_changing = false;                    // the next SV_SpawnServer is the vote's
    bool g_carrying = false;                    // and this is it
    uint32_t g_keptFrom = 0;                    // where the clients were
    std::vector<Kept> g_kept;
    std::mt19937 g_random{ std::random_device{}() };

    // A line for the players, as the script's message builder makes it: text
    // as it is after 0x15, a string reference bare, and 0x14 before one that
    // does not come first.
    struct Message
    {
        std::string text;

        Message& Plain(const std::string& part)
        {
            text += '\x15';
            for (char c : part) text += (uint8_t(c) < 0x20 || c == '"' || c == ';') ? ' ' : c;
            return *this;
        }
        // A menu's name for a map or a game type: a string reference when it
        // looks like one (MPUI_DOME), and text when a converted map's arena
        // gives its name in so many words.
        Message& Label(const std::string& label)
        {
            const bool reference = label.find('_') != std::string::npos
                && std::all_of(label.begin(), label.end(), [](char c) {
                       return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
            if (!reference) return Plain(label);
            if (!text.empty()) text += '\x14';
            text += label;
            return *this;
        }
    };

    // The game's fastfiles, by name without the extension: the game folder's,
    // and the disc image's where the game is played from one.
    std::vector<std::string> Fastfiles()
    {
        std::vector<std::string> names;
        const auto take = [&](const std::filesystem::path& path) {
            if (Lower(path.extension().string()) == ".ff") names.push_back(Lower(path.stem().string()));
        };
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(kernel::GameRoot(), error)) take(entry.path());
        if (const auto& disc = kernel::Disc())
            for (const std::string& file : disc->RootFiles()) take(file);
        return names;
    }

    // The title, with a guest stack frame of its own below the caller's for the
    // calls made here and the text they take.
    struct Game
    {
        PPCContext& ctx;
        uint8_t* base;
        const PPCContext saved;
        uint32_t text;                          // 0x800 bytes of guest scratch

        Game(PPCContext& context, uint8_t* memory) : ctx(context), base(memory), saved(context)
        {
            // A callee saves its arguments just above the stack pointer it is
            // given, so the text starts well clear of it.
            const uint32_t sp = (ctx.r1.u32 - 0x1000) & ~0xFu;
            ctx.r1.u64 = sp;
            text = sp + 0x200;
        }
        ~Game() { ctx = saved; }

        uint32_t U32(uint32_t address) const { return PPC_LOAD_U32(address); }
        const char* Text(uint32_t address) const { return reinterpret_cast<const char*>(base + address); }
        uint32_t PutText(const std::string& value) const
        {
            const size_t length = std::min<size_t>(value.size(), 0x7FF);
            std::memcpy(base + text, value.data(), length);
            PPC_STORE_U8(text + uint32_t(length), 0);
            return text;
        }

        bool Flag(uint32_t dvar) const { return PPC_LOAD_U8(U32(dvar) + T_DVAR_CURRENT) != 0; }
        std::string String(uint32_t dvar) const { return Lower(Text(U32(U32(dvar) + T_DVAR_CURRENT))); }
        void SetString(uint32_t dvar, const std::string& value)
        {
            ctx.r3.u64 = U32(dvar);
            ctx.r4.u64 = PutText(value);
            GUEST_FUNC(T_Dvar_SetString)(ctx, base);
        }

        int32_t Time() const { return int32_t(U32(T_DATA_Svs + T_SVS_TIME)); }
        uint32_t Level() const { return U32(T_DATA_ServerId); }
        // A lobby of this copy's: the two kinds of match the host starts from
        // his own menus. A playlist's match has the title's own vote.
        bool Hosting() const { return Flag(T_DATA_PrivateMatchDvar) || Flag(T_DATA_SystemLinkDvar); }
        uint32_t Client(int slot) const { return U32(T_DATA_Svs + T_SVS_CLIENTS) + uint32_t(slot) * T_SVCLIENT_SIZE; }
        int Count() const { return std::min(int(U32(T_DATA_Svs + T_SVS_CLIENT_COUNT)), kSlots); }
        // In the match and not a bot, whose address type is 0.
        bool Player(int slot) const { return U32(Client(slot)) == 5 && U32(Client(slot) + T_SVCLIENT_ADDRESS) != 0; }

        void Print(const Message& message)
        {
            ctx.r3.u64 = uint64_t(int64_t(-1));
            ctx.r4.u64 = 0;
            ctx.r5.u64 = PutText("f \"" + message.text + "\"");
            GUEST_FUNC(T_SV_GameSendServerCommand)(ctx, base);
        }

        // The menus' name for a map, and for a game type; the bare name when
        // they have none.
        std::string MapLabel(const std::string& map) const
        {
            const uint32_t count = std::min<uint32_t>(U32(T_DATA_UiInfo + T_UI_MAP_COUNT), 128);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t entry = T_DATA_UiInfo + T_UI_MAPS + i * T_UI_MAP_SIZE;
                if (Lower(Text(entry + T_UI_MAP_NAME)) == map && *Text(entry)) return Text(entry);
            }
            return map;
        }
        std::string ModeLabel(const std::string& mode) const
        {
            const uint32_t count = std::min<uint32_t>(U32(T_DATA_UiInfo + T_UI_GAMETYPE_COUNT), 32);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t entry = T_DATA_UiInfo + T_UI_GAMETYPES + i * T_UI_GAMETYPE_SIZE;
                if (Lower(Text(entry)) == mode && *Text(entry + T_UI_GAMETYPE_LABEL)) return Text(entry + T_UI_GAMETYPE_LABEL);
            }
            return mode;
        }
        Message& Name(Message& message, const Choice& choice) const
        {
            return message.Label(MapLabel(choice.map)).Plain(" - ").Label(ModeLabel(choice.mode));
        }

        // The game types the menus list.
        std::vector<std::string> Modes() const
        {
            std::vector<std::string> modes;
            const uint32_t count = std::min<uint32_t>(U32(T_DATA_UiInfo + T_UI_GAMETYPE_COUNT), 32);
            for (uint32_t i = 0; i < count; ++i)
                modes.push_back(Lower(Text(T_DATA_UiInfo + T_UI_GAMETYPES + i * T_UI_GAMETYPE_SIZE)));
            return modes;
        }

        std::vector<Choice> Offer(const std::string& map, const std::string& mode);
        bool Open();
        bool Hold();
        void Listen(int slot);
        void Watch();
        void PrintChoices();
        void Decide();
        void Load();
        void Frame();
    };

    // Up to three choices for the match after one of `mode` on `map`, the map
    // just played left out while there are enough others; the one next map
    // when a rotation is gone through in turn.
    std::vector<Choice> Game::Offer(const std::string& map, const std::string& mode)
    {
        const Settings& settings = Wanted();
        const std::vector<std::string> files = Fastfiles(), known = Modes();

        std::vector<Choice> pool;
        if (!settings.rotation.empty()) {
            for (Choice entry : settings.rotation) {
                if (!Has(files, entry.map)) { LOGW("map vote: MW2_MAP_ROTATION names %s, which has no fastfile here", entry.map.c_str()); continue; }
                if (!entry.mode.empty() && !Has(known, entry.mode)) {
                    LOGW("map vote: MW2_MAP_ROTATION names the game type %s, which the game does not have", entry.mode.c_str());
                    entry.mode.clear();
                }
                if (entry.mode.empty()) entry.mode = mode;
                pool.push_back(std::move(entry));
            }
            if (!settings.vote) {
                // In turn: the entry after the map just played, looked for from
                // where the rotation stands, so a map named twice is not a loop.
                if (pool.empty()) return {};
                for (size_t i = 0; i < pool.size(); ++i) {
                    const size_t at = (g_turn + i) % pool.size();
                    if (pool[at].map != map) continue;
                    g_turn = at + 1;
                    break;
                }
                g_turn %= pool.size();
                return { pool[g_turn] };
            }
        }

        // The game types a map of the menus' may be drawn with. The title's
        // list has five the disc left without a menu entry and unfinished
        // (match_options.cpp), two of which do not start: a vote on an AYN
        // Thor offered Bakaara as One Flag CTF. They are drawn only when
        // MW2_MAP_VOTE_MODES names them.
        static const std::vector<std::string> kCut = { "arena", "oneflag", "gtnw", "ctfpro", "vip" };
        std::vector<std::string> modes;
        if (settings.rotation.empty()) {
            for (const std::string& name : known)
                if (settings.modes.empty() ? !Has(kCut, name) : Has(settings.modes, name)) modes.push_back(name);
            if (modes.empty()) modes.push_back(mode);
            const uint32_t count = std::min<uint32_t>(U32(T_DATA_UiInfo + T_UI_MAP_COUNT), 128);
            for (uint32_t i = 0; i < count; ++i) {
                const std::string name = Lower(Text(T_DATA_UiInfo + T_UI_MAPS + i * T_UI_MAP_SIZE + T_UI_MAP_NAME));
                if (Has(files, name)) pool.push_back({ name, "" });
            }
        }

        std::shuffle(pool.begin(), pool.end(), g_random);
        const auto played = [&](const Choice& entry) { return entry.map == map; };
        if (size_t(std::count_if(pool.begin(), pool.end(), played)) + kChoices <= pool.size())
            pool.erase(std::remove_if(pool.begin(), pool.end(), played), pool.end());

        std::vector<Choice> offer;
        for (Choice& entry : pool) {
            if (offer.size() == kChoices) break;
            // The first keeps the game type just played where the settings
            // allow it, so a lobby that wants only that always can.
            if (entry.mode.empty()) entry.mode = offer.empty() && Has(modes, mode) ? mode : modes[g_random() % modes.size()];
            const auto same = [&](const Choice& other) { return other.map == entry.map && other.mode == entry.mode; };
            if (std::none_of(offer.begin(), offer.end(), same)) offer.push_back(std::move(entry));
        }
        return offer;
    }

    // The match is over: the vote opens, or a rotation names its next map.
    // False when there is nothing to offer.
    bool Game::Open()
    {
        const std::string map = String(T_DATA_UiMapNameDvar), mode = String(T_DATA_GameTypeDvar);
        g_choices = Offer(map, mode);
        if (g_choices.empty()) {
            LOGW("map vote: no map to offer after %s; back to the lobby", map.c_str());
            return false;
        }
        g_level = Level();
        g_held = false;
        g_opened = Time();
        std::fill(std::begin(g_votes), std::end(g_votes), -1);
        std::fill(std::begin(g_heard), std::end(g_heard), g_opened);
        // Down until seen up: a button held as the match ended is no answer.
        std::fill(std::begin(g_down), std::end(g_down), ~0u);
        if (g_choices.size() == 1) {
            g_next = g_choices[0];
            g_phase = Phase::Decided;
            Message line;
            Print(Name(line.Plain("Next map: "), g_next));
            LOGI("map vote: %s (%s) is over; next %s (%s)", map.c_str(), mode.c_str(), g_next.map.c_str(), g_next.mode.c_str());
            return true;
        }
        g_phase = Phase::Voting;
        g_closes = g_opened + Wanted().open;
        g_nextPrint = g_opened;
        std::string list;
        for (const Choice& choice : g_choices) list += (list.empty() ? "" : ", ") + choice.map + " (" + choice.mode + ")";
        LOGI("map vote: %s (%s) is over; a vote between %s", map.c_str(), mode.c_str(), list.c_str());
        return true;
    }

    // The script has called exitLevel. True keeps the level up until the next
    // map is known and loads; false leaves the call to the title.
    bool Game::Hold()
    {
        const bool mine = g_phase != Phase::Idle && g_level == Level();
        if (!Hosting() || g_endedByHost == Level()) {
            if (mine) g_phase = Phase::Idle;
            if (g_endedByHost == Level()) LOGI("map vote: the host ended the game; back to the lobby");
            return false;
        }
        if (!mine && !Open()) return false;
        g_held = true;
        // As the call left out would have: a match's scores end with it.
        PPC_STORE_U32(T_DATA_Level + T_LEVEL_SAVE_PERSIST, 0);
        // What is left of a vote is spent on the scoreboard, which shows none of it.
        if (g_phase == Phase::Voting) g_closes = std::min(g_closes, std::max(Time(), g_opened + kShortest));
        return true;
    }

    // Between votes: is the match over? The dvar still says so when the next
    // level starts, so a match counts once it has been seen before its end.
    void Game::Watch()
    {
        if (!Hosting()) return;
        ctx.r3.u64 = PutText("scr_gameended");
        GUEST_FUNC(T_Dvar_GetInt)(ctx, base);
        if (ctx.r3.u32 == 0 || ctx.r3.u32 == kRoundOver) { g_playing = Level(); return; }
        if (g_playing != Level()) return;
        g_playing.reset();
        if (g_endedByHost != Level()) Open();
    }

    // A player's vote is a press of one of three buttons while the vote is
    // open; his last one counts. The server keeps the newest usercmd it ran
    // and the eight it has yet to run, which between them are every one that
    // came: frozen, in a killcam or on the scoreboard a player's commands are
    // still sent, and their buttons are still his.
    void Game::Listen(int slot)
    {
        struct Command
        {
            int32_t time;
            uint32_t buttons;
        };
        Command fresh[1 + T_SVCLIENT_QUEUED_USERCMD_COUNT];
        size_t count = 0;
        const auto hear = [&](uint32_t cmd) {
            const int32_t time = int32_t(U32(cmd));
            if (time > g_heard[slot] && time <= Time() + 1000) fresh[count++] = { time, U32(cmd + T_USERCMD_BUTTONS) };
        };
        hear(Client(slot) + T_SVCLIENT_LAST_USERCMD);
        for (uint32_t i = 0; i < T_SVCLIENT_QUEUED_USERCMD_COUNT; ++i)
            hear(Client(slot) + T_SVCLIENT_QUEUED_USERCMDS + i * T_USERCMD_SIZE);
        std::sort(fresh, fresh + count, [](const Command& a, const Command& b) { return a.time < b.time; });

        for (size_t n = 0; n < count; ++n) {
            const uint32_t pressed = fresh[n].buttons & ~g_down[slot];
            g_heard[slot] = fresh[n].time;
            g_down[slot] = fresh[n].buttons;
            if (fresh[n].time < g_opened + kQuiet) continue;
            for (size_t i = 0; i < g_choices.size(); ++i) {
                if (!(pressed & kButtons[i]) || g_votes[slot] == int(i)) continue;
                g_votes[slot] = int(i);
                g_nextPrint = 0;                // the count shows at once
                LOGI("map vote: slot %d votes for %s (%s)", slot, g_choices[i].map.c_str(), g_choices[i].mode.c_str());
            }
        }
    }

    // Four lines, which is what the message feed holds: printed again they
    // stand still, with the votes so far.
    void Game::PrintChoices()
    {
        Message head;
        Print(head.Plain("Vote for the next map:"));
        for (size_t i = 0; i < g_choices.size(); ++i) {
            const int votes = int(std::count(std::begin(g_votes), std::end(g_votes), int(i)));
            Message line;
            Name(line.Plain(kBindings[i]), g_choices[i]);
            if (votes > 0) line.Plain("  (" + std::to_string(votes) + ")");
            Print(line);
        }
    }

    // The most votes; chance among the choices that share them, which with no
    // vote at all is every one.
    void Game::Decide()
    {
        int votes[kChoices] = {}, most = 0, cast = 0;
        for (int i = 0; i < Count(); ++i) {
            if (g_votes[i] < 0 || !Player(i)) continue;
            most = std::max(most, ++votes[g_votes[i]]);
            ++cast;
        }
        std::vector<size_t> leading;
        for (size_t i = 0; i < g_choices.size(); ++i)
            if (votes[i] == most) leading.push_back(i);
        g_next = g_choices[leading[g_random() % leading.size()]];

        g_phase = Phase::Decided;
        Message line;
        Print(Name(line.Plain("Next map: "), g_next));
        LOGI("map vote: %s (%s) wins with %d of %d votes", g_next.map.c_str(), g_next.mode.c_str(), most, cast);
    }

    void Game::Load()
    {
        g_phase = Phase::Idle;
        if (g_next.mode != String(T_DATA_GameTypeDvar)) {
            GUEST_FUNC(T_MatchRules_Get)(ctx, base);
            const uint32_t rules = ctx.r3.u32;
            if (rules && U32(rules + T_MATCHRULES_IN_USE)) {
                const uint32_t data = U32(rules + T_MATCHRULES_DATA);
                ctx.r3.u64 = data;
                ctx.r4.u64 = PutText(g_next.mode);
                GUEST_FUNC(T_MatchRules_LoadDefaults)(ctx, base);
                if (ctx.r3.u32) {
                    ctx.r3.u64 = data;
                    GUEST_FUNC(T_MatchRules_Apply)(ctx, base);
                } else {
                    LOGW("map vote: no match rules for %s; the game type stays %s", g_next.mode.c_str(),
                         String(T_DATA_GameTypeDvar).c_str());
                }
            } else {
                SetString(T_DATA_UiGameTypeDvar, g_next.mode);
                SetString(T_DATA_GameTypeDvar, g_next.mode);
            }
        }
        SetString(T_DATA_UiMapNameDvar, g_next.map);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = T_UI_StartServerCall;
        GUEST_FUNC(T_Cbuf_AddCall)(ctx, base);
        g_changing = true;
        LOGI("map vote: loading %s (%s)", g_next.map.c_str(), String(T_DATA_GameTypeDvar).c_str());
    }

    void Game::Frame()
    {
        // The level went some other way: the host left it.
        if (Level() != g_level) { g_phase = Phase::Idle; return; }
        if (g_phase == Phase::Voting) {
            const int32_t time = Time();
            int players = 0, voted = 0;
            for (int i = 0; i < Count(); ++i) {
                if (!Player(i)) continue;
                Listen(i);
                ++players;
                if (g_votes[i] >= 0) ++voted;
            }
            if (time >= g_closes || (players > 0 && voted == players)) {
                Decide();
            } else if (time >= g_nextPrint) {
                g_nextPrint = time + kRepeat;
                PrintChoices();
            }
        }
        if (g_phase == Phase::Decided && g_held) Load();
    }
}

void mapvote::ServerFrame(PPCContext& ctx, uint8_t* base)
{
    if (g_phase != Phase::Idle) {
        Game(ctx, base).Frame();
    } else if (Wanted().Active() && --g_untilWatch <= 0) {
        g_untilWatch = kWatch;
        Game(ctx, base).Watch();
    }
}

GUEST_HOOK(T_Scr_ExitLevel)
{
    if (Wanted().Active() && Game(ctx, base).Hold()) return;
    GUEST_ORIG(T_Scr_ExitLevel)(ctx, base);
}

// (client, text, ...). The pause menu's End Game is the menu response
// "endround" or "endgame": a match the host ends is not followed by another.
GUEST_HOOK(T_SV_ExecuteClientCommand)
{
    if (Wanted().Active() && ctx.r4.u32 && PPC_LOAD_U32(ctx.r3.u32 + T_SVCLIENT_ADDRESS) != 0) {
        const std::string text = reinterpret_cast<const char*>(base + ctx.r4.u32);
        const auto ends = [&](const char* word) {
            const size_t length = std::strlen(word);
            return text.size() > length && text.compare(text.size() - length, length, word) == 0;
        };
        if (text.compare(0, 3, "mr ") == 0 && (ends(" endround") || ends(" endgame")))
            g_endedByHost = PPC_LOAD_U32(T_DATA_ServerId);
    }
    GUEST_ORIG(T_SV_ExecuteClientCommand)(ctx, base);
}

// (map, preloaded, migrating). The one the vote queued takes its clients along.
GUEST_HOOK(T_SV_SpawnServer)
{
    g_carrying = std::exchange(g_changing, false) && PPC_LOAD_U32(T_DATA_ServerId) == g_level
        && PPC_LOAD_U8(PPC_LOAD_U32(T_DATA_SvRunningDvar) + T_DVAR_CURRENT) != 0;
    GUEST_ORIG(T_SV_SpawnServer)(ctx, base);
    g_carrying = false;
    g_kept = {};
}

// Inside SV_SpawnServer, with the old level's scripts shut down and its memory
// about to go: the clients are copied out, and the server reads as not running
// from here on, which is what has SV_Startup called for the new map.
GUEST_HOOK(T_SV_ClearServer)
{
    if (g_carrying) {
        g_keptFrom = PPC_LOAD_U32(T_DATA_Svs + T_SVS_CLIENTS);
        const uint32_t count = std::min<uint32_t>(PPC_LOAD_U32(T_DATA_Svs + T_SVS_CLIENT_COUNT), kSlots);
        for (uint32_t slot = 0; g_keptFrom && slot < count; ++slot) {
            const uint8_t* client = base + g_keptFrom + slot * T_SVCLIENT_SIZE;
            if (PPC_LOAD_U32(g_keptFrom + slot * T_SVCLIENT_SIZE) >= 3) g_kept.push_back({ slot, { client, client + T_SVCLIENT_SIZE } });
        }
        const PPCContext saved = ctx;
        ctx.r3.u64 = PPC_LOAD_U32(T_DATA_SvRunningDvar);
        ctx.r4.u64 = 0;
        GUEST_FUNC(T_Dvar_SetBool)(ctx, base);
        ctx = saved;
    }
    GUEST_ORIG(T_SV_ClearServer)(ctx, base);
}

// The new map's clients are allocated, empty: the ones kept go back, each
// with its netchan's buffers where it now is. SV_SpawnServer's own loop then
// connects them to the new level.
GUEST_HOOK(T_SV_Startup)
{
    GUEST_ORIG(T_SV_Startup)(ctx, base);
    if (!g_carrying) return;
    const uint32_t clients = PPC_LOAD_U32(T_DATA_Svs + T_SVS_CLIENTS);
    const uint32_t count = PPC_LOAD_U32(T_DATA_Svs + T_SVS_CLIENT_COUNT);
    for (const Kept& kept : g_kept) {
        if (kept.slot >= count) continue;
        const uint32_t client = clients + kept.slot * T_SVCLIENT_SIZE, was = g_keptFrom + kept.slot * T_SVCLIENT_SIZE;
        std::memcpy(base + client, kept.bytes.data(), T_SVCLIENT_SIZE);
        for (uint32_t at = client + T_SVCLIENT_NETCHAN; at < client + T_SVCLIENT_USERINFO; at += 4) {
            const uint32_t value = PPC_LOAD_U32(at);
            if (value == was + T_SVCLIENT_UNSENT_BUFFER || value == was + T_SVCLIENT_FRAGMENT_BUFFER) PPC_STORE_U32(at, value - was + client);
        }
    }
    LOGI("map vote: %zu clients go to the next map with the server", g_kept.size());
}

#endif

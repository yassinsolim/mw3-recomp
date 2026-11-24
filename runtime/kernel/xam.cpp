// XAM -- the dashboard-facing surface. Without an online service that carries
// lobbies, nothing here talks to anything like Xbox Live; the aim is to answer
// definitively so the title takes its offline path instead of polling or
// erroring, and to host the multiplayer's system-link session. With one
// (online::Live()), the player is signed in to Live and XAM stands in for the
// parts of it a private match needs: sessions, invitations, and Live storage
// kept on this machine.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "voice.h"
#include "../guest.h"
#include "../log.h"
#include "../friends.h"
#include "../presence.h"
#include "../signin.h"
#include "../online/demonware.h"
#include "../online/service.h"
#include "../title.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kLocalUser = 0;
    constexpr uint32_t kTitleId = T_TITLE_ID;

    // XOVERLAPPED. Not Win32's: the console's has the event at +12, which is
    // where the title's own XGetOverlappedResult looks for it, and it reads the
    // result from +0, the length from +4 and the extended error from +24.
    struct XOverlapped
    {
        be32 result;              // 0, ERROR_IO_PENDING while the call is running
        be32 length;              // 4, what XGetOverlappedResult hands back
        be32 context;             // 8
        be32 event;               // 12, handle to signal on completion
        be32 completionRoutine;   // 16
        be32 completionContext;   // 20
        be32 extendedError;       // 24
    };

    constexpr uint32_t kSignedInLocally = 1, kSignedInToLive = 2;
    constexpr uint32_t kFunctionFailed = 1627;   // ERROR_FUNCTION_FAILED
    constexpr uint32_t kNotLoggedOn = 1245;      // ERROR_NOT_LOGGED_ON

    // The player at the first controller: the profile there (signin.h), which
    // the online service knows by the same number -- plus one for a second copy
    // on this machine. An offline XUID (top byte 0xE0) over its 48 bits.
    uint64_t LocalAccount()
    {
        auto* service = online::Get();
        return (service ? service->Account() : signin::First().id) & 0xFFFFFFFFFFFFull;
    }
    uint64_t LocalXuid() { return 0xE000000000000000ull | LocalAccount(); }

    // The XUID Live knows an account by, the console's 0x0009 prefix over the
    // same 48 bits.
    uint64_t OnlineXuid(uint64_t account) { return 0x0009000000000000ull | (account & 0xFFFFFFFFFFFFull); }
    uint64_t LocalOnlineXuid() { return OnlineXuid(LocalAccount()); }

    std::string XuidText(uint64_t xuid)
    {
        char text[20];
        std::snprintf(text, sizeof text, "%016llx", (unsigned long long)xuid);
        return text;
    }

    // The name the service has for the player -- Steam's -- or the profile's.
    std::string LocalName()
    {
        auto* service = online::Get();
        std::string name = service ? service->LocalName() : std::string();
        if (name.empty()) name = signin::First().name;
        if (name.size() > 15) name.resize(15);   // a gamertag's longest
        return name;
    }

    // Who is at a controller. The first is the player above, signed in to
    // Live when the online service carries lobbies. The others are whoever
    // the sign-in screen put there (signin.h): a profile of this machine,
    // which is local and offline.
    struct Who
    {
        uint32_t state = 0;            // 0 nobody, 1 local, 2 Live
        uint64_t offline = 0, online = 0;
        std::string name;
    };
    Who WhoIs(uint32_t user)
    {
        Who who;
        if (user == kLocalUser)
        {
            const bool live = online::Live();
            who.state = live ? kSignedInToLive : kSignedInLocally;
            who.offline = LocalXuid();
            who.online = live ? LocalOnlineXuid() : 0;
            who.name = LocalName();
            return who;
        }
        const signin::Player player = signin::At(user);
        if (player.signedIn)
        {
            who.state = kSignedInLocally;
            who.offline = 0xE000000000000000ull | player.id;
            who.name = player.name;
        }
        return who;
    }
}

void kernel::CompleteOverlapped(uint32_t address, uint32_t result)
{
    CompleteOverlappedEx(address, result, result, 0);
}

void kernel::CompleteOverlappedEx(uint32_t address, uint32_t result, uint32_t extendedError,
                                  uint32_t length)
{
    auto* overlapped = GuestPtr<XOverlapped>(address);
    if (!overlapped) return;
    overlapped->result = result;
    overlapped->length = length;
    overlapped->extendedError = extendedError;
    if (uint32_t event = overlapped->event)
        if (auto signal = LookupHandleAs<Event>(event)) signal->Set();
}

// XNotify. A listener asks for areas of notification -- system, Live, friends
// -- and the title pumps it once a frame. The area of a notification is in its
// id's top bits.
namespace
{
    struct NotifyListener : Object
    {
        uint64_t areas = 0;
        std::mutex lock;
        std::deque<std::pair<uint32_t, uint32_t>> waiting;   // id, param
        const char* TypeName() const override { return "notify-listener"; }
    };

    std::mutex g_listenersLock;
    std::vector<std::weak_ptr<NotifyListener>> g_listeners;

    constexpr uint32_t XN_LIVE_CONNECTIONCHANGED = 0x02000001;
    constexpr uint32_t XN_LIVE_INVITE_ACCEPTED = 0x02000002;
    constexpr uint32_t X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED = 0x001510F0;
    constexpr uint64_t kAreaLive = 1ull << 1;
}

namespace
{
    uint64_t AreaOf(uint32_t id) { return 1ull << ((id >> 25) & 0x3F); }

    // False when nobody listens for it yet. The caller holds g_listenersLock.
    bool PostLocked(uint32_t id, uint32_t param)
    {
        bool heard = false;
        for (auto& weak : g_listeners)
            if (auto listener = weak.lock(); listener && (listener->areas & AreaOf(id)))
            {
                std::lock_guard l(listener->lock);
                listener->waiting.emplace_back(id, param);
                heard = true;
            }
        return heard;
    }

    // An invitation accepted before the title listens for Live -- as when the
    // console starts a game to take one up -- waits for its first listener.
    bool g_inviteUnheard = false;
}

void kernel::PostNotification(uint32_t id, uint32_t param)
{
    std::lock_guard g(g_listenersLock);
    PostLocked(id, param);
}

// (areas, version)
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    auto listener = std::make_shared<NotifyListener>();
    listener->areas = ctx.r3.u64;
    // Signed in to Live, the connection came up before the title was there to
    // hear it; the console tells a new listener so. The title asks for its NAT
    // type and measures its line when it hears it.
    if (online::Live() && (listener->areas & kAreaLive))
        listener->waiting.emplace_back(XN_LIVE_CONNECTIONCHANGED, X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED);
    {
        std::lock_guard g(g_listenersLock);
        std::erase_if(g_listeners, [](const auto& weak) { return weak.expired(); });
        g_listeners.push_back(listener);
        if (g_inviteUnheard && (listener->areas & AreaOf(XN_LIVE_INVITE_ACCEPTED)))
        {
            listener->waiting.emplace_back(XN_LIVE_INVITE_ACCEPTED, 0);
            g_inviteUnheard = false;
        }
    }
    ctx.r3.u64 = InsertHandle(listener);
}

// (listener, onlyThisId or 0, idOut, paramOut): 1 when one was taken.
PPC_FUNC(__imp__XNotifyGetNext)
{
    auto listener = LookupHandleAs<NotifyListener>(ctx.r3.u32);
    auto* id = GuestPtr<be32>(ctx.r5.u32);
    auto* param = GuestPtr<be32>(ctx.r6.u32);
    if (id) *id = 0;
    if (param) *param = 0;
    ctx.r3.u64 = 0;
    if (!listener) return;
    const uint32_t only = ctx.r4.u32;
    std::lock_guard l(listener->lock);
    for (auto it = listener->waiting.begin(); it != listener->waiting.end(); ++it)
    {
        if (only && it->first != only) continue;
        if (it->first == XN_LIVE_INVITE_ACCEPTED) LOGI("xam: the title heard of the accepted invitation");
        if (id) *id = it->first;
        if (param) *param = it->second;
        listener->waiting.erase(it);
        ctx.r3.u64 = 1;
        return;
    }
}

PPC_FUNC(__imp__XNotifyPositionUI) { ctx.r3.u64 = 0; }

// Zero for nobody, one for a local profile, two for a player on Live.
PPC_FUNC(__imp__XamUserGetSigninState)
{
    ctx.r3.u64 = WhoIs(ctx.r3.u32).state;
}

// (user, typeMask, xuidOut). Mask 1 asks for the offline XUID, 2 and 4 for the
// online one, which a player signed in to Live has.
PPC_FUNC(__imp__XamUserGetXUID)
{
    const Who who = WhoIs(ctx.r3.u32);
    if (!who.state) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (auto* xuid = GuestPtr<be64>(ctx.r5.u32)) *xuid = who.online && (ctx.r4.u32 & 6) ? who.online : who.offline;
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__XamUserGetName)
{
    const Who who = WhoIs(ctx.r3.u32);
    if (!who.state) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (auto* name = GuestPtr<char>(ctx.r4.u32))
    {
        uint32_t size = ctx.r5.u32;
        std::snprintf(name, size ? size : 1, "%s", who.name.c_str());
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// (user, flags, infoOut). Flags 1 asks for the online XUID only, 2 for the
// offline one; with neither, a Live player is named by the online one.
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    // XUSER_SIGNIN_INFO { xuid, flags, state, guestNumber, sponsorIndex, name[16] }
    const Who who = WhoIs(ctx.r3.u32);
    if (!who.state) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (auto* info = GuestPtr<be32>(ctx.r5.u32))
    {
        constexpr uint32_t kOfflineOnly = 2, kLiveEnabled = 1;
        std::memset(info, 0, 40);
        *GuestPtr<be64>(ctx.r5.u32) = who.online && !(ctx.r4.u32 & kOfflineOnly) ? who.online : who.offline;
        info[2] = who.online ? kLiveEnabled : 0;                      // flags
        info[3] = who.state;
        std::snprintf(reinterpret_cast<char*>(info) + 24, 16, "%s", who.name.c_str());
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// (user, privilege, result). Signed in to Live, the player has them all. A
// profile that is not on Live is not logged on, which the title tells from a
// privilege it was refused.
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    const uint32_t user = ctx.r3.u32;
    bool local = false;
    for (uint32_t index = 0; index < 4; index++)
        if (user == index || user == 0xFF) local |= WhoIs(index).state == kSignedInLocally;
    if (auto* result = GuestPtr<be32>(ctx.r5.u32)) *result = !local && WhoIs(user).state == kSignedInToLive ? 1 : 0;
    // Refused the communications privilege, the title registers no talker
    // for the player and never opens the headset (voice.cpp); said once,
    // since nothing else tells a player why voice chat does nothing.
    constexpr uint32_t kCommunications = 252;
    if (local && ctx.r4.u32 == kCommunications && voice::On())
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            LOGW("voice: the title keeps voice chat for players signed in to Xbox LIVE; "
                 "this one is not, so nobody hears or is heard (play through a relay, or set MW2_LIVE=1)");
        }
    }
    ctx.r3.u64 = local ? kNotLoggedOn : X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    if (auto* result = GuestPtr<be32>(ctx.r5.u32)) *result = 0;
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The profile settings calls are in profile.cpp.

PPC_FUNC(__imp__XamUserCreateStatsEnumerator){ ctx.r3.u64 = X_ERROR_NOT_FOUND; }

// (panes, flags). The console's sign-in screen, which is signin.h's here; the
// title hears of what was chosen as a change of who is signed in.
PPC_FUNC(__imp__XamShowSigninUI)
{
    signin::Ask();
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// Choosing where to save. This is the dialog behind "Unable to Write to Default
// Save Device": until it names a device the title has nowhere to put a save, so
// it asks again before every level.
//
// The title accepts exactly one answer from this call. It is asynchronous, and
// the caller tests the return for ERROR_IO_PENDING and then polls
// XGetOverlappedResult; anything else -- success included -- it reads as a
// refusal and goes back to the prompt. So the choice is made here and reported
// through the overlapped block, and the call itself says "pending".
//
// (userIndex, contentType, contentFlags, bytesNeeded, deviceIdOut, overlapped)
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    // The hard disk, as the content layer in content.cpp reports it.
    if (auto* deviceId = GuestPtr<be32>(ctx.r7.u32)) *deviceId = kSaveDeviceId;
    const uint32_t overlapped = ctx.r8.u32;
    if (!overlapped) { ctx.r3.u64 = X_ERROR_SUCCESS; return; }
    CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
}
PPC_FUNC(__imp__XamShowMessageBoxUIEx)     { ctx.r3.u64 = X_ERROR_NOT_FOUND; }
// The lobby's "Invite friends": the friends screen (friends.h).
PPC_FUNC(__imp__XamShowFriendsUI)
{
    friends::Ask();
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
PPC_FUNC(__imp__XamShowGamerCardUIForXUID) { ctx.r3.u64 = X_ERROR_SUCCESS; }
PPC_FUNC(__imp__XamShowDirtyDiscErrorUI)
{
    LOGE("the title reported a dirty disc and asked for the error UI");
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XAM's message ports. Everything the title asks of the multiplayer services --
// sessions, matchmaking, statistics -- arrives here as (app, message, buffer)
// rather than as its own import, so an entry point named XSessionCreate in the
// engine is app 0xFB message 0x000B0010 by the time it reaches the kernel.
// Answering "success" and writing nothing is not neutral: the title reads the
// session back out and gives up on what it finds.
namespace
{
    constexpr uint32_t kAppXGI = 0xFB;        // the game-invite / session app
    constexpr uint32_t kAppXLiveBase = 0xFC;  // Live itself: storage, invitations, NAT

    // What a message answers: its result, and for an asynchronous one the
    // extended error the caller reads from the overlapped block.
    struct Reply
    {
        uint32_t result = X_ERROR_SUCCESS;
        uint32_t extended = X_ERROR_SUCCESS;
        bool pending = false;   // the overlapped block is completed later
    };
    Reply Result(uint32_t result) { return { result, result }; }

    // XSESSION_INFO, the block XSessionCreate fills in for the caller. The
    // console's, so the same in MW3's multiplayer: its XSessionCreate
    // (sub_82511270) sends the same 28 bytes as 000B0010, and its session
    // keeps the 60 bytes at +0x14 (copied with a length of 0x3C at 823B0874)
    // and the nonce at +0x58.
    struct XSessionInfo
    {
        uint8_t sessionId[8];      // 0,  XNKID
        uint8_t hostAddress[36];   // 8,  XNADDR
        uint8_t keyExchangeKey[16];// 44, XNKEY
    };
    static_assert(sizeof(XSessionInfo) == online::kSessionInfoSize, "XSESSION_INFO is 60 bytes");

    std::string Hex(const uint8_t* p, size_t n)
    {
        std::string text;
        char pair[3];
        for (size_t i = 0; i < n; i++)
        {
            std::snprintf(pair, sizeof pair, "%02x", p[i]);
            text += pair;
        }
        return text;
    }

    void RandomBytes(uint8_t* p, size_t n)
    {
        thread_local std::mt19937 rng{ std::random_device{}() };
        for (size_t i = 0; i < n; i++) p[i] = uint8_t(rng());
    }

    // XSESSION_CREATE_* flags.
    constexpr uint32_t kHost = 0x01, kPresence = 0x02, kStats = 0x04, kMatchmaking = 0x08;
    constexpr uint32_t kInvitesDisabled = 0x100, kJoinInProgressDisabled = 0x400;
    constexpr uint32_t kLiveFeatures = 0x02 | 0x04 | 0x08 | 0x10;   // presence, stats, matchmaking, arbitration

    // Every session the title has made, by the session object it names them
    // with: what another machine needs to join one, and the order they came in.
    struct Session
    {
        uint32_t flags = 0;
        XSessionInfo info{};
        uint32_t publicSlots = 0, privateSlots = 0;
        uint8_t nonce[8]{};
        uint64_t made = 0;
        bool systemLink = false;   // a system-link game this machine hosts
        bool on = false;           // and it is on: from its creation or start to its end
        std::map<uint64_t, bool> members;   // who joined, and whether in a private slot
    };
    std::mutex g_sessionLock;
    std::map<uint32_t, Session> g_sessions;
    uint64_t g_sessionsMade = 0;

    // The contexts and properties the title's searches compare, as the player
    // last set them: the game mode, ranked or not and the playlist; the
    // protocol, the map packs, the playlists' version and the room for a party.
    // What Live kept with a session comes from these on the console too.
    constexpr uint32_t kPartyRoom = 0x1000001A;
    constexpr uint32_t kSearched[] = { 0x800B, 0x800A, 0x0006, 0x10000014, 0x10000015, 0x10000017, kPartyRoom };
    std::map<uint32_t, uint32_t> g_searchedNumbers;
    std::mutex g_listingLock;              // held from choosing what to list to telling the service
    std::array<uint8_t, 8> g_joinable{};   // the id of the session last handed to the service
    std::mutex g_advertLock;               // held from choosing what to advertise to telling the service
    std::string g_advert;                  // the connect line last told it

    // A new identity for a session this machine hosts, the way the console
    // makes one. The top bits of the id say what kind of session it is: clear
    // for system link, 0x80 for one peers hold over Live. A session id of all
    // zeroes is what the title reads as "the service never gave us one".
    // A player sent to a friend's private match gets the flags of a Find Game
    // session, as they are the host's to know. So the host says which it is in the
    // id, which every player is given however they came and which stays through
    // a migration.
    constexpr uint32_t kFindGameSession = kStats | kMatchmaking;
    constexpr uint8_t kFindGameId = 0x01;   // in the id's last byte
    bool IsFindGame(const Session& s)
    {
        return (s.flags & kFindGameSession) == kFindGameSession && (s.info.sessionId[7] & kFindGameId);
    }

    void MakeHostInfo(XSessionInfo& info, bool live, bool findGame)
    {
        RandomBytes(info.sessionId, sizeof info.sessionId);
        info.sessionId[0] = live ? uint8_t(0x80 | (info.sessionId[0] & 0x0F))
                                 : uint8_t((info.sessionId[0] & 0x7F) | 0x01);
        info.sessionId[7] = uint8_t((info.sessionId[7] & ~kFindGameId) | (findGame ? kFindGameId : 0));
        kernel::TitleXnAddr(info.hostAddress);
        RandomBytes(info.keyExchangeKey, sizeof info.keyExchangeKey);
    }

    // What another machine types to join a system-link game: the console
    // command the title's own server browser issues, with what the host's reply
    // to a search carries -- XNADDR, key and id in hex, the slots and the nonce.
    std::string ConnectLine(const Session& s)
    {
        return "connect " + Hex(s.info.hostAddress, 36) + " " + Hex(s.info.keyExchangeKey, 16) + " " +
               Hex(s.info.sessionId, 8) + " " + std::to_string(s.publicSlots) + " " +
               std::to_string(s.privateSlots) + " " + Hex(s.nonce, 8);
    }

    // The system-link game this machine hosts: the newest such session that
    // is on, from its creation (the server spawning a map) or start to its
    // end (the server shutting down, or the map ending). Told to the service
    // whenever the sessions change, under one lock from the choice to the
    // telling, so that two threads cannot tell it in the other order.
    void PublishHosted()
    {
        auto* service = online::Get();
        if (!service) return;
        std::lock_guard a(g_advertLock);
        std::string line;
        {
            std::lock_guard g(g_sessionLock);
            const Session* best = nullptr;
            for (const auto& [object, s] : g_sessions)
                if (s.systemLink && s.on && (!best || s.made > best->made)) best = &s;
            if (best) line = ConnectLine(*best);
        }
        if (line == g_advert) return;
        g_advert = line;
        if (line.empty())
            LOGI("online: no longer hosting");
        else
            LOGI("online: hosting; another machine joins with \"%s\"", line.c_str());
        service->Advertise(line);
    }

    // What matchmaking lists: the newest session this machine hosts that uses
    // matchmaking and stats, which the party's own session (0x29) does not. A
    // session that takes nobody once it has started leaves the list then, as it
    // leaves Live's. A lobby is listed from its creation, with no public slots
    // until the title's searches have failed and it opens them.
    void PublishListing()
    {
        auto* service = online::Get();
        if (!service || !service->Matchmaking()) return;
        std::lock_guard a(g_listingLock);
        online::SessionListing listing;
        const Session* best = nullptr;
        bool playing = false;
        {
            std::lock_guard g(g_sessionLock);
            constexpr uint32_t kListed = kHost | kStats | kMatchmaking;
            for (const auto& [object, s] : g_sessions)
            {
                playing |= IsFindGame(s);
                if (IsFindGame(s) && (s.flags & kListed) == kListed && !(s.on && (s.flags & kJoinInProgressDisabled)) &&
                    (!best || s.made > best->made))
                    best = &s;
            }
            if (best)
            {
                std::memcpy(listing.session, &best->info, sizeof listing.session);
                for (const auto& [xuid, inPrivate] : best->members)
                    (inPrivate ? listing.filledPrivate : listing.filledPublic)++;
                listing.openPublic = best->publicSlots - std::min(best->publicSlots, listing.filledPublic);
                listing.openPrivate = best->privateSlots - std::min(best->privateSlots, listing.filledPrivate);
                for (const auto& [id, value] : g_searchedNumbers) listing.numbers.push_back({ id, value });
            }
        }
        service->List(best ? &listing : nullptr);
        service->InGame(playing);
    }

    // What a friend joins over Live: the newest session this machine hosts that
    // shows in its presence and takes invitations -- the party's while the
    // player is in the Xbox LIVE menu, the private match's once in its lobby.
    // Told to the service whenever the sessions change. The console line is
    // the title's own way in to the same place, logged for testing.
    void PublishJoinable()
    {
        if (!online::Live()) return;
        const Session* best = nullptr;
        online::Joinable joinable;
        {
            std::lock_guard g(g_sessionLock);
            for (const auto& [object, s] : g_sessions)
                if ((s.flags & kHost) && (s.flags & kPresence) && !(s.flags & kInvitesDisabled) &&
                    (!best || s.made > best->made))
                    best = &s;
            std::array<uint8_t, 8> id{};
            if (best) std::memcpy(id.data(), best->info.sessionId, 8);
            if (id == g_joinable) return;
            g_joinable = id;
            if (best)
            {
                std::memcpy(joinable.session, &best->info, sizeof best->info);
                joinable.publicSlots = best->publicSlots;
                joinable.privateSlots = best->privateSlots;
                std::memcpy(joinable.nonce, best->nonce, sizeof joinable.nonce);
            }
        }
        if (best)
        {
            const auto& info = *reinterpret_cast<const XSessionInfo*>(joinable.session);
            LOGI("online: joinable; another machine joins with \"xpartyjoin %s %s %s\"",
                 Hex(info.hostAddress, 36).c_str(), Hex(info.keyExchangeKey, 16).c_str(),
                 Hex(info.sessionId, 8).c_str());
        }
        else
            LOGI("online: nothing to join");
        online::Get()->SetJoinable(best ? &joinable : nullptr);
    }

    // (session, flags, publicSlots, privateSlots, xuid, sessionInfoOut, nonceOut)
    Reply XgiSessionCreate(uint32_t buffer, uint32_t length)
    {
        if (length < 28) return Result(X_ERROR_INVALID_PARAMETER);
        const auto* words = GuestPtr<be32>(buffer);
        if (!words) return Result(X_ERROR_INVALID_PARAMETER);
        const uint32_t object = words[0], flags = words[1];

        // A session this console hosts on its own -- system link -- gets an
        // identity the title invents no differently than the console would.
        // One that wants Xbox Live needs a service that carries lobbies; without
        // one it is refused, and the title falls back to the offline path
        // instead of waiting on a service that never answers.
        const bool live = flags & kLiveFeatures;
        if (live && !online::Live())
        {
            LOGI("online: session %08x refused, flags %x: it asks for Xbox LIVE and the player is not signed in to it",
                 object, flags);
            return Result(0x80155209);   // X_ONLINE_E_SESSION_NOT_LOGGED_ON
        }

        auto* info = GuestPtr<XSessionInfo>(words[5]);
        auto* nonce = GuestPtr<uint8_t>(words[6]);
        // Joining someone else's session: the title passes the host's session
        // info and nonce in, from the game it found, and reads them back.
        const bool host = !flags || (flags & kHost);
        if (host)
        {
            if (info) MakeHostInfo(*info, live, (flags & kFindGameSession) == kFindGameSession);
            if (nonce) RandomBytes(nonce, 8);
        }
        Session s;
        s.flags = flags;
        if (info) s.info = *info;
        if (nonce) std::memcpy(s.nonce, nonce, 8);
        s.publicSlots = words[2];
        s.privateSlots = words[3];
        s.systemLink = s.on = host && info && nonce && !live;
        {
            std::lock_guard g(g_sessionLock);
            s.made = ++g_sessionsMade;
            g_sessions[object] = s;
        }
        if (info) kernel::NoteSessionId(info->sessionId);
        LOGI("online: session %08x created, flags %x, %u public and %u private slots", object, flags, s.publicSlots, s.privateSlots);
        // What the guest will knock on: the session's kind (Live sessions
        // carry 0x80 in the id's top bits, system link does not) and the
        // host's address out of its XNADDR. When a join loops on the title's
        // "attempt #N", this line against the host's "lan as ..." line says
        // whether the guest even has the right kind and door.
        if (!host && info)
        {
            const char* kind = (info->sessionId[0] & 0x80) ? "Live" : "system-link";
            if (uint64_t peer; kernel::XnAddrPeer(info->hostAddress, peer))
            {
                const uint32_t ip = uint32_t(peer >> 16);
                LOGI("online: joining a %s session, host %u.%u.%u.%u:%u", kind,
                     (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                     unsigned(peer & 0xFFFF));
            }
            else
                LOGI("online: joining a %s session, host address untagged (not this build's)", kind);
        }
        PublishHosted();
        PublishJoinable();
        PublishListing();
        return {};
    }

    // (session, info, user, ...): the session has another host. With a player's
    // index that is this machine, which fills in `info` for the title to send the
    // others; with XUSER_INDEX_NONE `info` is the new host's. Only the host's
    // address changes: everyone keeps the session's id and key.
    Reply XgiSessionMigrateHost(uint32_t buffer, uint32_t length)
    {
        const auto* words = GuestPtr<be32>(buffer);
        if (!words || length < 12) return Result(X_ERROR_INVALID_PARAMETER);
        auto* info = GuestPtr<XSessionInfo>(words[1]);
        if (!info) return Result(X_ERROR_INVALID_PARAMETER);
        const bool host = words[2] < 4;
        {
            std::lock_guard g(g_sessionLock);
            const auto it = g_sessions.find(words[0]);
            if (it == g_sessions.end()) return Result(X_ERROR_INVALID_PARAMETER);
            Session& s = it->second;
            if (host)
            {
                kernel::TitleXnAddr(s.info.hostAddress);
                *info = s.info;
                s.flags |= kHost;
            }
            else
            {
                s.info = *info;
                s.flags &= ~kHost;
            }
        }
        LOGI("online: session %08x migrated, this machine %s", uint32_t(words[0]), host ? "hosts it now" : "follows the new host");
        PublishJoinable();
        PublishListing();
        if (uint64_t peer; !host && kernel::XnAddrPeer(info->hostAddress, peer))
            if (auto* service = online::Get()) service->HostMoved(peer);
        return {};
    }

    void SetOn(uint32_t object, bool on)
    {
        {
            std::lock_guard g(g_sessionLock);
            const auto it = g_sessions.find(object);
            if (it == g_sessions.end()) return;
            it->second.on = on;
        }
        PublishHosted();
        PublishListing();
    }

    // (session, flags, ...)
    Reply XgiSessionStart(uint32_t buffer)
    {
        if (const auto* words = GuestPtr<be32>(buffer)) SetOn(words[0], true);
        return {};
    }

    // (session, 0)
    Reply XgiSessionEnd(uint32_t buffer)
    {
        if (const auto* words = GuestPtr<be32>(buffer)) SetOn(words[0], false);
        return {};
    }

    // (session, ...): the session goes, and with it what a friend could join.
    Reply XgiSessionDelete(uint32_t buffer)
    {
        if (const auto* words = GuestPtr<be32>(buffer))
        {
            std::lock_guard g(g_sessionLock);
            g_sessions.erase(words[0]);
        }
        PublishHosted();
        PublishJoinable();
        PublishListing();
        return {};
    }

    // (session, count, xuids, 0, private flags): players joining this session
    // on this machine's say-so. It counts them, which is all Live did for the
    // slots a search sees.
    Reply XgiSessionJoinRemote(uint32_t buffer, uint32_t length)
    {
        const auto* words = GuestPtr<be32>(buffer);
        if (!words || length < 20) return Result(X_ERROR_INVALID_PARAMETER);
        const auto* xuids = GuestPtr<be64>(words[2]);
        const auto* inPrivate = GuestPtr<be32>(words[4]);
        {
            std::lock_guard g(g_sessionLock);
            const auto it = g_sessions.find(words[0]);
            if (it != g_sessions.end() && xuids)
                for (uint32_t i = 0; i < words[1]; i++)
                    it->second.members[uint64_t(xuids[i])] = inPrivate && uint32_t(inPrivate[i]) != 0;
        }
        PublishListing();
        return {};
    }

    // (session, count, xuids, 0, 0)
    Reply XgiSessionLeaveRemote(uint32_t buffer, uint32_t length)
    {
        const auto* words = GuestPtr<be32>(buffer);
        if (!words || length < 12) return Result(X_ERROR_INVALID_PARAMETER);
        const auto* xuids = GuestPtr<be64>(words[2]);
        {
            std::lock_guard g(g_sessionLock);
            const auto it = g_sessions.find(words[0]);
            if (it != g_sessions.end() && xuids)
                for (uint32_t i = 0; i < words[1]; i++) it->second.members.erase(uint64_t(xuids[i]));
        }
        PublishListing();
        return {};
    }

    void NoteSearched(uint32_t id, uint32_t value)
    {
        const bool searched = std::find(std::begin(kSearched), std::end(kSearched), id) != std::end(kSearched);
        if (online::Trace())
            LOGI("net-trace: the title set %08x to %u%s", id, value, searched ? ", which a search compares" : "");
        if (!searched) return;
        {
            std::lock_guard g(g_sessionLock);
            g_searchedNumbers[id] = value;
        }
        PublishListing();
    }

    // (user, pad, xuid, property, size, value): the title sets one the way it
    // sets a context, and only numbers of 32 bits are compared.
    Reply XgiUserSetProperty(uint32_t buffer)
    {
        const auto* words = GuestPtr<be32>(buffer);
        if (!words) return Result(X_ERROR_INVALID_PARAMETER);
        if (words[5] == 4)
            if (const auto* value = GuestPtr<be32>(words[6])) NoteSearched(words[4], *value);
        return {};
    }

    // Searches the title is waiting on, and what it asked for.
    struct Search
    {
        uint32_t id = 0, overlapped = 0, results = 0, capacity = 0, most = 0;
#ifdef COD4_TITLE_MP
        // Call of Duty 4 searches once and, finding nothing, opens a lobby of
        // its own that it never leaves for another: two players who search
        // at the same moment each find nothing and wait in a lobby each, for
        // good (seen with an AYN Thor and a Retroid Pocket Flip 2). So a
        // search that finds nothing is asked again every kAskAgain until
        // `until`, which is later the more of the copies heard on the
        // network come before this one: the first of them opens its lobby at
        // once, and the others find it.
        online::SessionSearch asked;
        std::chrono::steady_clock::time_point sent, until;
#endif
    };
#ifdef COD4_TITLE_MP
    constexpr auto kAskAgain = std::chrono::seconds(1);       // the relay takes one search a second
    constexpr auto kWaitForEach = std::chrono::seconds(2);    // a lobby is listed and found within it
    constexpr size_t kMostWaitedFor = 3;
#endif
    std::mutex g_searchLock;
    std::vector<Search> g_searches;

    constexpr uint32_t kFindingSize = 92;   // XSESSION_SEARCHRESULT

    // (procedure, user, most, properties << 16 | contexts, properties, contexts,
    // size of the results, results, users). The search's meaning was Live's:
    // the procedure named a query on its servers, so what is asked for here is
    // what the title passes, and each must match. The title sends two
    // procedures, which it fills in alike. The numbers that are no integers
    // are skill bounds, which nothing here keeps, and the room for a party is a
    // lower bound.
    //
    // MW3 sends XSessionSearchWeighted, whose message has a second pair of
    // counts and lists before the size: what Live ordered its answer by, most
    // alike first. Nothing here orders, so of those the ones a lobby must
    // share to be the same game have to match -- kWeighed, which the title
    // has all set by the time it lists a lobby of its own. It sets the others
    // later or not at all (0x800B once it is in a game), and a listing
    // without a number that is asked for is refused.
    constexpr uint32_t kWeighed[] = { 0x800A, 0x0006, 0x10000014, 0x10000017 };
    Reply XgiSessionSearch(uint32_t buffer, uint32_t length, uint32_t overlapped, bool weighted)
    {
        const uint32_t size = weighted ? 9 : 6;   // the word the size of the results is in
        const auto* words = GuestPtr<be32>(buffer);
        if (!words || length < (size + 3) * 4) return Result(X_ERROR_INVALID_PARAMETER);
        auto* service = online::Get();
        // The title's wrapper has cleared the header: nothing found.
        if (!service || !service->Matchmaking() || !overlapped) return {};

        online::SessionSearch search;
        search.most = words[2];
        // In a weighted search the lists that matter are the weighted ones:
        // the plain ones hold only the skill.
        const uint32_t counts = words[weighted ? 6 : 3];
        const auto* properties = GuestPtr<uint8_t>(words[weighted ? 7 : 4]);
        const auto* contexts = GuestPtr<be32>(words[weighted ? 8 : 5]);
        const auto compared = [&](uint32_t id) {
            return !weighted || std::find(std::begin(kWeighed), std::end(kWeighed), id) != std::end(kWeighed);
        };
        for (uint32_t i = 0; contexts && i < (counts & 0xFFFF); i++)
            if (compared(contexts[2 * i])) search.wants.push_back({ contexts[2 * i], contexts[2 * i + 1], false });
        // XUSER_PROPERTY { id, pad, XUSER_DATA { type at 8, value at 16 } }
        for (uint32_t i = 0; properties && i < (counts >> 16); i++)
        {
            const uint8_t* property = properties + 24 * i;
            if (property[8] != 1) continue;
            const uint32_t id = *reinterpret_cast<const be32*>(property);
            const uint32_t value = *reinterpret_cast<const be32*>(property + 16);
            if (id == kPartyRoom) search.room = value;
            if (compared(id)) search.wants.push_back({ id, value, id == kPartyRoom });
        }
        kernel::ForgetSessionLags();
        // The title polls this block, and takes anything but pending as the
        // answer, in the frame after asking.
        GuestPtr<XOverlapped>(overlapped)->result = X_ERROR_IO_PENDING;
        const uint32_t id = service->Search(search);
        {
            std::lock_guard g(g_searchLock);
            g_searches.push_back({ id, overlapped, words[size + 1], words[size], words[2] });
#ifdef COD4_TITLE_MP
            Search& kept = g_searches.back();
            kept.asked = search;
            kept.sent = std::chrono::steady_clock::now();
            kept.until = kept.sent + kWaitForEach * std::min(service->PeersBefore(), kMostWaitedFor);
#endif
        }
        LOGI("online: searching for sessions, procedure %u, room for %u", uint32_t(words[0]), search.room);
        if (online::Trace())
            for (const online::SessionWant& want : search.wants)
                LOGI("net-trace: the search wants %08x %s %u", want.id, want.atLeast ? "at least" : "to be", want.value);
        Reply reply;
        reply.pending = true;
        return reply;
    }

    // XSESSION_SEARCHRESULT_HEADER, then the results:
    // { XSESSION_INFO, open public, open private, filled public, filled private,
    //   properties, contexts, their pointers }. The title reads no property a
    // search result can leave out. The host's address is rebuilt from the id
    // the service gives for the listing: a relay stamps it, so nobody can list a
    // session in another player's name there. A Steam listing says it itself.
    void AnswerSearch(const Search& search, const std::vector<online::SessionFound>& found)
    {
        uint32_t count = 0;
        if (auto* header = GuestPtr<be32>(search.results); header && search.capacity >= 8)
        {
            const uint32_t first = search.results + 8;
            for (const online::SessionFound& one : found)
            {
                if (count >= search.most || 8 + (count + 1) * kFindingSize > search.capacity) break;
                auto* p = GuestPtr<uint8_t>(first + count * kFindingSize);
                std::memset(p, 0, kFindingSize);
                std::memcpy(p, one.session, sizeof one.session);
                kernel::SetXnAddrPeer(p + 8, one.host);
                auto* slots = reinterpret_cast<be32*>(p + 60);
                slots[0] = one.openPublic;
                slots[1] = one.openPrivate;
                slots[2] = one.filledPublic;
                slots[3] = one.filledPrivate;
                kernel::NoteSessionLag(one.session, one.lag);
                count++;
            }
            header[0] = count;
            header[1] = first;
        }
        LOGI("online: search found %u sessions", count);
        CompleteOverlappedEx(search.overlapped, X_ERROR_SUCCESS, X_ERROR_SUCCESS, 0);
    }

    // (session, flags, publicSlots, privateSlots)
    Reply XgiSessionModify(uint32_t buffer, uint32_t length)
    {
        const auto* words = GuestPtr<be32>(buffer);
        if (!words || length < 16) return Result(X_ERROR_INVALID_PARAMETER);
        {
            std::lock_guard g(g_sessionLock);
            auto it = g_sessions.find(words[0]);
            if (it != g_sessions.end())
            {
                // The flags that say who hosts and what the session uses do not
                // change here; the rest can.
                constexpr uint32_t kFixed = 0x3F;
                it->second.flags = (it->second.flags & kFixed) | (words[1] & ~kFixed);
                it->second.publicSlots = words[2];
                it->second.privateSlots = words[3];
            }
        }
        PublishHosted();   // the slots are in the connect line
        PublishJoinable();
        PublishListing();
        return {};
    }

    // Live storage, kept on this machine under saves/online: the title's own
    // files (title/, which nothing here publishes, so each is there and empty
    // -- the playlists and the message of the day), and each player's
    // (user/<xuid>/, the multiplayer rank and stats the title uploads).
    //
    // The console names them by server path, which XStorageBuildServerPath
    // makes and the title passes back: //title.<id>/t:<id>/<file> and
    // //tuser.<id>/u:<xuid>/<id>/<file>.
    constexpr uint32_t kFacilityPerTitle = 2, kFacilityPerUserTitle = 3;
    constexpr uint32_t X_ONLINE_E_STORAGE_FILE_NOT_FOUND = 0x8015C004;

    struct StorageFile
    {
        fs::path path;
        bool title = false;
    };

    bool StorageName(const std::string& name)
    {
        return !name.empty() && name != "." && name != ".." &&
               name.find_first_of("/\\:") == std::string::npos;
    }

    bool StoragePath(const std::string& server, StorageFile& out)
    {
        char title[16];
        std::snprintf(title, sizeof title, "%08x", kTitleId);
        const std::string titleRoot = std::string("//title.") + title + "/t:" + title + "/";
        const std::string userRoot = std::string("//tuser.") + title + "/u:";
        const fs::path root = SaveRoot() / "online";
        if (server.rfind(titleRoot, 0) == 0)
        {
            const std::string name = server.substr(titleRoot.size());
            if (!StorageName(name)) return false;
            out = { root / "title" / name, true };
            return true;
        }
        if (server.rfind(userRoot, 0) == 0)
        {
            const std::string rest = server.substr(userRoot.size());   // <xuid>/<title>/<file>
            const size_t a = rest.find('/'), b = a == std::string::npos ? a : rest.find('/', a + 1);
            if (b == std::string::npos || a != 16 || rest.compare(a + 1, b - a - 1, title) != 0)
                return false;
            const std::string xuid = rest.substr(0, a), name = rest.substr(b + 1);
            if (xuid.find_first_not_of("0123456789abcdef") != std::string::npos || !StorageName(name))
                return false;
            out = { root / "user" / xuid / name, false };
            return true;
        }
        return false;
    }

    std::string WideToAscii(const be16* p, size_t count)
    {
        std::string text;
        for (size_t i = 0; i < count && p[i]; i++)
        {
            const uint16_t c = p[i];
            text += c < 0x80 ? char(c) : '?';
        }
        return text;
    }

    // (XSTORAGE_BUILD_SERVER_PATH*): { user, xuid@8, facility@16, info@20,
    // infoSize@24, fileName@28, pathOut@32, pathLengthInOut@36 }
    Reply StorageBuildServerPath(uint32_t buffer)
    {
        auto* p = GuestPtr<uint8_t>(buffer);
        if (!p) return Result(0x80070057);   // E_INVALIDARG
        const uint32_t user = *reinterpret_cast<be32*>(p);
        const uint64_t named = *reinterpret_cast<be64*>(p + 8);
        const uint32_t facility = *reinterpret_cast<be32*>(p + 16);
        const uint32_t fileName = *reinterpret_cast<be32*>(p + 28);
        const uint32_t pathOut = *reinterpret_cast<be32*>(p + 32);
        auto* length = GuestPtr<be32>(*reinterpret_cast<be32*>(p + 36));
        const std::string name = GuestWideToUtf8(fileName, 256);
        if (!length || name.empty()) return Result(0x80070057);

        char path[256];
        if (facility == kFacilityPerTitle)
            std::snprintf(path, sizeof path, "//title.%08x/t:%08x/%s", kTitleId, kTitleId, name.c_str());
        else if (facility == kFacilityPerUserTitle)
        {
            const uint64_t xuid = user == kLocalUser ? LocalOnlineXuid() : named;
            std::snprintf(path, sizeof path, "//tuser.%08x/u:%016llx/%08x/%s", kTitleId,
                          (unsigned long long)xuid, kTitleId, name.c_str());
        }
        else
            return Result(0x80155301);   // X_ONLINE_E_STORAGE_INVALID_FACILITY

        const size_t chars = std::strlen(path) + 1;
        if (auto* out = GuestPtr<be16>(pathOut))
        {
            const size_t room = std::min<size_t>(*length, chars);
            for (size_t i = 0; i < room; i++) out[i] = i + 1 < chars ? uint16_t(uint8_t(path[i])) : 0;
        }
        *length = uint32_t(chars);
        return {};
    }

    // XLIVEBASE_ASYNC_MESSAGE { task }, and the task's marshalled request:
    // { user, pathChars, path (UTF-16), bufferSize, buffer }.
    struct StorageRequest
    {
        std::string path;
        uint32_t size = 0, buffer = 0;
        uint32_t results = 0, resultsSize = 0;
    };

    bool ReadStorageRequest(uint32_t message, StorageRequest& out)
    {
        const auto* async = GuestPtr<be32>(message);
        const auto* task = async ? GuestPtr<uint8_t>(async[0]) : nullptr;
        if (!task) return false;
        const uint32_t request = *reinterpret_cast<const be32*>(task + 0x18);
        const uint32_t requestSize = *reinterpret_cast<const be32*>(task + 0x1C);
        out.results = *reinterpret_cast<const be32*>(task + 0x2C);
        out.resultsSize = *reinterpret_cast<const be32*>(task + 0x30);
        const auto* words = GuestPtr<uint8_t>(request);
        if (!words || requestSize < 16) return false;
        const uint32_t chars = *reinterpret_cast<const be32*>(words + 4);
        if (8 + size_t(chars) * 2 + 8 > requestSize) return false;
        out.path = WideToAscii(reinterpret_cast<const be16*>(words + 8), chars);
        const uint8_t* tail = words + 8 + size_t(chars) * 2;
        out.size = *reinterpret_cast<const be32*>(tail);
        out.buffer = *reinterpret_cast<const be32*>(tail + 4);
        return true;
    }

    // XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS { bytesTotal, xuidOwner, ftCreated }
    Reply StorageDownload(uint32_t message)
    {
        StorageRequest request;
        StorageFile file;
        if (!ReadStorageRequest(message, request) || !StoragePath(request.path, file))
        {
            LOGW("online: storage download of an unknown path \"%s\"", request.path.c_str());
            return { kFunctionFailed, X_ONLINE_E_STORAGE_FILE_NOT_FOUND };
        }
        std::vector<char> bytes;
        std::error_code ec;
        std::vector<uint8_t> shared;
        auto* service = online::Get();
        if (file.title && service && service->TitleFile(file.path.filename().string(), shared))
            bytes.assign(shared.begin(), shared.end());
        else if (fs::exists(file.path, ec))
        {
            std::ifstream in(file.path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        else if (!file.title)
        {
            LOGI("online: storage has no %s yet", request.path.c_str());
            return { kFunctionFailed, X_ONLINE_E_STORAGE_FILE_NOT_FOUND };
        }
        if (bytes.size() > request.size)
        {
            LOGW("online: %s has %zu bytes, and the title has room for %u", request.path.c_str(), bytes.size(), request.size);
            return { kFunctionFailed, 0x8007007A };   // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
        }
        if (auto* to = GuestPtr<char>(request.buffer); to && !bytes.empty())
            std::memcpy(to, bytes.data(), bytes.size());
        if (auto* results = GuestPtr<uint8_t>(request.results); results && request.resultsSize >= 20)
        {
            std::memset(results, 0, 20);
            *reinterpret_cast<be32*>(results) = uint32_t(bytes.size());
            const uint64_t owner = file.title ? 0 : LocalOnlineXuid();
            for (int i = 0; i < 8; i++) results[4 + i] = uint8_t(owner >> (56 - 8 * i));
        }
        LOGI("online: storage download %s, %zu bytes", request.path.c_str(), bytes.size());
        return {};
    }

    Reply StorageUpload(uint32_t message)
    {
        StorageRequest request;
        StorageFile file;
        const char* from = nullptr;
        if (!ReadStorageRequest(message, request) || !StoragePath(request.path, file) || file.title ||
            !(from = GuestPtr<const char>(request.buffer)))
        {
            LOGW("online: storage upload refused for \"%s\"", request.path.c_str());
            return Result(0x80070005);   // E_ACCESSDENIED
        }
        std::error_code ec;
        fs::create_directories(file.path.parent_path(), ec);
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out.write(from, request.size);
        if (!out) return Result(0x80004005);   // E_FAIL
        LOGI("online: storage upload %s, %u bytes", request.path.c_str(), request.size);
        return {};
    }

    // The invitation the player last accepted, as XINVITE_INFO:
    // { xuidInvitee, xuidInviter, titleId, hostInfo (XSESSION_INFO), fromGameInvite }.
    struct AcceptedInvite
    {
        bool any = false;
        uint8_t bytes[84]{};
    };
    std::mutex g_inviteLock;
    AcceptedInvite g_invite;

    // (task, X_ARGUMENT_LIST*): entries of { size, value } where value 0 points
    // at the user index and value 1 at the XINVITE_INFO to fill.
    Reply InviteGetAcceptedInfo(uint32_t arguments)
    {
        const auto* list = GuestPtr<uint8_t>(arguments);
        if (!list) return Result(0x80070057);
        const uint32_t userAt = uint32_t(*reinterpret_cast<const be64*>(list + 8));
        const uint32_t infoAt = uint32_t(*reinterpret_cast<const be64*>(list + 16 + 8));
        const auto* user = GuestPtr<be32>(userAt);
        auto* info = GuestPtr<uint8_t>(infoAt);
        if (!user || !info || *user != kLocalUser) return Result(0x80070057);
        std::lock_guard g(g_inviteLock);
        if (!g_invite.any) return Result(0x80155200);   // X_ONLINE_E_SESSION_NOT_FOUND
        std::memcpy(info, g_invite.bytes, sizeof g_invite.bytes);
        LOGI("xam: the title read the accepted invitation");
        return {};
    }

    // XOnlineGetServiceInfo(service, XONLINE_SERVICE_INFO* out): whether the
    // player's logon to Live includes a service, and where it is:
    // { dwServiceID, serviceIP, wServicePort, wReserved }. MW3's multiplayer
    // asks for the one its DemonWare servers are behind before it looks for
    // them, with no block to fill.
    Reply ServiceInfo(uint32_t service, uint32_t out)
    {
        constexpr uint32_t XONLINE_E_LOGON_SERVICE_NOT_AUTHORIZED = 0x80151802;
        if (service != online::demonware::kLspService) return Result(XONLINE_E_LOGON_SERVICE_NOT_AUTHORIZED);
        if (auto* info = GuestPtr<be32>(out))
        {
            info[0] = service;
            info[1] = online::demonware::kServerAddress;
            info[2] = uint32_t(online::demonware::kLobbyPort) << 16;
        }
        return {};
    }

    Reply XLiveBase(uint32_t message, uint32_t param1, uint32_t param2)
    {
        switch (message)
        {
        case 0x00050009: return StorageDownload(param1);
        case 0x0005000B: return StorageUpload(param1);
        case 0x00058004:   // XOnlineGetLogonID
            if (auto* id = GuestPtr<be32>(param1)) *id = 1;
            return {};
        case 0x00058006:   // XOnlineGetNatType: open
            if (auto* type = GuestPtr<be32>(param1)) *type = 1;
            return {};
        case 0x0005800E:   // XUserMuteListQuery: nobody is muted
            if (auto* muted = GuestPtr<be32>(param2)) *muted = 0;
            return {};
        case 0x00058007: return ServiceInfo(param1, param2);
        case 0x00058023: return InviteGetAcceptedInfo(param2);
        case 0x00058035: return StorageBuildServerPath(param1);
        default: return { X_ERROR_SUCCESS, X_ERROR_SUCCESS };
        }
    }

    // XUserSetContext: (user index, unused, XUID, context id, value). The
    // player is the XUID's when there is one, as Xenia reads it.
    Reply XgiUserSetContext(uint32_t buffer)
    {
        const auto* set = GuestPtr<be32>(buffer);
        if (!set) return Result(0x80070057);
        uint32_t user = set[0];
        if (const uint64_t xuid = (uint64_t(uint32_t(set[2])) << 32) | uint32_t(set[3]))
            for (uint32_t index = 0; index < 4; index++)
                if (const Who who = WhoIs(index); who.state && (who.offline == xuid || who.online == xuid))
                    user = index;
        presence::SetContext(user, set[4], set[5]);
        NoteSearched(set[4], set[5]);
        return {};
    }

#ifdef COD4_TITLE_MP
    // XUserReadStats: (title, players, their XUIDs, views asked for, their
    // specifications, the size of the results, the results). No service keeps
    // statistics, so each view asked for is answered with no rows. Call of
    // Duty 4 needs that much: the host of a Find Match lobby of a team game
    // reads everyone's skill before it makes the teams, and takes anything
    // but one view for a failure it prints nowhere a player sees, with the
    // lobby left at "Match beginning in: 0" for good. The later titles go on
    // with nothing written.
    Reply XgiUserReadStats(uint32_t buffer)
    {
        const auto* read = GuestPtr<be32>(buffer);
        if (!read) return Result(0x80070057);
        const uint32_t views = read[3], size = read[5], results = read[6];
        constexpr uint32_t kHeader = 8, kView = 16, kSpecification = 136;
        auto* header = GuestPtr<be32>(results);
        const auto* specifications = GuestPtr<be32>(uint32_t(read[4]));
        if (!header || !specifications || uint64_t(kHeader) + uint64_t(views) * kView > size)
            return Result(0x80070057);
        header[0] = views;
        header[1] = results + kHeader;
        for (uint32_t i = 0; i < views; i++)
        {
            be32* view = header + (kHeader + i * kView) / 4;
            view[0] = specifications[i * (kSpecification / 4)];   // the view's id
            view[1] = 0;                                          // rows it holds in all
            view[2] = 0;                                          // rows given
            view[3] = 0;
        }
        return {};
    }
#endif

    bool Handled(uint32_t app, uint32_t message)
    {
        if (app == kAppXGI)
        {
#ifdef COD4_TITLE_MP
            if (message == 0x000B0021) return true;
#endif
            return message == 0x000B0006 || message == 0x000B0007 || message == 0x000B0010 ||
                   message == 0x000B0011 || message == 0x000B0012 || message == 0x000B0013 ||
                   message == 0x000B0014 || message == 0x000B0015 || message == 0x000B0018 ||
                   message == 0x000B001C || message == 0x000B001E || message == 0x000B001F ||
                   message == 0x000B0065;
        }
        if (app == kAppXLiveBase)
            return message == 0x00050009 || message == 0x0005000B || message == 0x00058004 ||
                   message == 0x00058006 || (message == 0x00058007 && online::demonware::Served()) ||
                   message == 0x0005800E || message == 0x00058023 || message == 0x00058035;
        return false;
    }

    Reply XamMessage(uint32_t app, uint32_t message, uint32_t param1, uint32_t param2, uint32_t overlapped = 0)
    {
        if (!Handled(app, message))
        {
            // Answered "success" with nothing written; named once so a run shows
            // what the title asked for.
            static std::mutex lock;
            static std::set<uint64_t> seen;
            std::lock_guard g(lock);
            if (seen.insert((uint64_t(app) << 32) | message).second)
                LOGI("xam: message %02x/%08x answered with success", app, message);
            return {};
        }
        if (app == kAppXGI)
        {
            if (online::Trace() && message >= 0x000B0010)
                if (const auto* words = GuestPtr<be32>(param1))
                    LOGI("net-trace: session message %08x, %u bytes, session %08x, second word %x", message, param2,
                         uint32_t(words[0]), param2 >= 8 ? uint32_t(words[1]) : 0);
            switch (message)
            {
            case 0x000B0006: return XgiUserSetContext(param1);
            case 0x000B0007: return XgiUserSetProperty(param1);
            case 0x000B0010: return XgiSessionCreate(param1, param2);
            case 0x000B0011: return XgiSessionDelete(param1);
            case 0x000B0012: return XgiSessionJoinRemote(param1, param2);
            case 0x000B0013: return XgiSessionLeaveRemote(param1, param2);
            case 0x000B0014: return XgiSessionStart(param1);
            case 0x000B0015: return XgiSessionEnd(param1);
            case 0x000B0018: return XgiSessionModify(param1, param2);
            case 0x000B001C: return XgiSessionSearch(param1, param2, overlapped, false);
            case 0x000B001E: return XgiSessionMigrateHost(param1, param2);
            case 0x000B001F: return {};   // XSessionModifySkill: no service keeps a skill
#ifdef COD4_TITLE_MP
            case 0x000B0021: return XgiUserReadStats(param1);
#endif
            case 0x000B0065: return XgiSessionSearch(param1, param2, overlapped, true);
            }
        }
        return XLiveBase(message, param1, param2);
    }
}

uint64_t kernel::LocalLiveXuid() { return LocalOnlineXuid(); }

void kernel::MovePlayerData(uint64_t from, uint64_t to)
{
    from &= 0xFFFFFFFFFFFFull;
    to &= 0xFFFFFFFFFFFFull;
    if (!from || from == to) return;
    const fs::path& saves = SaveRoot();
    std::error_code ec;
    const std::string was = "mpdata_" + XuidText(0xE000000000000000ull | from), is = "mpdata_" + XuidText(0xE000000000000000ull | to);
    if (fs::exists(saves / was / was, ec) && !fs::exists(saves / is, ec))
    {
        fs::rename(saves / was, saves / is, ec);
        if (!ec) fs::rename(saves / is / was, saves / is / is, ec);
        LOGI("xam: the rank kept as %s is now %s%s", was.c_str(), is.c_str(), ec ? " -- not moved" : "");
    }
    // The package a replaced save was rotated aside into (content.cpp,
    // RotatePackage) follows its own number, for the reason given below.
    std::error_code rankEc;
    const fs::path rankWas = saves / (was + ".bak"), rankIs = saves / (is + ".bak");
    if (fs::exists(rankWas, rankEc))
    {
        fs::remove_all(rankIs, rankEc);
        fs::rename(rankWas, rankIs, rankEc);
    }
    const fs::path users = saves / "online" / "user";
    const fs::path liveWas = users / XuidText(OnlineXuid(from)), liveIs = users / XuidText(OnlineXuid(to));
    if (fs::is_directory(liveWas, ec) && !fs::exists(liveIs, ec)) fs::rename(liveWas, liveIs, ec);
    // A profile's settings at the second to fourth controllers (kernel/profile.cpp).
    char settingsWas[40], settingsIs[40];
    std::snprintf(settingsWas, sizeof settingsWas, "profile_%012llx.bin", (unsigned long long)from);
    std::snprintf(settingsIs, sizeof settingsIs, "profile_%012llx.bin", (unsigned long long)to);
    if (fs::exists(saves / settingsWas, ec) && !fs::exists(saves / settingsIs, ec))
    {
        fs::rename(saves / settingsWas, saves / settingsIs, ec);
        // The copy an interrupted save left behind goes with it. Left under the
        // old name it is both clutter and a trap: a profile later given this
        // same number would find it and recover a stranger's settings out of it.
        std::error_code backupEc;
        const fs::path backupWas = saves / (std::string(settingsWas) + ".bak");
        const fs::path backupIs  = saves / (std::string(settingsIs)  + ".bak");
        if (fs::exists(backupWas, backupEc))
        {
            fs::remove(backupIs, backupEc);
            fs::rename(backupWas, backupIs, backupEc);
        }
    }
}

void kernel::AcceptInvite(uint64_t inviterAccount, const uint8_t* session60, bool fromInvite)
{
    LOGI("online: accepting %s from %016llX for session %02X..",
         fromInvite ? "an invitation" : "a join", (unsigned long long)inviterAccount, session60[0]);
    {
        std::lock_guard g(g_inviteLock);
        uint8_t* p = g_invite.bytes;
        std::memset(p, 0, sizeof g_invite.bytes);
        *reinterpret_cast<be64*>(p + 0) = LocalOnlineXuid();
        *reinterpret_cast<be64*>(p + 8) = OnlineXuid(inviterAccount);
        *reinterpret_cast<be32*>(p + 16) = kTitleId;
        std::memcpy(p + 20, session60, online::kSessionInfoSize);
        *reinterpret_cast<be32*>(p + 80) = fromInvite ? 1 : 0;
        g_invite.any = true;
    }
    std::lock_guard g(g_listenersLock);
    g_inviteUnheard = !PostLocked(XN_LIVE_INVITE_ACCEPTED, kLocalUser);
}

// A session is a kernel object the title names by handle and hands XAM by
// object address in every session message. Nothing here reads the object; it
// only has to be memory of its own.
namespace
{
    struct SessionObject : Object
    {
        uint32_t guest = 0;
        const char* TypeName() const override { return "session"; }
    };
}

// (handleOut)
PPC_FUNC(__imp__XamSessionCreateHandle)
{
    auto session = std::make_shared<SessionObject>();
    session->guest = kernel::AllocateGuest(64, 16);
    if (session->guest) std::memset(GuestPtr<uint8_t>(session->guest), 0, 64);
    const uint32_t handle = session->guest ? InsertHandle(session) : 0;
    if (auto* out = GuestPtr<be32>(ctx.r3.u32)) *out = handle;
    ctx.r3.u64 = handle ? X_ERROR_SUCCESS : X_ERROR_NOT_ENOUGH_MEMORY;
}

// (handle, objectOut)
PPC_FUNC(__imp__XamSessionRefObjByHandle)
{
    auto session = LookupHandleAs<SessionObject>(ctx.r3.u32);
    if (auto* out = GuestPtr<be32>(ctx.r4.u32)) *out = session ? session->guest : 0;
    ctx.r3.u64 = session ? X_ERROR_SUCCESS : X_ERROR_INVALID_HANDLE;
}

PPC_FUNC(__imp__XMsgInProcessCall)
{
    // (app, message, param1, param2)
    ctx.r3.u64 = XamMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32).result;
}

PPC_FUNC(__imp__XMsgStartIORequest)
{
    // (app, message, overlapped, buffer, length). The asynchronous form of the
    // same call: it finishes inline here, but the caller polls the overlapped
    // block, so the answer has to go there and the call itself say "pending".
    const uint32_t overlapped = ctx.r5.u32;
    const Reply reply = XamMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, overlapped);
    if (reply.pending) { ctx.r3.u64 = X_ERROR_IO_PENDING; return; }
    if (!overlapped) { ctx.r3.u64 = reply.result; return; }
    CompleteOverlappedEx(overlapped, reply.result, reply.extended, 0);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
}

PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    // (app, message, overlapped, buffer, length, auxiliary stack): the same
    // request with room for the system's own call, which is not made here.
    __imp__XMsgStartIORequest(ctx, base);
}

void kernel::FinishSearches()
{
    auto* service = online::Get();
    if (!service) return;
    std::lock_guard g(g_searchLock);
    for (auto it = g_searches.begin(); it != g_searches.end();)
    {
        std::vector<online::SessionFound> found;
#ifdef COD4_TITLE_MP
        const auto now = std::chrono::steady_clock::now();
        if (!it->id)
        {
            // Found nothing, and waiting to ask again.
            if (now - it->sent >= kAskAgain)
            {
                it->id = service->Search(it->asked);
                it->sent = now;
            }
            ++it;
            continue;
        }
        if (!service->SearchResults(it->id, found)) { ++it; continue; }
        if (found.empty() && now < it->until)
        {
            it->id = 0;
            ++it;
            continue;
        }
#else
        if (!service->SearchResults(it->id, found)) { ++it; continue; }
#endif
        AnswerSearch(*it, found);
        it = g_searches.erase(it);
    }
}

// (overlapped, wait). The only call that can still be running is a search; the
// title may reuse its block once this returns, so its answer must not come.
PPC_FUNC(__imp__XMsgCancelIORequest)
{
    const uint32_t overlapped = ctx.r3.u32;
    {
        std::lock_guard g(g_searchLock);
        const auto it = std::find_if(g_searches.begin(), g_searches.end(),
                                     [&](const Search& s) { return s.overlapped == overlapped; });
        if (it != g_searches.end())
        {
            g_searches.erase(it);
            constexpr uint32_t X_ERROR_CANCELLED = 1223;
            CompleteOverlappedEx(overlapped, X_ERROR_CANCELLED, 0x80070000 | X_ERROR_CANCELLED, 0);
        }
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// Enumerators: the title asks XAM for a list -- content packages, Live's
// title servers, friends -- by a handle it then pages through. Nothing here
// has content or friends to list, so those enumerations end at once, which is
// what the console answers for a player with no downloaded content. Live's
// title servers are the one list with an entry: where MW3's multiplayer finds
// its DemonWare lobby (online/demonware.h).
namespace
{
    // The title reaches its private block through the enumerator object, whose
    // own fields come first: XTitleServerCreateEnumerator writes the server
    // name it wants at +0x18 of what XamGetPrivateEnumStructureFromHandle
    // hands back, and goes on for the size it asked for.
    constexpr uint32_t kEnumeratorHeader = 0x18;
    constexpr uint32_t kAppXLiveBaseEnumerators = 0xFC, kTitleServersOpen = 0x00058039;
    // XTITLE_SERVER_INFO { inaServer, dwFlags, szServerInfo[200] }
    constexpr uint32_t kTitleServerInfoSize = 0xD0, kTitleServerNameSize = 200;
    // After the name in that private block: a word, the size and then the
    // address of the work memory XTitleServerCreateEnumerator takes with
    // XamAlloc and leaves to XAM, which frees it when the handle closes.
    constexpr uint32_t kTitleServerWork = kTitleServerNameSize + 8;

    struct Enumerator : Object
    {
        uint32_t extra = 0;          // the object and the caller's private block after it
        uint32_t extraSize = 0;      // that block's size
        bool titleServers = false;   // XTitleServerCreateEnumerator's
        bool listed = false;         // its one server was handed out
        // Call of Duty 4's multiplayer, signed in and told Live has no
        // title server, asks again every frame: with each enumerator's work
        // memory left behind (11 KB), the guest heap's 416 MB were gone after
        // five minutes at 120 frames a second, and the next enumeration ended
        // the match ("Xbox 360 Error 6 while trying to enumerate lsp title
        // servers").
        ~Enumerator() override
        {
            if (!extra) return;
            if (titleServers && extraSize >= kTitleServerWork + 4)
                kernel::FreeGuest(*GuestPtr<be32>(extra + kEnumeratorHeader + kTitleServerWork));
            kernel::FreeGuest(extra);
        }
        const char* TypeName() const override { return "enumerator"; }
    };

    uint32_t NewEnumerator(uint32_t extraSize, bool titleServers = false)
    {
        auto enumerator = std::make_shared<Enumerator>();
        enumerator->titleServers = titleServers;
        if (extraSize)
        {
            if (extraSize > 0x10000) return 0;
            enumerator->extra = kernel::AllocateGuest(kEnumeratorHeader + extraSize, 16);
            enumerator->extraSize = extraSize;
            if (!enumerator->extra) return 0;
            std::memset(GuestPtr<uint8_t>(enumerator->extra), 0, kEnumeratorHeader + extraSize);
        }
        return InsertHandle(enumerator);
    }

    // The DemonWare lobby as Live's one title server for this title, under the
    // name the title asked for. False when the buffer has no room for it.
    bool WriteTitleServer(const Enumerator& enumerator, uint32_t buffer, uint32_t size)
    {
        auto* info = GuestPtr<uint8_t>(buffer);
        const auto* wanted = GuestPtr<char>(enumerator.extra + kEnumeratorHeader);
        if (!info || !enumerator.extra || size < kTitleServerInfoSize) return false;
        std::memset(info, 0, kTitleServerInfoSize);
        *reinterpret_cast<be32*>(info) = online::demonware::kServerAddress;
        std::memcpy(info + 8, wanted, strnlen(wanted, kTitleServerNameSize - 1));
        return true;
    }

    constexpr uint32_t X_ERROR_NO_MORE_FILES = 0x12;
    constexpr uint32_t kXContentDataSize = 0x134;
}

// (user, app, openMessage, closeMessage, extraSize, itemCount, flags, handleOut)
PPC_FUNC(__imp__XamCreateEnumeratorHandle)
{
    const bool titleServers = ctx.r4.u32 == kAppXLiveBaseEnumerators && ctx.r5.u32 == kTitleServersOpen;
    const uint32_t handle = NewEnumerator(ctx.r7.u32, titleServers);
    if (auto* out = GuestPtr<be32>(ctx.r10.u32)) *out = handle;
    ctx.r3.u64 = handle ? X_ERROR_SUCCESS : X_ERROR_NOT_ENOUGH_MEMORY;
}

// (handle, extraOut)
PPC_FUNC(__imp__XamGetPrivateEnumStructureFromHandle)
{
    auto enumerator = LookupHandleAs<Enumerator>(ctx.r3.u32);
    if (auto* out = GuestPtr<be32>(ctx.r4.u32)) *out = enumerator ? enumerator->extra : 0;
    ctx.r3.u64 = enumerator ? X_ERROR_SUCCESS : X_ERROR_INVALID_HANDLE;
}

// (user, device, contentType, flags, itemCount, bufferSizeOut, handleOut)
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    const uint32_t handle = NewEnumerator(0);
    if (auto* size = GuestPtr<be32>(ctx.r8.u32)) *size = ctx.r7.u32 * kXContentDataSize;
    if (auto* out = GuestPtr<be32>(ctx.r9.u32)) *out = handle;
    ctx.r3.u64 = handle ? X_ERROR_SUCCESS : X_ERROR_NOT_ENOUGH_MEMORY;
}

// (handle, flags, buffer, bufferSize, itemsOut, overlapped)
PPC_FUNC(__imp__XamEnumerate)
{
    auto enumerator = LookupHandleAs<Enumerator>(ctx.r3.u32);
    if (!enumerator) { ctx.r3.u64 = X_ERROR_INVALID_HANDLE; return; }
    const bool server = enumerator->titleServers && !enumerator->listed && online::demonware::Served() &&
                        WriteTitleServer(*enumerator, ctx.r5.u32, ctx.r6.u32);
    if (server) enumerator->listed = true;
    if (const uint32_t overlapped = ctx.r8.u32)
    {
        if (server) CompleteOverlappedEx(overlapped, X_ERROR_SUCCESS, X_ERROR_SUCCESS, 1);
        else CompleteOverlappedEx(overlapped, kFunctionFailed, 0x80070000 | X_ERROR_NO_MORE_FILES, 0);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    if (auto* items = GuestPtr<be32>(ctx.r7.u32)) *items = server ? 1 : 0;
    ctx.r3.u64 = server ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
}

// XamGetSystemVersion, XGetLanguage and XGetAVPack are in misc.cpp.

PPC_FUNC(__imp__XamAlloc)
{
    // (flags, size, outPointer)
    uint32_t address = kernel::AllocateGuest(ctx.r4.u32, 16);
    if (auto* out = GuestPtr<be32>(ctx.r5.u32)) *out = address;
    ctx.r3.u64 = address ? X_ERROR_SUCCESS : X_STATUS_NO_MEMORY;
}

PPC_FUNC(__imp__XamFree)
{
    kernel::FreeGuest(ctx.r3.u32);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__ExGetXConfigSetting)
{
    uint32_t category = ctx.r3.u32, setting = ctx.r4.u32;
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32;

    uint32_t value = 0;
    uint32_t width = 4;
    if (category == 0x0003)          // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x0001: value = 0;  width = 1; break;   // timezone-ish
        case 0x0002: value = 1;  width = 4; break;   // language: English
        case 0x0003: value = 0;  width = 4; break;   // video flags
        default: break;
        }
    }

    if (buffer && size >= width)
    {
        if (width == 1) *GuestPtr<uint8_t>(buffer) = uint8_t(value);
        else            *GuestPtr<be32>(buffer) = value;
    }
    if (auto* out = GuestPtr<be16>(ctx.r7.u32)) *out = uint16_t(width);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The service over a transport (transport.h): the players it reaches, with no
// account anywhere. lan and server are this, on the local network's UDP and on
// a relay server.
//
// Who the player is, to the title, is the profile at the first controller
// (signin.h), plus the transport's slot, so a second copy on one machine is a
// second player. MW2_NAME is another name for him than the profile's.
//
// Xbox LIVE lobbies: every copy says who it is once a second to everyone the
// transport reaches. The friends screen (friends.h) lists them, and the one
// chosen there is invited: a copy started with MW2_LAN_ACCEPT=1 accepts.
// MW2_LAN_JOIN=<name> joins that player's party or lobby as soon as it is
// seen, as choosing a friend and "Join Session" does on the console.
//
// What can be joined goes with that to everyone on the local network. On a
// relay everyone is strangers too, so there it goes only to the players this
// copy invited, one by one: nobody else on the relay learns of a private
// match or has what it takes to join it.
#include "transport.h"
#include "../env.h"
#include "../log.h"
#include "../signin.h"
#include "../title.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
    // What travels is big-endian; every host this runs on is little-endian.
    uint64_t BigEndian64(uint64_t value) { return __builtin_bswap64(value); }
    uint32_t BigEndian32(uint32_t value) { return __builtin_bswap32(value); }
    uint16_t BigEndian16(uint16_t value) { return __builtin_bswap16(value); }

    // Four letters of a packet's kind, the title's three first (T_NET_TAG):
    // another title's copies on the same network or relay are not heard.
    constexpr uint32_t Magic(char kind)
    {
        return (uint32_t(uint8_t(T_NET_TAG[0])) << 24) | (uint32_t(uint8_t(T_NET_TAG[1])) << 16) |
               (uint32_t(uint8_t(T_NET_TAG[2])) << 8) | uint8_t(kind);
    }

    // Magic, the sender's instance, its port and the port it is for.
    struct Frame
    {
        uint32_t magic;
        uint32_t instance;
        uint16_t fromPort, toPort;
    };
    constexpr uint32_t kMagic = Magic('L');   // "MW2L", "MW3L"

    // Who a copy is and what can be joined: to everyone as presence, to one
    // player as an invitation, or to one player as what he alone is told.
    struct Presence
    {
        uint32_t magic;
        uint32_t instance;
        uint8_t kind;          // kAnnounce, kInvite or kTold
        uint8_t joinable;      // `session` holds something
        uint8_t discreet;      // its kAnnounce never says what can be joined: kInvite and kTold do
        uint8_t pad;
        uint64_t account;      // big-endian
        char name[32];
        uint8_t session[online::kSessionInfoSize];
    };
    constexpr uint32_t kPresenceMagic = Magic('P');   // "MW2P", "MW3P"
    constexpr uint8_t kAnnounce = 0, kInvite = 1, kTold = 2;
    constexpr auto kAnnounceEvery = std::chrono::seconds(1);

    // A copy heard, kept under its id: the name is the player's to choose
    // (every fresh install was "Player 1" once), and kept by name a second
    // copy of the same name took the first one's place in the list.
    struct Peer
    {
        uint64_t id = 0, account = 0;
        std::string name;
        bool joinable = false;
        uint8_t session[online::kSessionInfoSize]{};
        std::chrono::steady_clock::time_point heard{};
    };
    constexpr size_t kLargest = 64 * 1024;
    constexpr size_t kQueued = 512;
    // A copy says who it is every second; one silent this long has left. On a
    // public relay anyone can say any name, so the list has a size too.
    constexpr auto kPeerGoneAfter = std::chrono::seconds(10);
    constexpr size_t kMostPeers = 256;

    class Players final : public online::Service
    {
    public:
        explicit Players(std::unique_ptr<online::Transport> transport) : transport_(std::move(transport)) {}

        bool Start() override
        {
            if (!transport_->Open()) return false;
            instance_ = std::random_device{}();
            const signin::Player player = signin::First();
            account_ = player.id;
            const char* name = env::Text("MW2_NAME");
            name_ = name ? name : player.name;
            if (const char* join = env::Text("MW2_LAN_JOIN")) join_ = join;
            accept_ = env::Flag("MW2_LAN_ACCEPT");
            LOGI("online: %s as \"%s\"", transport_->Name(), name_.c_str());
            return true;
        }

        uint64_t LocalId() override { return transport_->LocalId(); }
        // The profile, and which copy on this machine this is.
        uint64_t Account() override { return account_ + transport_->Slot(); }
        std::string LocalName() override { return name_; }

        bool Send(uint64_t peer, uint16_t fromPort, uint16_t toPort,
                  const void* bytes, size_t size) override
        {
            const std::vector<uint8_t>& packet = Framed(fromPort, toPort, bytes, size);
            const bool sent = !packet.empty() && transport_->SendTo(peer, packet.data(), packet.size());
            if (!sent && online::Trace())
                LOGI("net-trace: %s could not send %zu bytes to %012llx", transport_->Name(), size,
                     static_cast<unsigned long long>(peer));
            return sent;
        }

        bool Broadcast(uint16_t fromPort, uint16_t toPort, const void* bytes, size_t size) override
        {
            const std::vector<uint8_t>& packet = Framed(fromPort, toPort, bytes, size);
            const bool sent = !packet.empty() && transport_->SendAll(packet.data(), packet.size());
            if (!sent && online::Trace())
                LOGI("net-trace: %s could not send %zu bytes to everyone", transport_->Name(), size);
            return sent;
        }

        bool Receive(uint16_t port, online::Datagram& out) override
        {
            std::lock_guard g(lock_);
            Drain();
            auto it = waiting_.find(port);
            if (it == waiting_.end() || it->second.empty()) return false;
            out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }

        bool Lobbies() override { return true; }

        void SetJoinable(const online::Joinable* joinable) override
        {
            std::lock_guard g(lock_);
            hasJoinable_ = joinable != nullptr;
            if (joinable) joinable_ = *joinable;
            AnnounceLocked();
        }

        bool NextInvite(online::Invite& out) override
        {
            std::lock_guard g(lock_);
            Drain();
            const auto now = std::chrono::steady_clock::now();
            if (now - announced_ >= kAnnounceEvery) AnnounceLocked();
            if (!join_.empty())
            {
                const auto it = std::find_if(peers_.begin(), peers_.end(), [&](const auto& entry) {
                    return entry.second.joinable && entry.second.name == join_;
                });
                if (it != peers_.end())
                {
                    LOGI("online: %s joins %s", transport_->Name(), join_.c_str());
                    invites_.push_back(InviteFrom(it->second, false));
                    join_.clear();
                }
            }
            if (invites_.empty()) return false;
            out = invites_.front();
            invites_.pop_front();
            return true;
        }

        bool Matchmaking() override { return transport_->Matchmaking(); }
        void List(const online::SessionListing* listing) override { transport_->List(listing); }
        uint32_t Search(const online::SessionSearch& search) override { return transport_->Search(search); }
        // Results arrive with everything else the transport holds.
        bool SearchResults(uint32_t search, std::vector<online::SessionFound>& out) override
        {
            {
                std::lock_guard g(lock_);
                Drain();
            }
            return transport_->SearchResults(search, out);
        }
        bool TitleFile(const std::string& name, std::vector<uint8_t>& out) override
        {
            return transport_->TitleFile(name, out);
        }

        // Everyone heard, by name: there is no account here to have friends,
        // so the player picks among who is there.
        bool Friends(online::FriendList& list) override
        {
            std::lock_guard g(lock_);
            Drain();
            ForgetGoneLocked();
            std::vector<online::Friend> playing;
            for (const auto& [id, peer] : peers_)
            {
                online::Friend one;
                one.id = id;
                one.name = peer.name;
                // Two players may have the same name; the end of each one's id
                // then tells the two lines apart.
                const bool shared = std::any_of(peers_.begin(), peers_.end(), [&](const auto& other) {
                    return other.first != id && other.second.name == peer.name;
                });
                if (shared)
                {
                    char end[8];
                    std::snprintf(end, sizeof end, " (%04X)", unsigned(id & 0xFFFF));
                    one.name += end;
                }
                playing.push_back(std::move(one));
            }
            std::sort(playing.begin(), playing.end(), [](const online::Friend& a, const online::Friend& b) {
                return a.name != b.name ? a.name < b.name : a.id < b.id;
            });
            const auto same = [](const online::Friend& a, const online::Friend& b) {
                return a.id == b.id && a.name == b.name;
            };
            if (!friends_.version ||
                !std::equal(playing.begin(), playing.end(), friends_.playing.begin(), friends_.playing.end(), same))
            {
                friends_.playing = std::move(playing);
                friends_.version++;
            }
            friends_.strangers = true;
            if (list.version != friends_.version) list = friends_;
            return true;
        }

        bool InviteFriend(uint64_t id) override
        {
            std::lock_guard g(lock_);
            if (!hasJoinable_) { LOGW("online: nothing to invite anyone to yet"); return false; }
            const auto it = peers_.find(id);
            if (it == peers_.end()) return false;
            LOGI("online: %s invites %s at %012llx", transport_->Name(), it->second.name.c_str(),
                 static_cast<unsigned long long>(id));
            told_.insert(id);
            const Presence invite = PresenceLocked(kInvite, true);
            return transport_->SendTo(id, &invite, sizeof invite);
        }

        size_t PeerCount() override
        {
            std::lock_guard g(lock_);
            ForgetGoneLocked();
            return peers_.size();
        }

        size_t PeersBefore() override
        {
            std::lock_guard g(lock_);
            ForgetGoneLocked();
            const uint64_t own = transport_->LocalId();
            return size_t(std::count_if(peers_.begin(), peers_.end(),
                                        [&](const auto& entry) { return entry.second.id < own; }));
        }

        std::string JoinablePeers() override
        {
            std::lock_guard g(lock_);
            ForgetGoneLocked();
            std::string lines;
            char id[20];
            for (const auto& [from, peer] : peers_)
            {
                if (!peer.joinable) continue;
                std::snprintf(id, sizeof id, "%llx\t", static_cast<unsigned long long>(from));
                lines += id + peer.name + "\n";
            }
            return lines;
        }

        // The peer's Live party or lobby, as MW2_LAN_JOIN joins it.
        bool JoinPeer(uint64_t id) override
        {
            std::lock_guard g(lock_);
            const auto it = peers_.find(id);
            if (it == peers_.end() || !it->second.joinable) return false;
            LOGI("online: %s joins %s at %012llx", transport_->Name(), it->second.name.c_str(),
                 static_cast<unsigned long long>(id));
            invites_.push_back(InviteFrom(it->second, false));
            return true;
        }

    private:
        void ForgetGoneLocked()
        {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(peers_, [&](const auto& entry) { return now - entry.second.heard >= kPeerGoneAfter; });
            // An id may be another copy's the next time it is heard.
            std::erase_if(told_, [&](uint64_t id) { return !peers_.count(id); });
        }

        static online::Invite InviteFrom(const Peer& peer, bool fromInvite)
        {
            online::Invite invite;
            invite.inviter = peer.account;
            std::memcpy(invite.session, peer.session, sizeof invite.session);
            invite.fromInvite = fromInvite;
            return invite;
        }

        // The title's datagram behind a frame; empty when it is too large.
        const std::vector<uint8_t>& Framed(uint16_t fromPort, uint16_t toPort, const void* bytes, size_t size)
        {
            thread_local std::vector<uint8_t> packet;
            packet.clear();
            if (size > kLargest - sizeof(Frame)) return packet;
            packet.resize(sizeof(Frame) + size);
            const Frame frame{ BigEndian32(kMagic), instance_, BigEndian16(fromPort), BigEndian16(toPort) };
            std::memcpy(packet.data(), &frame, sizeof frame);
            std::memcpy(packet.data() + sizeof frame, bytes, size);
            return packet;
        }

        Presence PresenceLocked(uint8_t kind, bool withSession)
        {
            Presence presence{};
            presence.magic = BigEndian32(kPresenceMagic);
            presence.instance = instance_;
            presence.kind = kind;
            presence.joinable = withSession && hasJoinable_;
            presence.discreet = transport_->Public();
            presence.account = BigEndian64(Account());
            std::snprintf(presence.name, sizeof presence.name, "%s", name_.c_str());
            if (presence.joinable) std::memcpy(presence.session, joinable_.session, sizeof presence.session);
            return presence;
        }

        void AnnounceLocked()
        {
            announced_ = std::chrono::steady_clock::now();
            const bool discreet = transport_->Public();
            const Presence presence = PresenceLocked(kAnnounce, !discreet);
            transport_->SendAll(&presence, sizeof presence);
            if (!discreet) return;
            // Also when nothing can be joined any more: that is how they
            // learn the lobby is gone.
            const Presence told = PresenceLocked(kTold, true);
            for (const uint64_t id : told_) transport_->SendTo(id, &told, sizeof told);
        }

        void Heard(uint64_t from, const Presence& presence)
        {
            if (presence.instance == instance_) return;
            std::string name(presence.name, strnlen(presence.name, sizeof presence.name));
            // The list goes to a picker a line a peer, id and name a tab apart.
            for (char& c : name)
                if (uint8_t(c) < ' ') c = ' ';
            if (!peers_.count(from))
            {
                ForgetGoneLocked();
                if (peers_.size() >= kMostPeers) return;
                LOGI("online: %s hears %s at %012llx", transport_->Name(), name.c_str(),
                     static_cast<unsigned long long>(from));
            }
            Peer& peer = peers_[from];
            peer.id = from;
            peer.name = name;
            peer.heard = std::chrono::steady_clock::now();
            peer.account = BigEndian64(presence.account);
            if (peer.account == Account() && !warned_)
            {
                warned_ = true;
                LOGE("online: %s on this %s is the same player as this one; on one of the two machines,"
                     " take the `machine` line out of saves/profiles.txt", name.c_str(),
                     transport_->Name() == std::string("lan") ? "network" : "server");
            }
            // A discreet copy's word to everyone says nothing of what can be
            // joined, which is not that nothing can.
            if (presence.kind != kAnnounce || !presence.discreet)
            {
                peer.joinable = presence.joinable;
                std::memcpy(peer.session, presence.session, sizeof peer.session);
            }
            if (presence.kind != kInvite || !peer.joinable) return;
            if (accept_)
            {
                LOGI("online: %s accepts %s's invitation", transport_->Name(), name.c_str());
                invites_.push_back(InviteFrom(peer, true));
            }
            else
                LOGI("online: %s invites you (MW2_LAN_ACCEPT=1 accepts)", name.c_str());
        }

        // Everything the transport holds, sorted by the port it is for. A
        // broadcast can come back to the copy that sent it; the instance tells
        // it apart.
        void Drain()
        {
            thread_local std::vector<uint8_t> packet(kLargest);
            for (;;)
            {
                uint64_t from = 0;
                const long got = transport_->Next(packet.data(), packet.size(), from);
                if (got < 0) return;
                uint32_t magic = 0;
                if (size_t(got) >= sizeof magic) std::memcpy(&magic, packet.data(), sizeof magic);
                if (BigEndian32(magic) == kPresenceMagic && size_t(got) == sizeof(Presence))
                {
                    Presence presence;
                    std::memcpy(&presence, packet.data(), sizeof presence);
                    Heard(from, presence);
                    continue;
                }
                Frame frame;
                if (size_t(got) < sizeof frame) continue;
                std::memcpy(&frame, packet.data(), sizeof frame);
                if (BigEndian32(frame.magic) != kMagic)
                {
                    if (online::Trace())
                        LOGI("net-trace: %s dropped %ld bytes from %012llx: not this game's (%08x)", transport_->Name(),
                             got, static_cast<unsigned long long>(from), BigEndian32(frame.magic));
                    continue;
                }
                if (frame.instance == instance_) continue;   // this copy's own broadcast
                online::Datagram datagram;
                datagram.from = from;
                datagram.fromPort = BigEndian16(frame.fromPort);
                datagram.toPort = BigEndian16(frame.toPort);
                datagram.bytes.assign(packet.begin() + sizeof frame, packet.begin() + got);
                // A port nobody reads fills up and drops, as a socket's buffer does.
                auto& queue = waiting_[datagram.toPort];
                if (online::Trace())
                    LOGI("net-trace: %s heard %zu bytes from %012llx, port %u to port %u (%zu waiting there)",
                         transport_->Name(), datagram.bytes.size(), static_cast<unsigned long long>(from),
                         datagram.fromPort, datagram.toPort, queue.size());
                if (queue.size() < kQueued) queue.push_back(std::move(datagram));
            }
        }

        std::unique_ptr<online::Transport> transport_;
        uint32_t instance_ = 0;
        std::string name_;
        uint64_t account_ = 0;
        bool warned_ = false;                  // told once that another machine has this identity
        std::mutex lock_;
        std::map<uint16_t, std::deque<online::Datagram>> waiting_;

        std::map<uint64_t, Peer> peers_;      // by the transport's id for the copy
        bool hasJoinable_ = false;
        online::Joinable joinable_;
        std::set<uint64_t> told_;              // a relay's players who are told what can be joined
        online::FriendList friends_;           // as last built for the friends screen
        std::chrono::steady_clock::time_point announced_{};
        std::string join_;                     // MW2_LAN_JOIN, until joined
        bool accept_ = false;                  // MW2_LAN_ACCEPT
        std::deque<online::Invite> invites_;
    };
}

std::unique_ptr<online::Service> online::MakePlayers(std::unique_ptr<Transport> transport)
{
    return std::make_unique<Players>(std::move(transport));
}

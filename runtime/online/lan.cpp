// Players on the same network, over plain UDP: the smallest real transport, and
// the one that tests the kernel side without an account anywhere. Two copies on
// one machine find each other too, which the title's own sockets cannot do --
// both would want port 1000.
//
// Each copy sends from a port of its own, and a peer is that port's address:
// the IPv4 address in the top bits, the port in the low 16. Broadcasts go to a
// port every copy shares (MW2_LAN_PORT, 3074 by default), which the kernel
// delivers to all of them; each copy's own port is the first free one after
// it, and how far after is the copy's slot.
// MW2_LAN_IFACE pins the announced adapter by name or address substring;
// without it the first connection up is announced.
//
// What goes to everyone also goes to the own ports of the other copies on
// this machine, at the announced address (the first kLocalCopies slots): two
// apps on one phone then find each other with no network connection at all,
// where a broadcast has nowhere to go, and on a network that drops broadcasts.
#include "transport.h"
#include "udp.h"
#include "../env.h"
#include "../log.h"

#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace
{
    using udp::Socket;
    using udp::kNoSocket;

    uint64_t IdOf(const sockaddr_in& address)
    {
        return (uint64_t(ntohl(address.sin_addr.s_addr)) << 16) | ntohs(address.sin_port);
    }

    sockaddr_in AddressOf(uint64_t id)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(uint32_t(id >> 16));
        address.sin_port = htons(uint16_t(id));
        return address;
    }

    // The network connections that are up, the loopback aside, in the order
    // the system lists them. On Android the WiFi interfaces come first: the
    // system lists cellular (rmnet) before wlan, and announcing a cellular
    // address, or broadcasting by the default route, would send discovery to
    // the carrier where it dies instead of to the LAN the players share.
    struct Adapter { std::string name; uint32_t address; uint32_t netmask; };
    std::vector<Adapter> Adapters()
    {
        std::vector<Adapter> found;
#ifdef _WIN32
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        }
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) != NO_ERROR) return found;
        for (auto* at = adapters; at; at = at->Next)
        {
            if (at->OperStatus != IfOperStatusUp || at->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char name[128] = "?";
            if (at->FriendlyName)
                WideCharToMultiByte(CP_UTF8, 0, at->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
            name[sizeof name - 1] = 0;
            for (auto* unicast = at->FirstUnicastAddress; unicast; unicast = unicast->Next)
            {
                const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                if (address->sin_family != AF_INET) continue;
                const ULONG prefix = unicast->OnLinkPrefixLength > 32 ? 32 : unicast->OnLinkPrefixLength;
                const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
                found.push_back({ name, ntohl(address->sin_addr.s_addr), mask });
            }
        }
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) return found;
        for (ifaddrs* at = list; at; at = at->ifa_next)
        {
            if (!at->ifa_addr || at->ifa_addr->sa_family != AF_INET) continue;
            if (!(at->ifa_flags & IFF_UP) || (at->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t mask = 0;
            if (at->ifa_netmask && at->ifa_netmask->sa_family == AF_INET)
                mask = ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_netmask)->sin_addr.s_addr);
            found.push_back({ at->ifa_name, ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_addr)->sin_addr.s_addr), mask });
        }
        freeifaddrs(list);
#ifdef MW2_ANDROID
        std::stable_partition(found.begin(), found.end(), [](const Adapter& a) {
            return a.name.compare(0, 4, "wlan") == 0;
        });
#endif
#endif
        return found;
    }

    // The address other copies reach this one at: the first connection that is
    // up and not the loopback, or the loopback when there is none. The log
    // names them all: with more than one, the first may not be the one the
    // other players are on.
    //
    // MW2_LAN_IFACE pins one: any adapter whose name or address contains the
    // value ("WiFi", "wlan0", "192.168.1"). Matters with several up at once --
    // a VPN adapter first means sessions invite to an address only VPN peers
    // reach, and WiFi-only guests then loop on "attempt #N" forever.
    uint32_t OwnAddress()
    {
        const std::vector<Adapter> adapters = Adapters();
        std::string list;
        for (const Adapter& adapter : adapters)
        {
            in_addr address;
            address.s_addr = htonl(adapter.address);
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, text, sizeof text);
            list += (list.empty() ? "" : ", ") + adapter.name + " " + text;
        }
        if (adapters.empty()) LOGI("online: no network connection is up; lan stays on this machine");
        else LOGI("online: network connections: %s; announcing the first", list.c_str());
        const char* pinned = env::Text("MW2_LAN_IFACE");
        if (pinned && *pinned)
        {
            for (const Adapter& adapter : adapters)
            {
                in_addr address;
                address.s_addr = htonl(adapter.address);
                char text[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &address, text, sizeof text);
                if (adapter.name.find(pinned) != std::string::npos ||
                    std::string(text).find(pinned) != std::string::npos)
                {
                    LOGI("online: MW2_LAN_IFACE=%s, announcing %s %s", pinned,
                         adapter.name.c_str(), text);
                    return adapter.address;
                }
            }
            LOGW("online: MW2_LAN_IFACE=%s matches no connection, announcing the first", pinned);
        }
        return adapters.empty() ? uint32_t(INADDR_LOOPBACK) : adapters.front().address;
    }

    class Lan final : public online::Transport
    {
    public:
        ~Lan() override
        {
            if (own_ != kNoSocket) udp::CloseSocket(own_);
            if (shared_ != kNoSocket) udp::CloseSocket(shared_);
        }

        bool Open() override
        {
            if (!udp::Startup()) { LOGE("online: no Winsock"); return false; }
            sharedPort_ = uint16_t(env::Number("MW2_LAN_PORT", 3074));
            own_ = udp::OpenOwnSocket(sharedPort_, slot_);
            shared_ = udp::OpenSocket(sharedPort_, true);
            if (own_ == kNoSocket || shared_ == kNoSocket)
            {
                LOGE("online: lan could not open its sockets (shared port %u)", sharedPort_);
                return false;
            }
            sockaddr_in bound{};
            socklen_t length = sizeof bound;
            getsockname(own_, reinterpret_cast<sockaddr*>(&bound), &length);
            address_ = OwnAddress();
            bound.sin_addr.s_addr = htonl(address_);
            id_ = IdOf(bound);
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &bound.sin_addr, text, sizeof text);
            LOGI("online: lan at %s:%u, broadcasts on %u", text, ntohs(bound.sin_port), sharedPort_);
            // Discovery also goes to every connected subnet's broadcast
            // address: a limited broadcast (255.255.255.255) is filtered on
            // some routers and hotspots, while the subnet's own still passes.
            {
                std::string subnets;
                for (const Adapter& adapter : Adapters())
                {
                    if (adapter.netmask == 0 || adapter.netmask == 0xFFFFFFFFu) continue;
                    const uint32_t target = adapter.address | ~adapter.netmask;
                    if (target == adapter.address || target == INADDR_BROADCAST) continue;
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_addr.s_addr = htonl(target);
                    to.sin_port = htons(sharedPort_);
                    bool dup = false;
                    for (const sockaddr_in& have : broadcasts_)
                        dup = dup || have.sin_addr.s_addr == to.sin_addr.s_addr;
                    if (dup) continue;
                    broadcasts_.push_back(to);
                    in_addr bcast;
                    bcast.s_addr = to.sin_addr.s_addr;
                    char btext[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &bcast, btext, sizeof btext);
                    subnets += (subnets.empty() ? "" : ", ") + std::string(btext);
                }
                if (!subnets.empty()) LOGI("online: lan also announces to %s", subnets.c_str());
            }
            return true;
        }

        const char* Name() const override { return "lan"; }
        uint64_t LocalId() const override { return id_; }
        uint32_t Slot() const override { return slot_; }

        bool SendTo(uint64_t peer, const void* bytes, size_t size) override
        {
            return udp::SendDatagram(own_, bytes, size, AddressOf(peer));
        }

        bool SendAll(const void* bytes, size_t size) override
        {
            sockaddr_in everyone{};
            everyone.sin_family = AF_INET;
            everyone.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            everyone.sin_port = htons(sharedPort_);
            bool ok = udp::SendDatagram(own_, bytes, size, everyone);
            for (const sockaddr_in& subnet : broadcasts_)
                ok = udp::SendDatagram(own_, bytes, size, subnet) && ok;
#ifndef _WIN32   // there a send to a port nobody has comes back as an error on the socket
            for (uint32_t slot = 0; slot < kLocalCopies; slot++)
            {
                if (slot == slot_) continue;
                sockaddr_in copy{};
                copy.sin_family = AF_INET;
                copy.sin_addr.s_addr = htonl(address_);
                copy.sin_port = htons(uint16_t(sharedPort_ + 1 + slot));
                udp::SendDatagram(own_, bytes, size, copy);
            }
#endif
            return ok;
        }

        long Next(void* bytes, size_t size, uint64_t& from) override
        {
            for (Socket s : { own_, shared_ })
            {
                sockaddr_in address{};
                const long got = udp::ReceiveDatagram(s, bytes, size, address);
                if (got < 0) continue;
                from = IdOf(address);
                return got;
            }
            return -1;
        }

    private:
        static constexpr uint32_t kLocalCopies = 4;
        Socket own_ = kNoSocket, shared_ = kNoSocket;
        uint16_t sharedPort_ = 0;
        uint32_t address_ = 0;                  // the announced one, host order
        uint64_t id_ = 0;
        uint32_t slot_ = 0;
        std::vector<sockaddr_in> broadcasts_;   // subnet broadcasts, beside 255.255.255.255
    };
}

std::unique_ptr<online::Transport> online::MakeLanTransport() { return std::make_unique<Lan>(); }
std::unique_ptr<online::Service> online::MakeLan() { return MakePlayers(MakeLanTransport()); }

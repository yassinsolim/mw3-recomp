#pragma once
// A stand-in for the DemonWare lobby service of MW3's multiplayer, and of
// Advanced Warfare's.
//
// Beside Xbox LIVE, MW3 logs on to its publisher's servers: the lobby service
// keeps the player's rank and classes ("mpdata"), hands out the playlists and
// the time, and takes reports. The title ends an online game it cannot log on
// from with EXE_DEMONWARE_DISCONNECT. On the console it reaches the lobby
// through an Xbox LSP: it asks Live whether the logon includes the service,
// enumerates the title servers named "MW3 - DW ATVI LSP", turns one's address
// into a secure one and opens one TCP stream to port 1020 of it, on which
// nothing is encrypted (the console's security gateway did that).
//
// Nothing here opens a socket. The kernel answers those four steps
// (kernel/xam.cpp, kernel/net.cpp) with the constants below, and the stream it
// "connects" is a Lobby: what the title sends is parsed here, in this process,
// and what it receives was written here. So the title's DemonWare parsers
// never see a byte a stranger wrote, and no port is opened for the lobby.
// What players must share comes from the online service (service.h): the
// playlists.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace online::demonware
{
    // The Live service id the title's lobby is behind, the address its one
    // title server is listed under (198.18.0.1, of a block set aside for tests
    // that is never routed; no host socket uses it) and the lobby's port.
    constexpr uint32_t kLspService = 0x415607E9;
    constexpr uint32_t kServerAddress = 0xC6120001;
#if defined(AW_TITLE_MP)
    constexpr uint16_t kLobbyPort = 1027;
#else
    constexpr uint16_t kLobbyPort = 1020;
#endif

    // Whether there is a lobby to log on to: MW3's multiplayer or Advanced
    // Warfare's, signed in to Live (online::Live()).
    bool Served();

    // One stream to the lobby, as the title's socket sees it.
    class Lobby
    {
    public:
        virtual ~Lobby() = default;
        // What the title wrote. Whole messages in it are answered at once.
        virtual void Send(const uint8_t* bytes, size_t size) = 0;
        // Up to `room` bytes of the answers, without waiting; 0 when none.
        virtual size_t Receive(uint8_t* out, size_t room) = 0;
        virtual bool Readable() = 0;
        // The lobby hung up, on bytes that are not its protocol.
        virtual bool Closed() = 0;
    };
    // The player logged on, and the folder players' files are kept in.
    std::shared_ptr<Lobby> Connect(uint64_t xuid, const std::filesystem::path& files);

    // A request to one of the publisher's web servers (kernel/xhttp.cpp), from
    // the player with that XUID: its reply, or false for a server nothing
    // here stands in for. Advanced Warfare's multiplayer logs on with one,
    // before it connects to the lobby.
    bool Http(const std::string& host, const std::string& path, const std::string& body, uint64_t xuid,
              std::string& reply);
}

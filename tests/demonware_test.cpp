// What the DemonWare lobby stand-in (runtime/online/demonware.cpp) promises the
// title, and what it promises about bytes that are not the title's.
//
//   * The stream: the eight opening bytes, the logon answered with the
//     connection id, a keep-alive answered in kind, and the same answers
//     however the bytes are cut into sends.
//   * The player's files: a file that is not there is BD_NO_FILE (1000), which
//     is what makes the title start a new rank; an upload is read back whole,
//     from the folder of the player logged on and no other.
//   * The publisher's files: social.cfg, and the playlists as the aggregate the
//     title inflates -- checked by inflating it here, by the format and not by
//     the code under test.
//   * Bytes that are not the protocol: a stream that does not open as one, a
//     length no message has and an encrypted message end the connection; a
//     task whose parameters are cut short or name a path is refused, and
//     neither reads past what was sent nor writes outside the folder.
//
// Synthetic data only; it writes under a temporary directory of its own.
#include "online/demonware.h"
#include "online/service.h"
#include "playlists_data.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    // The online service the stand-in asks for the playlists: none, or one
    // that has them.
    class Relay final : public online::Service
    {
    public:
        bool Start() override { return true; }
        uint64_t LocalId() override { return 1; }
        uint64_t Account() override { return 1; }
        std::string LocalName() override { return {}; }
        bool Send(uint64_t, uint16_t, uint16_t, const void*, size_t) override { return false; }
        bool Broadcast(uint16_t, uint16_t, const void*, size_t) override { return false; }
        bool Receive(uint16_t, online::Datagram&) override { return false; }
        bool TitleFile(const std::string& name, Bytes& out) override
        {
            if (name != "playlists.info") return false;
            out.assign(playlists.begin(), playlists.end());
            return true;
        }
        std::string playlists;
    };
    Relay* g_relay = nullptr;
}

online::Service* online::Get() { return g_relay; }
bool online::Live() { return true; }

namespace
{
    constexpr uint64_t kPlayer = 0x0009000012345678ull;

    void Little(Bytes& to, uint64_t value, int size)
    {
        for (int i = 0; i < size; i++) to.push_back(uint8_t(value >> (8 * i)));
    }
    uint32_t Little32(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    // A task as the title frames it: length, "not encrypted", service, the
    // task number as a typed byte, the parameters and the spare byte the title
    // ends every task with.
    Bytes Task(uint8_t service, uint8_t task, const Bytes& parameters, bool spare)
    {
        Bytes frame;
        Little(frame, 4 + parameters.size() + spare, 4);
        frame.insert(frame.end(), { 0, service, 3, task });
        frame.insert(frame.end(), parameters.begin(), parameters.end());
        if (spare) frame.push_back(0);
        return frame;
    }
    void String(Bytes& to, const std::string& text)
    {
        to.push_back(16);
        to.insert(to.end(), text.begin(), text.end());
        to.push_back(0);
    }
    void UInt64(Bytes& to, uint64_t value) { to.push_back(10); Little(to, value, 8); }
    void Blob(Bytes& to, const Bytes& bytes)
    {
        to.push_back(19);
        to.push_back(8);
        Little(to, bytes.size(), 4);
        to.insert(to.end(), bytes.begin(), bytes.end());
    }

    const Bytes kHello = { 200, 0, 0, 0, 0xFF, 0xFF, 0, 0 };
    Bytes Logon()
    {
        Bytes frame;
        Little(frame, 2 + 4, 4);
        frame.insert(frame.end(), { 0, 7, 0x11, 0xF1, 0x11, 0 });
        return frame;
    }

    // Everything the lobby has to say now.
    Bytes Drain(online::demonware::Lobby& lobby)
    {
        Bytes all;
        uint8_t part[1000];
        while (const size_t got = lobby.Receive(part, sizeof part)) all.insert(all.end(), part, part + got);
        return all;
    }

    // One reply to a task, taken apart by the title's own rules.
    struct Reply
    {
        bool whole = false;
        uint32_t error = 0, results = 0;
        Bytes objects;
    };
    Reply TakeReply(Bytes& stream)
    {
        Reply reply;
        if (stream.size() < 4) return reply;
        const uint32_t size = Little32(stream.data());
        if (stream.size() < 4 + size_t(size) || size < 16) return reply;
        const uint8_t* p = stream.data() + 4;
        // not encrypted, a task reply, the transaction id, the error
        if (p[0] != 0 || p[1] != 1 || p[2] != 10 || p[11] != 8) return reply;
        reply.error = Little32(p + 12);
        if (!reply.error)
        {
            if (size < 23 || p[16] != 3 || p[18] != 8) return reply;
            reply.results = Little32(p + 19);
            const size_t at = reply.results ? 28 : 23;
            if (size < at) return reply;
            reply.objects.assign(p + at, p + size);
        }
        reply.whole = true;
        stream.erase(stream.begin(), stream.begin() + 4 + size);
        return reply;
    }

    Reply Ask(online::demonware::Lobby& lobby, uint8_t service, uint8_t task, const Bytes& parameters, bool spare = true)
    {
        const Bytes frame = Task(service, task, parameters, spare);
        lobby.Send(frame.data(), frame.size());
        Bytes stream = Drain(lobby);
        Reply reply = TakeReply(stream);
        if (!stream.empty()) reply.whole = false;
        return reply;
    }

    std::shared_ptr<online::demonware::Lobby> LoggedOn(const fs::path& files)
    {
        auto lobby = online::demonware::Connect(kPlayer, files);
        lobby->Send(kHello.data(), kHello.size());
        const Bytes logon = Logon();
        lobby->Send(logon.data(), logon.size());
        Drain(*lobby);
        return lobby;
    }

    void TestStream(const fs::path& files)
    {
        auto lobby = online::demonware::Connect(kPlayer, files);
        lobby->Send(kHello.data(), kHello.size());
        Check(!lobby->Readable(), "the opening bytes have no answer");
        const Bytes logon = Logon();
        lobby->Send(logon.data(), logon.size());
        const Bytes id = { 11, 0, 0, 0, 0, 4, 10, 1, 0, 0, 0, 0, 0, 0, 0 };
        Check(lobby->Readable() && Drain(*lobby) == id, "the logon is answered with the connection id");

        const Bytes alive(4, 0);
        lobby->Send(alive.data(), alive.size());
        Check(Drain(*lobby) == alive, "a keep-alive is answered in kind");
        Check(!lobby->Closed(), "the lobby stays connected");

        // The same stream a byte at a time gives the same answers.
        Bytes parameters;
        String(parameters, "social.cfg");
        Bytes stream = kHello;
        stream.insert(stream.end(), logon.begin(), logon.end());
        const Bytes task = Task(10, 7, parameters, true);
        stream.insert(stream.end(), task.begin(), task.end());
        auto whole = online::demonware::Connect(kPlayer, files);
        whole->Send(stream.data(), stream.size());
        auto pieces = online::demonware::Connect(kPlayer, files);
        for (const uint8_t byte : stream) pieces->Send(&byte, 1);
        const Bytes a = Drain(*whole), b = Drain(*pieces);
        Check(a.size() > id.size() && a == b, "answers do not depend on how the stream is cut");

        // A task nothing here knows succeeds with no results.
        const Reply unknown = Ask(*LoggedOn(files), 67, 1, {});
        Check(unknown.whole && unknown.error == 0 && unknown.results == 0, "an unknown task succeeds with nothing");
    }

    void TestPlayerFiles(const fs::path& files)
    {
        auto lobby = LoggedOn(files);
        Bytes get;
        String(get, "mpdata");
        UInt64(get, 0);
        Reply reply = Ask(*lobby, 10, 3, get);
        Check(reply.whole && reply.error == 1000, "a file that is not there is BD_NO_FILE");

        Bytes stats(0x3000);
        for (size_t i = 0; i < stats.size(); i++) stats[i] = uint8_t(i * 7 + (i >> 8));
        Bytes put;
        String(put, "mpdata");
        put.insert(put.end(), { 1, 1 });
        Blob(put, stats);
        reply = Ask(*lobby, 10, 1, put);
        Check(reply.whole && reply.error == 0 && reply.results == 1, "an upload is taken");
        Check(fs::file_size(files / "0009000012345678" / "mpdata") == stats.size(), "and kept under the player's XUID");

        // Another connection, as after a restart.
        reply = Ask(*LoggedOn(files), 10, 3, get);
        Bytes back;
        Blob(back, stats);
        Check(reply.whole && reply.error == 0 && reply.results == 1 && reply.objects == back, "and read back whole");

        Bytes list;
        UInt64(list, 0);
        list.insert(list.end(), { 8, 0, 0, 0, 0, 6, 10, 0, 6, 0, 0 });
        String(list, "mp");
        reply = Ask(*lobby, 10, 5, list);
        Check(reply.whole && reply.error == 0 && reply.results == 1, "a listing has the file, and not its backup");

        Bytes other = put;
        UInt64(other, kPlayer + 1);
        reply = Ask(*lobby, 10, 1, other);
        Check(reply.whole && reply.error == 1001 && !fs::exists(files / "0009000012345679"),
              "nothing is written for a player who is not logged on");

        for (const char* name : { "../mpdata", "a/b", "c:d", "..", "mpdata.bak", "" })
        {
            Bytes bad;
            String(bad, name);
            bad.insert(bad.end(), { 1, 1 });
            Blob(bad, stats);
            reply = Ask(*lobby, 10, 1, bad);
            Check(reply.whole && reply.error == 106, std::string("a file cannot be named \"") + name + "\"");
        }
        size_t kept = 0;
        for (const auto& entry : fs::recursive_directory_iterator(files)) kept += entry.is_regular_file();
        Check(kept == 1 && !fs::exists(files.parent_path() / "mpdata"), "and none of those wrote anything");

        Bytes remove;
        String(remove, "mpdata");
        reply = Ask(*lobby, 10, 2, remove);
        Check(reply.whole && reply.error == 0 && !fs::exists(files / "0009000012345678" / "mpdata"), "a file is removed");
    }

    // A zlib stream of stored blocks, inflated by the format.
    bool Inflate(const uint8_t* p, size_t size, Bytes& out)
    {
        if (size < 6 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[0] & 0x0F) != 8 || (p[1] & 0x20)) return false;
        size_t at = 2;
        for (bool last = false; !last;)
        {
            if (at + 5 > size || (p[at] & 6) != 0) return false;
            last = p[at] & 1;
            const uint32_t length = p[at + 1] | (p[at + 2] << 8), check = p[at + 3] | (p[at + 4] << 8);
            if ((length ^ check) != 0xFFFF || at + 5 + length > size) return false;
            out.insert(out.end(), p + at + 5, p + at + 5 + length);
            at += 5 + length;
        }
        uint32_t a = 1, b = 0;
        for (const uint8_t byte : out)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        return at + 4 == size && ((uint32_t(p[at]) << 24) | (p[at + 1] << 16) | (p[at + 2] << 8) | p[at + 3]) == ((b << 16) | a);
    }

    // The text inside a playlists.aggr reply; empty when it is not one.
    std::string Playlists(const Reply& reply)
    {
        // a blob: its type, its length as a typed number, the bytes
        if (!reply.whole || reply.error || reply.results != 1 || reply.objects.size() < 14 || reply.objects[0] != 19) return {};
        const uint8_t* file = reply.objects.data() + 6;
        const size_t size = Little32(reply.objects.data() + 2);
        if (size + 6 != reply.objects.size() || size > 0xF000) return {};
        const auto big16 = [&](size_t at) { return size_t(file[at] << 8 | file[at + 1]); };
        const size_t offset = big16(2), length = big16(4);
        if (big16(0) != 1 || offset + length != size || offset < 8 + 4 * 256) return {};
        for (size_t at = 8; at < offset; at++)
            if (file[at]) return {};   // every recipe is empty
        Bytes text;
        if (!Inflate(file + offset, length, text)) return {};
        return std::string(text.begin(), text.end());
    }

    void TestPublisherFiles(const fs::path& files)
    {
        auto lobby = LoggedOn(files);
        Bytes name;
        String(name, "social.cfg");
        Reply reply = Ask(*lobby, 10, 7, name);
        const std::string social(reply.objects.begin(), reply.objects.end());
        Check(reply.whole && reply.error == 0 && reply.results == 1 && social.find("set facebook_active 0\n") != std::string::npos,
              "social.cfg is served");

        name.clear();
        String(name, "playlists.aggr");
        Check(Playlists(Ask(*lobby, 10, 7, name)) == std::string(reinterpret_cast<const char*>(kPlaylists), kPlaylistsSize),
              "without a relay the playlists are the build's, as the aggregate the title inflates");

        Relay relay;
        relay.playlists = "version 7\nplaylist 1\n";
        g_relay = &relay;
        Check(Playlists(Ask(*lobby, 10, 7, name)) == relay.playlists, "a relay's playlists come first");
        relay.playlists.assign(0xF000, 'x');
        reply = Ask(*lobby, 10, 7, name);
        Check(reply.whole && reply.error == 1000, "playlists the title has no room for are not served");
        g_relay = nullptr;

        name.clear();
        String(name, "motd-english.txt");
        reply = Ask(*lobby, 10, 7, name);
        Check(reply.whole && reply.error == 1000, "a publisher's file nothing here has is BD_NO_FILE");

        reply = Ask(*lobby, 12, 6, {});
        Check(reply.whole && reply.error == 0 && reply.results == 1 && reply.objects.size() == 5 && reply.objects[0] == 8,
              "the time is one number");
    }

    void TestStrangeBytes(const fs::path& files)
    {
        const Bytes garbage = { 'G', 'E', 'T', ' ', '/', ' ', 'H', 'T' };
        auto lobby = online::demonware::Connect(kPlayer, files);
        lobby->Send(garbage.data(), garbage.size());
        Check(lobby->Closed() && Drain(*lobby).empty(), "a stream that does not open as the lobby's is hung up on");
        lobby->Send(kHello.data(), kHello.size());
        Check(lobby->Closed(), "and stays hung up");

        lobby = LoggedOn(files);
        const Bytes huge = { 0, 0, 1, 0 };
        lobby->Send(huge.data(), huge.size());
        Check(lobby->Closed(), "so is a length no message has");

        lobby = LoggedOn(files);
        const Bytes secret = { 3, 0, 0, 0, 1, 10, 3 };
        lobby->Send(secret.data(), secret.size());
        Check(lobby->Closed(), "and an encrypted message");

        lobby = LoggedOn(files);
        const Bytes numberless = { 3, 0, 0, 0, 0, 10, 9 };
        lobby->Send(numberless.data(), numberless.size());
        Check(lobby->Closed(), "and a task with no number");

        // Parameters cut short, of the wrong type, or longer than they say.
        lobby = LoggedOn(files);
        const Bytes cuts[] = {
            {},                                             // nothing
            { 16, 'm', 'p', 'd', 'a', 't', 'a' },           // a name that never ends
            { 10, 1, 2, 3 },                                // a number where the name goes
            { 16, 'm', 0, 1, 1, 19, 8, 0xFF, 0xFF, 0xFF, 0x7F, 1, 2, 3 },   // a blob longer than the message
            { 16, 'm', 0, 1, 1, 19, 8, 4, 0 },              // a length cut short
        };
        for (const Bytes& cut : cuts)
            for (const uint8_t task : { uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(5), uint8_t(6), uint8_t(7), uint8_t(8) })
            {
                const Reply reply = Ask(*lobby, 10, task, cut, false);
                Check(reply.whole && reply.error != 0, "parameters that are not the task's are refused");
            }
        Bytes longName;
        String(longName, std::string(0x80, 'a'));
        Check(Ask(*lobby, 10, 7, longName).error == 106, "so is a name longer than a file's");
        Check(!lobby->Closed(), "and the lobby goes on");
        std::error_code ec;
        Check(fs::is_empty(files, ec) || !fs::exists(files), "none of it wrote a file");
    }
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "mw2-demonware-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path files = root / "online" / "user";

    TestStream(files);
    TestPlayerFiles(files);
    TestPublisherFiles(files);
    fs::remove_all(root, ec);
    TestStrangeBytes(files);
    fs::remove_all(root, ec);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

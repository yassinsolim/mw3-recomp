// The DemonWare lobby stand-in (demonware.h): the wire protocol of the title's
// bdLobbyService, read in the disc's default_mp.xex (the addresses below are
// its), and what each task of the menu is answered with.
//
// The stream. The title opens with eight bytes, the little-endian words 200
// and the largest message it takes, and from then on both sides send
// messages: a little-endian length, a byte that says whether the rest is
// encrypted (never, here: bdLobbyService::connect clears the flag at
// 8252B3D4) and the payload. A length of zero is a keep-alive: the title
// sends one after 40 s without sending and hangs up after 120 s without
// receiving (bdLobbyConnection::pump, 8253DD78), so each is answered in kind.
//
// What the title sends. A payload starts with a service number. 7 is the
// logon, a bit buffer with the DemonWare title id and the ticket (the player's
// XUID and gamertag; nothing here needs them), answered with the connection
// id. Any other is a task of that service: the task number and its
// parameters as a bdByteBuffer, in which every value follows a byte naming
// its type (bdRemoteTaskManager::initTaskBuffer, 8252E218).
//
// What it receives. A payload starts with the message type
// (bdLobbyService::pump, 8252B530): 1 is the reply to the oldest task still
// waiting, 4 the connection id. A task reply is the transaction id, the error
// code and, when that is zero, the task number, the number of results, the
// number there are in all when any follow, and the results
// (bdRemoteTask::handleTaskReply, 825285C8, and deserializeTaskReply,
// 82528928). The title takes replies in the order it sent the tasks, so every
// task is answered, at once, in order.
//
// The bytes come from the title in this process, not from a stranger, but
// they are parsed as if they did: every length is checked against what is
// there, and a stream that is not this protocol is hung up on.
#include "demonware.h"
#include "service.h"
#include "../env.h"
#include "../kernel/durable.h"
#include "../log.h"

#include "playlists_data.h"   // generated: the playlists this release was built with

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    using Bytes = std::vector<uint8_t>;

    // bdByteBuffer's type bytes.
    constexpr uint8_t kBool = 1, kUByte8 = 3, kUInt16 = 6, kUInt32 = 8, kUInt64 = 10, kString = 16, kBlob = 19;

    // bdLobbyErrorCode.
    constexpr uint32_t kNoError = 0, kResultExceedsBuffer = 100, kParamParseError = 106, kNoFile = 1000,
                       kPermissionDenied = 1001;

    constexpr uint32_t kHello = 200;
    constexpr uint8_t kLogon = 7;
    constexpr uint8_t kTaskReply = 1, kConnectionId = 4;

    // The largest message either side sends: bdLobbyService::connect asks for
    // buffers of 0xFFFF bytes (8252BA60), and the title hangs up on a longer
    // one. A file that cannot fit in a reply is refused instead.
    constexpr size_t kLargestMessage = 0xFFFF;
    constexpr size_t kLargestFile = kLargestMessage - 64;
    constexpr size_t kNameSize = 0x80;   // a file's name, with its terminator

    // MW2_TRACE_DW=1: a line for the logon and for every task, with its
    // service, number, what it named and the size of the reply ("dw:").
    bool Traced()
    {
        static const bool traced = env::Flag("MW2_TRACE_DW");
        return traced;
    }

    uint32_t Little32(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    // A task's parameters. A value that is not there, or not of the type
    // asked for, makes this and every later read fail and give zero.
    class Reader
    {
    public:
        Reader(const uint8_t* at, size_t size) : at_(at), end_(at + size) {}

        bool Ok() const { return ok_; }
        size_t Left() const { return size_t(end_ - at_); }
        // Whether the next value is of that type, for one a task may leave out.
        bool Next(uint8_t type) const { return ok_ && Left() && *at_ == type; }

        bool Bool() { return Is(kBool) && Byte() != 0; }
        uint8_t UByte8() { return Is(kUByte8) ? Byte() : 0; }
        uint16_t UInt16()
        {
            uint8_t b[4]{};
            if (Is(kUInt16)) Take(b, 2);
            return uint16_t(Little32(b));
        }
        uint32_t UInt32()
        {
            uint8_t b[4]{};
            if (Is(kUInt32)) Take(b, sizeof b);
            return Little32(b);
        }
        uint64_t UInt64()
        {
            uint8_t b[8]{};
            if (Is(kUInt64)) Take(b, sizeof b);
            return uint64_t(Little32(b)) | (uint64_t(Little32(b + 4)) << 32);
        }
        // Text of fewer than `size` characters, which must end inside the
        // message.
        std::string String(size_t size)
        {
            if (!Is(kString)) return {};
            const auto* end = static_cast<const uint8_t*>(std::memchr(at_, 0, Left()));
            if (!end || size_t(end - at_) >= size) { ok_ = false; return {}; }
            std::string text(reinterpret_cast<const char*>(at_), size_t(end - at_));
            at_ = end + 1;
            return text;
        }
        Bytes Blob(size_t most)
        {
            if (!Is(kBlob)) return {};
            const uint32_t size = UInt32();
            if (!ok_ || size > Left() || size > most) { ok_ = false; return {}; }
            Bytes bytes(at_, at_ + size);
            at_ += size;
            return bytes;
        }

    private:
        uint8_t Byte()
        {
            uint8_t value = 0;
            Take(&value, 1);
            return value;
        }
        bool Is(uint8_t type)
        {
            if (ok_ && Byte() != type) ok_ = false;
            return ok_;
        }
        void Take(uint8_t* out, size_t size)
        {
            if (!ok_ || Left() < size) { ok_ = false; return; }
            std::memcpy(out, at_, size);
            at_ += size;
        }

        const uint8_t* at_;
        const uint8_t* end_;
        bool ok_ = true;
    };

    class Writer
    {
    public:
        void Byte(uint8_t value) { bytes_.push_back(value); }
        void Bool(bool value) { Byte(kBool); Byte(value ? 1 : 0); }
        void UByte8(uint8_t value) { Byte(kUByte8); Byte(value); }
        void UInt32(uint32_t value) { Byte(kUInt32); Little(value, 4); }
        void UInt64(uint64_t value) { Byte(kUInt64); Little(value, 8); }
        void String(const std::string& text)
        {
            Byte(kString);
            bytes_.insert(bytes_.end(), text.begin(), text.end());
            Byte(0);
        }
        void Blob(const Bytes& bytes)
        {
            Byte(kBlob);
            UInt32(uint32_t(bytes.size()));
            bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        }
        void Append(const Writer& other) { bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end()); }
        const Bytes& Written() const { return bytes_; }

    private:
        void Little(uint64_t value, int size)
        {
            for (int i = 0; i < size; i++) Byte(uint8_t(value >> (8 * i)));
        }

        Bytes bytes_;
    };

    // What a task is answered with: an error, or results of the task's own
    // type, each written as the title's deserialiser reads it.
    struct Answer
    {
        uint32_t error = kNoError;
        uint32_t results = 0;
        Writer objects;
        std::string note;   // for the trace: what the task named
    };

    // ---- bdStorage (service 10) --------------------------------------------
    //
    // The player's files are kept where MW2's Live storage is (docs/saves.md):
    // saves/online/user/<xuid>/<name>, the folder the kernel names when the
    // title connects. The one that matters is "mpdata", the rank, classes and
    // statistics, which the title uploads as they change.

    // A name the title may keep a file under: letters, digits, '.', '_' and
    // '-', so it is one path component on every host, and not ending as the
    // copies an atomic write keeps beside a file do (kernel/durable.h).
    bool FileName(const std::string& name)
    {
        if (name.empty() || name.size() >= kNameSize || name == "." || name == "..") return false;
        if (name.ends_with(".bak") || name.ends_with(".tmp")) return false;
        for (const char c : name)
        {
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '.' || c == '_' || c == '-';
            if (!plain) return false;
        }
        return true;
    }

    // Who is logged on, and where players' files are kept: a folder for each
    // player, named by XUID.
    struct Player
    {
        uint64_t xuid = 0;
        fs::path files;

        fs::path Folder(uint64_t owner) const
        {
            char name[20];
            std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(owner));
            return files / name;
        }
        // The player a task is about: the one it names, or the one logged on.
        uint64_t Owner(uint64_t named) const { return named ? named : xuid; }
    };

    // The number the lobby knows a file by, which the title hands back to
    // replace it: the same for a name on every run, so nothing has to be kept.
    uint64_t FileId(const std::string& name)
    {
        uint64_t hash = 0xCBF29CE484222325ull;   // FNV-1a
        for (const char c : name) hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
        return hash | 1;
    }

    bool ReadFile(const fs::path& path, Bytes& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }

    // bdFileInfo, as 825281C8 reads it. The lobby keeps no dates: the title
    // shows none of a file it keeps for itself.
    void WriteFileInfo(Writer& to, const std::string& name, uint64_t owner, size_t size)
    {
        to.UInt32(uint32_t(size));
        to.UInt64(FileId(name));
        to.UInt32(0);         // created
        to.UInt32(0);         // modified
        to.Bool(owner != 0);  // private: a player's, not the publisher's
        to.UInt64(owner);
        to.String(name);
    }

    // A player's files are written for the one logged on, never for another.
    bool StoreFile(const Player& player, const std::string& name, const Bytes& bytes)
    {
        if (!kernel::WriteFileAtomically(player.Folder(player.xuid) / name, bytes.data(), bytes.size()))
        {
            LOGW("dw: %s could not be kept for %016llx", name.c_str(), static_cast<unsigned long long>(player.xuid));
            return false;
        }
        LOGI("dw: kept %s for %016llx, %zu bytes", name.c_str(), static_cast<unsigned long long>(player.xuid),
             bytes.size());
        return true;
    }

    // uploadFile(name, private, bytes, owner), 82533908. The title leaves the
    // owner out for the player's own file.
    void UploadFile(const Player& player, Reader& in, Answer& answer)
    {
        const std::string name = in.String(kNameSize);
        in.Bool();
        const Bytes bytes = in.Blob(kLargestMessage);
        const uint64_t owner = player.Owner(in.Next(kUInt64) ? in.UInt64() : 0);
        answer.note = name;
        if (!in.Ok() || !FileName(name)) { answer.error = kParamParseError; return; }
        if (owner != player.xuid || !StoreFile(player, name, bytes)) { answer.error = kPermissionDenied; return; }
        WriteFileInfo(answer.objects, name, owner, bytes.size());
        answer.results = 1;
    }

    // The file of the player's that has this number.
    bool NameOfFile(const Player& player, uint64_t id, std::string& name)
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(player.Folder(player.xuid), ec))
        {
            name = entry.path().filename().string();
            if (FileName(name) && FileId(name) == id) return true;
        }
        return false;
    }

    // Task 8 (82534080): (file id, bytes), new contents for a file uploaded
    // before. It has no result.
    void UpdateFile(const Player& player, Reader& in, Answer& answer)
    {
        const uint64_t id = in.UInt64();
        const Bytes bytes = in.Blob(kLargestMessage);
        std::string name;
        if (!in.Ok()) { answer.error = kParamParseError; return; }
        if (!NameOfFile(player, id, name)) { answer.error = kNoFile; return; }
        answer.note = name;
        if (!StoreFile(player, name, bytes)) answer.error = kPermissionDenied;
    }

    // removeFile(name, owner), 82533BE0.
    void RemoveFile(const Player& player, Reader& in, Answer& answer)
    {
        const std::string name = in.String(kNameSize);
        const uint64_t owner = player.Owner(in.Next(kUInt64) ? in.UInt64() : 0);
        answer.note = name;
        if (!in.Ok() || !FileName(name)) { answer.error = kParamParseError; return; }
        if (owner != player.xuid) { answer.error = kPermissionDenied; return; }
        std::error_code ec;
        if (!fs::remove(player.Folder(owner) / name, ec)) answer.error = kNoFile;
    }

    // The rest of what a listing is asked with: (since, most, from, filter),
    // 82534200 and 825344D0. Files are listed by name, those whose names
    // start with the filter, as many as are asked for and fit a reply.
    struct Listing
    {
        uint32_t most = 0, from = 0, seen = 0;
        std::string filter;

        explicit Listing(Reader& in)
        {
            in.UInt32();   // changed since: no dates are kept
            most = in.UInt16();
            from = in.UInt16();
            if (in.Next(kString)) filter = in.String(kNameSize);
        }
        void Add(const std::string& name, uint64_t owner, size_t size, Answer& answer)
        {
            if (name.compare(0, filter.size(), filter) != 0 || seen++ < from) return;
            if (answer.results >= most || answer.objects.Written().size() + 64 + name.size() > kLargestFile) return;
            WriteFileInfo(answer.objects, name, owner, size);
            answer.results++;
        }
    };

    // listFilesByOwner(owner, ...), 82534200.
    void ListFiles(const Player& player, Reader& in, Answer& answer)
    {
        const uint64_t owner = player.Owner(in.UInt64());
        Listing listing(in);
        answer.note = listing.filter;
        if (!in.Ok()) { answer.error = kParamParseError; return; }
        std::vector<std::pair<std::string, size_t>> files;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(player.Folder(owner), ec))
        {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file(ec) || !FileName(name)) continue;
            files.emplace_back(name, size_t(entry.file_size(ec)));
        }
        std::sort(files.begin(), files.end());
        for (const auto& [name, size] : files) listing.Add(name, owner, size, answer);
    }

    // bdFileData is the bytes alone (82528070).
    void GiveFile(const Bytes& bytes, Answer& answer)
    {
        if (bytes.size() > kLargestFile) { answer.error = kResultExceedsBuffer; return; }
        answer.objects.Blob(bytes);
        answer.results = 1;
    }

    // getFile(name, owner), 82533E30.
    void GetFile(const Player& player, Reader& in, Answer& answer)
    {
        const std::string name = in.String(kNameSize);
        const uint64_t owner = player.Owner(in.UInt64());
        answer.note = name;
        if (!in.Ok() || !FileName(name)) { answer.error = kParamParseError; return; }
        const fs::path path = player.Folder(owner) / name;
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) { answer.error = kNoFile; return; }
        if (size > kLargestFile) { answer.error = kResultExceedsBuffer; return; }
        Bytes bytes;
        if (!ReadFile(path, bytes)) { answer.error = kNoFile; return; }
        GiveFile(bytes, answer);
    }

    // ---- the publisher's files ---------------------------------------------
    //
    // Two of them hold the menu up until they arrive (the test is 82351E18,
    // which closes "Fetching Online Profile"): social.cfg and
    // playlists.aggr. The others -- the message of the day, the fast file of
    // the day, the heat map -- are asked for once and done without.

    // social.cfg is `set <dvar> <number>` lines (823CCB20) that turn on what
    // the publisher's other servers carry. None of them is here, so each is
    // turned off, and the title does not ask for what would never be answered.
    constexpr char kSocial[] =
        "set theater_active 0\n"
        "set facebook_active 0\n"
        "set facebook_friends_active 0\n"
        "set facebook_upload_video_active 0\n"
        "set facebook_upload_photo_active 0\n"
        "set entitlements_active 0\n"
        "set userGroup_active 0\n"
        "set elite_clan_active 0\n"
        "set dw_presence_active 0\n"
        "set onlinevault_active 0\n"
        "set past_title_data_active 0\n"
        "set matchdata_active 0\n"
        "set dw_leaderboard_write_active 0\n";

    // The playlists, as text: the online service's, the same for everyone on a
    // relay, else the list this release was built with (relay/playlists_mw3.info).
    Bytes PlaylistText()
    {
        Bytes text;
        auto* service = online::Get();
        if (!service || !service->TitleFile("playlists.info", text)) text.assign(kPlaylists, kPlaylists + kPlaylistsSize);
        return text;
    }

    // The title takes its playlists only as playlists.aggr (8228B510): a header
    // of big-endian 16-bit words -- 1, where the playlists start and their
    // size, a spare, then where and how long each of the game-type recipes a
    // playlist can name is (8228B790) -- and the playlists as a zlib stream,
    // which it inflates into 0x3C000 bytes (8228B720). There are no recipes
    // here, and all 256 a `recipe` line can name are given as empty, so that
    // naming one reads nothing rather than the playlists as a recipe. The
    // stream is stored, not compressed: nothing else in the game needs a
    // compressor, and the list is small. The whole file must fit the title's
    // 0xF000 bytes; false when the text is too long for that.
    bool PlaylistAggregate(const Bytes& text, Bytes& file)
    {
        constexpr size_t kRecipes = 256, kHeader = 8 + 4 * kRecipes, kLargest = 0xF000, kBlock = 0xFFFF;
        constexpr size_t kStream = 2 + 5 + 4;   // zlib's header, one stored block's, the checksum
        if (text.empty() || text.size() > kBlock || kHeader + kStream + text.size() > kLargest) return false;
        const auto big16 = [&](size_t value) { file.push_back(uint8_t(value >> 8)); file.push_back(uint8_t(value)); };
        file.clear();
        big16(1);
        big16(kHeader);
        big16(kStream + text.size());
        file.resize(kHeader, 0);
        file.push_back(0x78);   // deflate, a 32 KB window
        file.push_back(0x01);   // no dictionary; makes the pair a multiple of 31
        file.push_back(0x01);   // the last block, stored
        file.push_back(uint8_t(text.size()));
        file.push_back(uint8_t(text.size() >> 8));
        file.push_back(uint8_t(~text.size()));
        file.push_back(uint8_t(~text.size() >> 8));
        file.insert(file.end(), text.begin(), text.end());
        uint32_t a = 1, b = 0;   // Adler-32
        for (const uint8_t byte : text)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        for (int shift = 24; shift >= 0; shift -= 8) file.push_back(uint8_t(((b << 16) | a) >> shift));
        return true;
    }

    bool PublisherFile(const std::string& name, Bytes& bytes)
    {
        if (name == "social.cfg")
        {
            bytes.assign(kSocial, kSocial + sizeof kSocial - 1);
            return true;
        }
        if (name != "playlists.aggr") return false;
        const Bytes text = PlaylistText();
        if (PlaylistAggregate(text, bytes)) return true;
        LOGW("dw: the playlists are %zu bytes, more than the title takes; Find Game has nothing to offer", text.size());
        return false;
    }

    // getPublisherFile(name), 82534778.
    void GetPublisherFile(Reader& in, Answer& answer)
    {
        const std::string name = in.String(kNameSize);
        answer.note = name;
        if (!in.Ok() || !FileName(name)) { answer.error = kParamParseError; return; }
        Bytes bytes;
        if (!PublisherFile(name, bytes)) { answer.error = kNoFile; return; }
        GiveFile(bytes, answer);
    }

    // listAllPublisherFiles(...), 825344D0.
    void ListPublisherFiles(Reader& in, Answer& answer)
    {
        Listing listing(in);
        answer.note = listing.filter;
        if (!in.Ok()) { answer.error = kParamParseError; return; }
        for (const char* name : { "playlists.aggr", "social.cfg" })
            if (Bytes bytes; PublisherFile(name, bytes)) listing.Add(name, 0, bytes.size(), answer);
    }

    void Storage(const Player& player, uint8_t task, Reader& in, Answer& answer)
    {
        switch (task)
        {
        case 1: UploadFile(player, in, answer); break;
        case 2: RemoveFile(player, in, answer); break;
        case 3: GetFile(player, in, answer); break;
        case 5: ListFiles(player, in, answer); break;
        case 6: ListPublisherFiles(in, answer); break;
        case 7: GetPublisherFile(in, answer); break;
        case 8: UpdateFile(player, in, answer); break;
        default: break;
        }
    }

    // ---- bdTitleUtilities (service 12) -------------------------------------

    // getServerTime (8252DC88): a bdTimeStamp, seconds since 1970.
    void TitleUtilities(uint8_t task, Reader&, Answer& answer)
    {
        if (task != 6) return;
        answer.objects.UInt32(uint32_t(std::time(nullptr)));
        answer.results = 1;
    }

    const char* ServiceName(uint8_t service)
    {
        switch (service)
        {
        case 3: return "teams";
        case 4: return "stats";
        case 6: return "messaging";
        case 8: return "profiles";
        case 10: return "storage";
        case 12: return "title utilities";
        case 15: return "key archive";
        case 21: return "matchmaking";
        case 23: return "counter";
        case 27: return "dml";
        case 28: return "group";
        case 36: return "facebook";
        case 38: return "anti-cheat";
        case 50: return "content streaming";
        case 52: return "tags";
        case 65: return "user groups";
        case 67: return "event log";
        case 68: return "rich presence";
        default: return "unknown";
        }
    }

    // A task the menu does not depend on -- reports, counters, mail, clans,
    // the theater, the leaderboards, where the player is and how many play
    // each playlist -- succeeds with no results, which each of the title's
    // callers takes for "nothing there".
    void Serve(const Player& player, uint8_t service, uint8_t task, Reader& in, Answer& answer)
    {
        switch (service)
        {
        case 10: Storage(player, task, in, answer); break;
        case 12: TitleUtilities(task, in, answer); break;
        default: break;
        }
    }

    class LocalLobby final : public online::demonware::Lobby
    {
    public:
        LocalLobby(uint64_t xuid, const fs::path& files) : player_{ xuid, files } {}

        void Send(const uint8_t* bytes, size_t size) override
        {
            std::lock_guard g(lock_);
            if (closed_) return;
            in_.insert(in_.end(), bytes, bytes + size);
            size_t at = 0;
            while (!closed_ && Next(at)) {}
            in_.erase(in_.begin(), in_.begin() + ptrdiff_t(at));
        }

        size_t Receive(uint8_t* out, size_t room) override
        {
            std::lock_guard g(lock_);
            const size_t got = std::min(room, out_.size());
            std::memcpy(out, out_.data(), got);
            out_.erase(out_.begin(), out_.begin() + ptrdiff_t(got));
            return got;
        }

        bool Readable() override
        {
            std::lock_guard g(lock_);
            return !out_.empty();
        }

        bool Closed() override
        {
            std::lock_guard g(lock_);
            return closed_ && out_.empty();
        }

    private:
        // Takes the next whole message at `at` of what the title sent; false
        // when the rest is not one yet.
        bool Next(size_t& at)
        {
            const size_t left = in_.size() - at;
            if (left < 4) return false;
            const uint32_t size = Little32(&in_[at]);
            if (!greeted_)
            {
                if (size != kHello) return HangUp("does not start as a lobby connection");
                if (left < 8) return false;
                at += 8;
                greeted_ = true;
                return true;
            }
            if (size > kLargestMessage) return HangUp("sent a message longer than the lobby takes");
            if (left - 4 < size) return false;
            if (size) Message(&in_[at + 4], size);
            else
            {
                out_.insert(out_.end(), 4, 0);   // a keep-alive, answered in kind
                if (Traced()) LOGI("dw: keep-alive");
            }
            at += 4 + size;
            return true;
        }

        bool HangUp(const char* why)
        {
            LOGW("dw: the title %s; the lobby hangs up", why);
            closed_ = true;
            return false;
        }

        void Message(const uint8_t* bytes, size_t size)
        {
            if (size < 2 || bytes[0] != 0) { HangUp("sent an encrypted or empty message"); return; }
            const uint8_t service = bytes[1];
            if (service == kLogon)
            {
                // The id is only stored (bdRemoteTaskManager +0x50).
                Writer id;
                id.Byte(kConnectionId);
                id.UInt64(1);
                Reply(id);
                LOGI("dw: the title logged on to the lobby");
                return;
            }
            Reader in(bytes + 2, size - 2);
            const uint8_t task = in.UByte8();
            if (!in.Ok()) { HangUp("sent a task with no number"); return; }
            const size_t parameters = in.Left();
            Answer answer;
            Serve(player_, service, task, in, answer);

            Writer reply;
            reply.Byte(kTaskReply);
            reply.UInt64(++transaction_);
            reply.UInt32(answer.error);
            if (answer.error == kNoError)
            {
                reply.UByte8(task);
                reply.UInt32(answer.results);
                if (answer.results)
                {
                    reply.UInt32(answer.results);
                    reply.Append(answer.objects);
                }
            }
            Reply(reply);
            if (Traced())
            {
                // What the task named is the title's text: only its plain characters are logged.
                for (char& c : answer.note)
                    if (c < 0x20 || c > 0x7E) c = '?';
                LOGI("dw: %s (%u) task %u%s%s, %zu bytes: error %u, %u results, reply of %zu bytes",
                     ServiceName(service), service, task, answer.note.empty() ? "" : " ", answer.note.c_str(),
                     parameters, answer.error, answer.results, reply.Written().size());
            }
        }

        void Reply(const Writer& payload)
        {
            const uint32_t size = uint32_t(payload.Written().size() + 1);
            for (int i = 0; i < 4; i++) out_.push_back(uint8_t(size >> (8 * i)));
            out_.push_back(0);   // not encrypted
            out_.insert(out_.end(), payload.Written().begin(), payload.Written().end());
        }

        const Player player_;
        std::mutex lock_;
        Bytes in_, out_;
        bool greeted_ = false, closed_ = false;
        uint64_t transaction_ = 0;
    };
}

bool online::demonware::Served()
{
#if defined(MW3_TITLE_MP)
    return online::Live();
#else
    return false;
#endif
}

std::shared_ptr<online::demonware::Lobby> online::demonware::Connect(uint64_t xuid, const fs::path& files)
{
    LOGI("dw: the title connects to the lobby");
    return std::make_shared<LocalLobby>(xuid, files);
}

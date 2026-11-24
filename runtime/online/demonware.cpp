// The DemonWare lobby stand-in (demonware.h): the wire protocol of the title's
// bdLobbyService, read in the disc's default_mp.xex (the addresses below are
// its), and what each task of the menu is answered with.
//
// Advanced Warfare's multiplayer has a later build of the same library and
// speaks the same protocol, unencrypted too, to port 1027: what differs is
// under AW_TITLE_MP, with that executable's addresses (a logon over HTTPS
// first, the player's files under other task numbers, and matchmaking).
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
#include "../kernel/kernel.h"
#include "../log.h"

#include "playlists_data.h"   // generated: the playlists this release was built with

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
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
    constexpr uint8_t kBool = 1, kUByte8 = 3, kUInt16 = 6, kInt32 = 7, kUInt32 = 8, kUInt64 = 10, kFloat32 = 13,
                      kString = 16, kBlob = 19;

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
        // A signed word and a float's bits, as written.
        uint32_t Int32Bits() { return Word(kInt32); }
        uint32_t Float32Bits() { return Word(kFloat32); }
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
        uint32_t Word(uint8_t type)
        {
            uint8_t b[4]{};
            if (Is(type)) Take(b, sizeof b);
            return Little32(b);
        }
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
        void Int32Bits(uint32_t value) { Byte(kInt32); Little(value, 4); }
        void Float32Bits(uint32_t value) { Byte(kFloat32); Little(value, 4); }
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
        // The online service's search the results are those of, once it has
        // them (FinishSearch): the reply waits for it, and every later one
        // behind it.
        uint32_t search = 0;
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
#if defined(AW_TITLE_MP)
        // Advanced Warfare's library has the player's files under later
        // numbers (upload 821C4128, get 821C42A0, list 821C43E0), each with
        // a context name first, which is empty.
        case 10: in.String(kNameSize); UploadFile(player, in, answer); break;
        case 12: in.String(kNameSize); GetFile(player, in, answer); break;
        case 13: in.String(kNameSize); ListFiles(player, in, answer); break;
#endif
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

#if defined(AW_TITLE_MP)
    // ---- bdMatchMaking (service 138) ---------------------------------------
    //
    // Advanced Warfare finds its public matches here, not by XSessionSearch:
    // a host creates a session (task 1, 821AF470), updates it as its lobby
    // changes (2, 821AF5B0) and deletes it (3, 821AF6F8), and a player
    // looking for one asks for those that match a query (16, 821AF978).
    //
    // A session is what the console's has (the host's XNADDR, and the
    // session's id and key in the title's part), a game type, how many it
    // takes, and fifteen words of the title's (its class is read at 827A9810;
    // the title fills them where it sets the console's contexts and
    // properties, 827AD1C8, 827ACE70, 8234F7C8 and 827ACEF0 for the first
    // four): the playlist, a rule of it, the protocol and the map packs,
    // which a query names too and which must be the same; the public slots
    // still open, which a query asks at least so many of; then what the
    // publisher's servers weighed a match by (where the host is, its skill),
    // with the private slots and how many are taken among them.
    //
    // The sessions are the online service's listings (service.h), so the
    // players of one relay find each other. A listing is 60 bytes the service
    // does not read and four counts: the id, the key and eight of the words
    // are the bytes, the slots are the counts, and what a query compares are
    // its numbers. The host's address is not carried: the service says who
    // listed a session, and that is the address (kernel/net.cpp).
    constexpr size_t kAddressSize = 36, kSessionIdSize = 8, kKeySize = 16, kWords = 15;
    constexpr size_t kPlaylist = 0, kMapPacks = 3, kOpenPublic = 4, kPrivateSlots = 10, kPrivateTaken = 11;
    // The words a listing's bytes hold, in their order.
    constexpr size_t kCarried[] = { 5, 6, 7, 8, 9, 12, 13, 14 };
    static_assert(kSessionIdSize + kKeySize + 4 * std::size(kCarried) <= online::kSessionInfoSize);
    // The number a listing has its game type under, after the words a query names.
    constexpr uint32_t kGameTypeNumber = uint32_t(kMapPacks) + 1;
    constexpr size_t kMostFound = 50;   // the results the title has room for (82805000)

    struct MatchSession
    {
        uint8_t address[kAddressSize]{};
        uint32_t gameType = 0, maxPlayers = 0, numPlayers = 0;
        uint32_t words[kWords]{};
        uint8_t key[kKeySize]{}, id[kKeySize]{};   // the id is 8 bytes of the 16 the title sends

        // The title's part: six signed words, two unsigned, two floats, two
        // signed, the key, the id and three unsigned.
        bool ReadTitlePart(Reader& in)
        {
            for (size_t i = 0; i < kWords - 3; i++)
                words[i] = i == 6 || i == 7 ? in.UInt32() : i == 8 || i == 9 ? in.Float32Bits() : in.Int32Bits();
            const Bytes keyBytes = in.Blob(kKeySize), idBytes = in.Blob(kKeySize);
            for (size_t i = kWords - 3; i < kWords; i++) words[i] = in.UInt32();
            if (!in.Ok() || keyBytes.size() != kKeySize || idBytes.size() != kKeySize) return false;
            std::memcpy(key, keyBytes.data(), kKeySize);
            std::memcpy(id, idBytes.data(), kKeySize);
            return true;
        }
        // (address, game type, slots, the title's part), as tasks 1 and 2 send it.
        bool Read(Reader& in)
        {
            const Bytes addressBytes = in.Blob(kAddressSize);
            gameType = in.UInt32();
            maxPlayers = in.UInt32();
            const Bytes part = in.Blob(kLargestMessage);
            if (!in.Ok() || addressBytes.size() != kAddressSize) return false;
            std::memcpy(address, addressBytes.data(), kAddressSize);
            Reader inner(part.data(), part.size());
            return ReadTitlePart(inner);
        }
        // As a result of a search (bdMatchMakingInfo, 821AEC10, then 827A9810).
        void Write(Writer& to) const
        {
            to.Blob(Bytes(address, address + kAddressSize));
            to.Blob(Bytes(id, id + kSessionIdSize));
            to.UInt32(gameType);
            to.UInt32(maxPlayers);
            to.UInt32(numPlayers);
            for (size_t i = 0; i < kWords - 3; i++)
            {
                if (i == 6 || i == 7) to.UInt32(words[i]);
                else if (i == 8 || i == 9) to.Float32Bits(words[i]);
                else to.Int32Bits(words[i]);
            }
            to.Blob(Bytes(key, key + kKeySize));
            to.Blob(Bytes(id, id + kKeySize));
            for (size_t i = kWords - 3; i < kWords; i++) to.UInt32(words[i]);
        }
    };

    // The session this copy lists, and what its last search asked for.
    std::mutex g_matchLock;
    bool g_listing = false;
    MatchSession g_listed, g_sought;

    void ListSession(const MatchSession* session)
    {
        auto* service = online::Get();
        if (!service || !service->Matchmaking()) return;
        if (!session) { service->List(nullptr); return; }
        online::SessionListing listing;
        std::memcpy(listing.session, session->id, kSessionIdSize);
        std::memcpy(listing.session + kSessionIdSize, session->key, kKeySize);
        uint8_t* at = listing.session + kSessionIdSize + kKeySize;
        for (const size_t word : kCarried)
            for (int shift = 0; shift < 32; shift += 8) *at++ = uint8_t(session->words[word] >> shift);
        const uint32_t open = session->words[kOpenPublic], taken = session->words[kPrivateTaken];
        listing.openPublic = open;
        listing.filledPublic = session->maxPlayers - std::min(session->maxPlayers, open);
        listing.openPrivate = session->words[kPrivateSlots] - std::min(session->words[kPrivateSlots], taken);
        listing.filledPrivate = taken;
        for (size_t i = kPlaylist; i <= kMapPacks; i++) listing.numbers.push_back({ uint32_t(i), session->words[i] });
        listing.numbers.push_back({ kGameTypeNumber, session->gameType });
        service->List(&listing);
    }

    // Tasks 1 and 2: (context, [session id,] session). The result of the
    // first is the session's id, which the second names: the console's.
    void ListMatch(bool created, Reader& in, Answer& answer)
    {
        in.String(kNameSize);
        if (!created) in.Blob(kSessionIdSize);
        MatchSession session;
        if (!session.Read(in)) { answer.error = kParamParseError; return; }
        {
            std::lock_guard g(g_matchLock);
            g_listed = session;
            g_listing = true;
        }
        ListSession(&session);
        if (!created) return;
        answer.objects.Blob(Bytes(session.id, session.id + kSessionIdSize));
        answer.results = 1;
    }

    void UnlistMatch()
    {
        {
            std::lock_guard g(g_matchLock);
            g_listing = false;
        }
        ListSession(nullptr);
    }

    // Task 16: (context, query, a spare, the most results, then the query's
    // numbers: the playlist, its rule, the protocol, the map packs, the
    // slots needed, and what the publisher's servers weighed by). The
    // sessions wanted are of this copy's own game type, the multiplayer's or
    // Exo Survival's, which its own session says.
    void FindMatches(Reader& in, Answer& answer)
    {
        in.String(kNameSize);
        in.UInt32();
        in.UInt32();
        const uint32_t most = in.UInt32();
        MatchSession sought;
        for (size_t i = kPlaylist; i <= kOpenPublic; i++) sought.words[i] = in.Int32Bits();
        auto* service = online::Get();
        if (!in.Ok()) { answer.error = kParamParseError; return; }
        if (!service || !service->Matchmaking()) return;
        online::SessionSearch search;
        search.room = sought.words[kOpenPublic];
        search.most = uint32_t(std::min(size_t(most), kMostFound));
        for (size_t i = kPlaylist; i <= kMapPacks; i++) search.wants.push_back({ uint32_t(i), sought.words[i], false });
        {
            std::lock_guard g(g_matchLock);
            if (g_listing)
            {
                sought.gameType = g_listed.gameType;
                search.wants.push_back({ kGameTypeNumber, sought.gameType, false });
            }
            g_sought = sought;
        }
        kernel::ForgetSessionLags();
        answer.search = service->Search(search);
    }

    // The sessions a search found, as the title's results; false while the
    // service has no answer yet.
    bool FinishSearch(Answer& answer)
    {
        auto* service = online::Get();
        std::vector<online::SessionFound> found;
        if (service && !service->SearchResults(answer.search, found)) return false;
        MatchSession sought;
        {
            std::lock_guard g(g_matchLock);
            sought = g_sought;
        }
        for (const auto& one : found)
        {
            MatchSession session = sought;
            kernel::TitleXnAddr(session.address);
            kernel::SetXnAddrPeer(session.address, one.host);
            std::memcpy(session.id, one.session, kSessionIdSize);
            std::memcpy(session.key, one.session + kSessionIdSize, kKeySize);
            const uint8_t* at = one.session + kSessionIdSize + kKeySize;
            for (const size_t word : kCarried)
            {
                session.words[word] = Little32(at);
                at += 4;
            }
            session.words[kOpenPublic] = one.openPublic;
            session.maxPlayers = one.openPublic + one.filledPublic;
            session.numPlayers = one.filledPublic;
            session.words[kPrivateSlots] = one.openPrivate + one.filledPrivate;
            session.words[kPrivateTaken] = one.filledPrivate;
            session.Write(answer.objects);
            answer.results++;
            kernel::NoteSessionLag(one.session, one.lag);
        }
        LOGI("dw: the search found %u sessions", answer.results);
        return true;
    }

    void MatchMaking(uint8_t task, Reader& in, Answer& answer)
    {
        switch (task)
        {
        case 1: ListMatch(true, in, answer); break;
        case 2: ListMatch(false, in, answer); break;
        case 3: UnlistMatch(); break;
        case 16: FindMatches(in, answer); break;
        default: break;
        }
    }
#else
    bool FinishSearch(Answer&) { return true; }
#endif

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
        case 21: return "matchmaking (the older)";
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
        case 80: return "marketplace";
        case 138: return "matchmaking";
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
#if defined(AW_TITLE_MP)
        case 138: MatchMaking(task, in, answer); break;
#endif
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
            Answered();
            const size_t got = std::min(room, out_.size());
            std::memcpy(out, out_.data(), got);
            out_.erase(out_.begin(), out_.begin() + ptrdiff_t(got));
            return got;
        }

        bool Readable() override
        {
            std::lock_guard g(lock_);
            Answered();
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
            Waiting& waiting = waiting_.emplace_back();
            waiting.service = service;
            waiting.task = task;
            waiting.parameters = in.Left();
            Serve(player_, service, task, in, waiting.answer);
            Answered();
        }

        // Replies to the tasks whose answers are there, in the order the
        // tasks came: the title takes them in that order, so one that waits
        // for the online service holds back those behind it.
        void Answered()
        {
            while (!waiting_.empty())
            {
                Waiting& waiting = waiting_.front();
                Answer& answer = waiting.answer;
                if (answer.search && !FinishSearch(answer)) return;
                Writer reply;
                reply.Byte(kTaskReply);
                reply.UInt64(++transaction_);
                reply.UInt32(answer.error);
                if (answer.error == kNoError)
                {
                    reply.UByte8(waiting.task);
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
                         ServiceName(waiting.service), waiting.service, waiting.task, answer.note.empty() ? "" : " ",
                         answer.note.c_str(), waiting.parameters, answer.error, answer.results,
                         reply.Written().size());
                }
                waiting_.pop_front();
            }
        }

        void Reply(const Writer& payload)
        {
            const uint32_t size = uint32_t(payload.Written().size() + 1);
            for (int i = 0; i < 4; i++) out_.push_back(uint8_t(size >> (8 * i)));
            out_.push_back(0);   // not encrypted
            out_.insert(out_.end(), payload.Written().begin(), payload.Written().end());
        }

        // A task that came, and its answer.
        struct Waiting
        {
            uint8_t service = 0, task = 0;
            size_t parameters = 0;
            Answer answer;
        };

        const Player player_;
        std::mutex lock_;
        Bytes in_, out_;
        std::deque<Waiting> waiting_;
        bool greeted_ = false, closed_ = false;
        uint64_t transaction_ = 0;
    };
}

#if defined(AW_TITLE_MP)
namespace
{
    // ---- the auth server ---------------------------------------------------
    //
    // Advanced Warfare logs on over HTTPS first (bdAuthXenon, 821C6310): a
    // JSON object with the task (0x2A), a seed, the DemonWare title id and
    // the console's proof of who the player is. The reply (821C6718) is the
    // task plus one, the code 700, the seed, two tickets in base64 and, in a
    // JSON text of its own, the player's DemonWare id.
    //
    // The client ticket is 128 bytes (bdAuthTicket, 821D9FB0): its magic
    // number, a type, the title id, when it was issued and when it runs out,
    // a licence, the player's id and name, and the key the lobby connection
    // is encrypted with. The server encrypts it with a key the console's
    // proof gives both sides; the title takes one whose magic number reads
    // as it is without decrypting it (821C6A2C), and that is what it gets.
    constexpr uint32_t kAuthTask = 0x2A, kAuthorised = 700, kTicketMagic = 0xEFBDADDE;
    constexpr size_t kTicketSize = 128, kTicketName = 64, kSessionKeySize = 24;

    std::string Base64(const uint8_t* bytes, size_t size)
    {
        static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string text;
        for (size_t at = 0; at < size; at += 3)
        {
            const size_t left = size - at;
            const uint32_t three = (uint32_t(bytes[at]) << 16) | (left > 1 ? uint32_t(bytes[at + 1]) << 8 : 0) |
                                   (left > 2 ? bytes[at + 2] : 0);
            text += kDigits[three >> 18];
            text += kDigits[(three >> 12) & 63];
            text += left > 1 ? kDigits[(three >> 6) & 63] : '=';
            text += left > 2 ? kDigits[three & 63] : '=';
        }
        return text;
    }

    // The number a JSON object of the title's has under a name; the title
    // writes its numbers bare (bdJSONSerializer, 82205DA0).
    bool JsonNumber(const std::string& json, const char* name, uint32_t& value)
    {
        const std::string key = std::string("\"") + name + "\"";
        size_t at = json.find(key);
        if (at == std::string::npos) return false;
        at = json.find_first_not_of(": \t\"", at + key.size());
        if (at == std::string::npos || json[at] < '0' || json[at] > '9') return false;
        value = uint32_t(std::strtoul(json.c_str() + at, nullptr, 10));
        return true;
    }

    std::string Authorise(const std::string& request, uint64_t xuid)
    {
        uint32_t task = 0, seed = 0, title = 0;
        if (!JsonNumber(request, "auth_task", task) || !JsonNumber(request, "iv_seed", seed) ||
            !JsonNumber(request, "title_id", title) || task != kAuthTask)
        {
            LOGW("dw: the title's logon is not one the auth server knows");
            return "{\"code\":701}";
        }
        uint8_t ticket[kTicketSize]{};
        size_t at = 0;
        const auto little = [&](uint64_t value, size_t size) {
            for (size_t i = 0; i < size; i++) ticket[at++] = uint8_t(value >> (8 * i));
        };
        const uint32_t now = uint32_t(std::time(nullptr));
        little(kTicketMagic, 4);
        little(0, 1);                    // the type: a user's
        little(title, 4);
        little(now, 4);
        little(now + 24 * 60 * 60, 4);
        little(0, 8);                    // the licence
        little(xuid, 8);
        if (auto* service = online::Get())
            std::snprintf(reinterpret_cast<char*>(ticket + at), kTicketName, "%s", service->LocalName().c_str());
        at += kTicketName + kSessionKeySize;   // the key: zeros, for a connection that is not encrypted
        const uint8_t serverTicket[kTicketSize]{};

        char reply[1024];
        std::snprintf(reply, sizeof reply,
                      "{\"auth_task\":%u,\"code\":%u,\"iv_seed\":%u,\"client_ticket\":\"%s\","
                      "\"server_ticket\":\"%s\",\"extra_data\":\"{\\\"dpi\\\":\\\"%llu\\\"}\"}",
                      task + 1, kAuthorised, seed, Base64(ticket, sizeof ticket).c_str(),
                      Base64(serverTicket, sizeof serverTicket).c_str(), static_cast<unsigned long long>(xuid));
        LOGI("dw: the title logged on to the auth server");
        return reply;
    }
}
#endif

bool online::demonware::Http(const std::string& host, const std::string& path, const std::string& body,
                             uint64_t xuid, std::string& reply)
{
#if defined(AW_TITLE_MP)
    if (Served() && host.starts_with("aw-xbox360-auth3.") && path == "/auth/")
    {
        reply = Authorise(body, xuid);
        return true;
    }
#else
    (void)host; (void)path; (void)body; (void)xuid; (void)reply;
#endif
    return false;
}

bool online::demonware::Served()
{
#if defined(MW3_TITLE_MP) || defined(AW_TITLE_MP)
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

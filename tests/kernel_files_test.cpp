// Asset-free test of the kernel's file layer: how guest NT paths become host
// files. It compiles runtime/kernel/files.cpp unchanged, calls its NtCreateFile,
// NtReadFile and friends the way recompiled code would, and runs them against a
// temporary folder of synthetic files made here -- among them a disc image and
// a content package, built byte by byte. No game files are involved.
#include "tests/fake_guest.h"
#include "tests/fake_crash.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/volume.h"
#include "runtime/install/crypto.h"
#include "runtime/movie_trace.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

PPC_FUNC(__imp__NtCreateFile);
PPC_FUNC(__imp__NtOpenFile);
PPC_FUNC(__imp__NtReadFile);
PPC_FUNC(__imp__NtWriteFile);
PPC_FUNC(__imp__NtQueryInformationFile);
PPC_FUNC(__imp__NtQueryFullAttributesFile);
PPC_FUNC(__imp__NtSetInformationFile);

// ---- what the file sources link against ------------------------------------
static fs::path g_discRoot;
const fs::path& kernel::GameRoot() { return g_discRoot; }
void kernel::QueueUserApc(uint32_t, uint32_t, uint32_t, std::function<void()> work)
{
    if (work) work();   // not delivered later here: run it where it was queued
}
// The trace of a movie's reads (MW2_TRACE_MOVIES) is off here: its own
// source hooks the title's functions, which this test has none of.
#if MW2_DIAGNOSTICS
bool movies::On() { return false; }
double movies::Clock() { return 0.0; }
void movies::Read(const char*, uint32_t, long long, double) {}
#endif

using namespace kernel;

namespace
{
    constexpr uint32_t ACCESS_DENIED = 0xC0000022u;
    constexpr uint32_t GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u;
    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2,
                       FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3;
    constexpr uint32_t FILE_DIRECTORY_FILE = 1;

    // Fixed guest addresses for the structures a call passes by pointer.
    constexpr uint32_t kHandleOut = 0x3000, kAttributes = 0x3010, kAnsi = 0x3020,
                       kName = 0x3030, kIosb = 0x3200, kOffset = 0x3210, kInfo = 0x3300,
                       kBuffer = 0x10000, kStack = 0x8000;

    PPCContext Ctx()
    {
        PPCContext ctx;
        ctx.r1.u64 = kStack;
        return ctx;
    }

    void PutName(const std::string& name)
    {
        std::memcpy(guest::Base() + kName, name.c_str(), name.size() + 1);
        *GuestPtr<be16>(kAnsi) = uint16_t(name.size());
        *GuestPtr<be16>(kAnsi + 2) = uint16_t(name.size() + 1);
        *GuestPtr<be32>(kAnsi + 4) = kName;
        *GuestPtr<be32>(kAttributes) = 0;
        *GuestPtr<be32>(kAttributes + 4) = kAnsi;
        *GuestPtr<be32>(kAttributes + 8) = 0;
    }

    struct Opened { uint32_t status = 0, handle = 0, information = 0; };

    Opened Create(const std::string& name, uint32_t access, uint32_t disposition,
                  uint32_t options = 0)
    {
        PutName(name);
        *GuestPtr<be32>(kHandleOut) = 0;
        PPCContext ctx = Ctx();
        *GuestPtr<be32>(kStack + 0x14 + 8 * 8) = options;   // the ninth argument
        ctx.r3.u64 = kHandleOut;
        ctx.r4.u64 = access;
        ctx.r5.u64 = kAttributes;
        ctx.r6.u64 = kIosb;
        ctx.r10.u64 = disposition;
        __imp__NtCreateFile(ctx, guest::Base());
        return { uint32_t(ctx.r3.u64), *GuestPtr<be32>(kHandleOut), *GuestPtr<be32>(kIosb + 4) };
    }

    uint32_t Open(const std::string& name, uint32_t access = GENERIC_READ)
    {
        PutName(name);
        PPCContext ctx = Ctx();
        ctx.r3.u64 = kHandleOut;
        ctx.r4.u64 = access;
        ctx.r5.u64 = kAttributes;
        ctx.r6.u64 = kIosb;
        __imp__NtOpenFile(ctx, guest::Base());
        return uint32_t(ctx.r3.u64);
    }

    struct Transfer { uint32_t status = 0, information = 0; };

    Transfer Read(uint32_t handle, uint32_t length, int64_t offset = -1)
    {
        PPCContext ctx = Ctx();
        if (offset >= 0) *GuestPtr<be64>(kOffset) = uint64_t(offset);
        ctx.r3.u64 = handle;
        ctx.r7.u64 = kIosb;
        ctx.r8.u64 = kBuffer;
        ctx.r9.u64 = length;
        ctx.r10.u64 = offset >= 0 ? kOffset : 0;
        __imp__NtReadFile(ctx, guest::Base());
        return { uint32_t(ctx.r3.u64), *GuestPtr<be32>(kIosb + 4) };
    }

    Transfer Write(uint32_t handle, const std::string& data, int64_t offset = -1)
    {
        PPCContext ctx = Ctx();
        std::memcpy(guest::Base() + kBuffer, data.data(), data.size());
        if (offset >= 0) *GuestPtr<be64>(kOffset) = uint64_t(offset);
        ctx.r3.u64 = handle;
        ctx.r7.u64 = kIosb;
        ctx.r8.u64 = kBuffer;
        ctx.r9.u64 = data.size();
        ctx.r10.u64 = offset >= 0 ? kOffset : 0;
        __imp__NtWriteFile(ctx, guest::Base());
        return { uint32_t(ctx.r3.u64), *GuestPtr<be32>(kIosb + 4) };
    }

    uint32_t QueryInfo(uint32_t handle, uint32_t infoClass)
    {
        PPCContext ctx = Ctx();
        ctx.r3.u64 = handle;
        ctx.r4.u64 = kIosb;
        ctx.r5.u64 = kInfo;
        ctx.r7.u64 = infoClass;
        __imp__NtQueryInformationFile(ctx, guest::Base());
        return uint32_t(ctx.r3.u64);
    }

    void SetInfo(uint32_t handle, uint32_t infoClass, uint64_t value)
    {
        PPCContext ctx = Ctx();
        *GuestPtr<be64>(kInfo) = value;
        ctx.r3.u64 = handle;
        ctx.r4.u64 = kIosb;
        ctx.r5.u64 = kInfo;
        ctx.r7.u64 = infoClass;
        __imp__NtSetInformationFile(ctx, guest::Base());
    }

    uint32_t FullAttributes(const std::string& name)
    {
        PutName(name);
        PPCContext ctx = Ctx();
        ctx.r3.u64 = kAttributes;
        ctx.r4.u64 = kInfo;
        __imp__NtQueryFullAttributesFile(ctx, guest::Base());
        return uint32_t(ctx.r3.u64);
    }

    std::string Bytes(uint32_t count)
    {
        return std::string(reinterpret_cast<const char*>(guest::Base() + kBuffer), count);
    }

    std::string Slurp(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    std::string Pattern(size_t n)
    {
        std::string s(n, '\0');
        for (size_t i = 0; i < n; i++) s[i] = char((i * 7 + 3) & 0xFF);
        return s;
    }
}

int main()
{
    // A disc and a save area made of synthetic files, outside the repository.
    const fs::path base = fs::temp_directory_path() / ("mw3-files-test-" + std::to_string(getpid()));
    fs::remove_all(base);
    g_discRoot = base / "disc";
    const fs::path saves = base / "saves";
    fs::create_directories(g_discRoot / "Zone" / "Sub");
    fs::create_directories(saves);
    const std::string data = Pattern(1000);
    { std::ofstream(g_discRoot / "Zone" / "Sub" / "Data.bin", std::ios::binary) << data; }
    { std::ofstream(g_discRoot / "empty.bin", std::ios::binary); }
    { std::ofstream(base / "outside.txt") << "outside the disc"; }

    // ---- paths: case, separators, devices ------------------------------------
    for (const char* name : { "D:\\zone\\sub\\data.bin", "D:\\ZONE\\SUB\\DATA.BIN",
                              "D:\\\\zone\\\\sub\\\\data.bin", "D:\\zone\\.\\sub\\data.bin",
                              "update:\\zone\\sub\\data.bin", "game:\\zone/sub/data.bin",
                              "\\Device\\Cdrom0\\zone\\sub\\data.bin" })
    {
        Opened o = Create(name, GENERIC_READ, FILE_OPEN);
        assert(o.status == X_STATUS_SUCCESS && o.handle && o.information == FILE_OPENED);
        assert(CloseHandle(o.handle));
    }

    // ---- a name that is not there: the requests the title has been seen to make
    for (const char* name : { "D:\\patch.ff", "update:\\patch.ff", "D:\\patch.ffm",
                              "D:\\imagefile4.pak", "D:\\imagefile4.pakm", "D:\\zone\\nothing.bin" })
    {
        Opened o = Create(name, GENERIC_READ, FILE_OPEN);
        assert(o.status == X_STATUS_OBJECT_NAME_NOT_FOUND && o.handle == 0 && o.information == 0);
        assert(Open(name) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    }
    assert(Create("D:\\", GENERIC_READ, FILE_OPEN).status == X_STATUS_OBJECT_NAME_NOT_FOUND);

    // ---- the level asked for: the name its loading screen goes by ------------
    assert(kernel::LevelAsked().empty());
    for (const auto& [name, level] : std::initializer_list<std::pair<const char*, const char*>>{
             { "D:\\mp_dome_load.ff", "mp_dome" },
             { "D:\\common.ff", "mp_dome" },
             { "D:\\video\\Trainer_load.bik", "trainer" },
             { "D:\\bog_a_load.ff", "bog_a" },
             { "D:\\mp_solar_load.ff", "mp_solar" },
             { "update:\\patch_mp_solar_load.ff", "mp_solar" },
             { "D:\\english\\eng_patch_mp_solar_load.ff", "mp_solar" },
             { "D:\\english\\eng_seoul_load.ff", "seoul" },
             { "D:\\_load.ff", "seoul" },
             { "D:\\seoul_load.txt", "seoul" },
             { "D:\\load.ff", "seoul" } })
    {
        Open(name);
        assert(kernel::LevelAsked() == level);
    }

    // An OBJECT_ATTRIBUTES with no name in it is a different bug, and also not a file.
    PutName("D:\\zone\\sub\\data.bin");
    *GuestPtr<be32>(kAttributes + 4) = 0;
    {
        PPCContext ctx = Ctx();
        ctx.r3.u64 = kHandleOut; ctx.r4.u64 = GENERIC_READ; ctx.r5.u64 = kAttributes; ctx.r6.u64 = kIosb;
        ctx.r10.u64 = FILE_OPEN;
        __imp__NtCreateFile(ctx, guest::Base());
        assert(uint32_t(ctx.r3.u64) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    }

    // ---- no path climbs out of its device -------------------------------------
    for (const char* name : { "D:\\..\\outside.txt", "D:\\zone\\..\\..\\outside.txt",
                              "D:\\..\\..\\..\\outside.txt", "D:/../outside.txt" })
        assert(Create(name, GENERIC_READ, FILE_OPEN).status == X_STATUS_OBJECT_NAME_NOT_FOUND);

    // ---- reading ---------------------------------------------------------------
    {
        Opened o = Create("D:\\zone\\sub\\data.bin", GENERIC_READ, FILE_OPEN);
        Transfer t = Read(o.handle, 16, 100);
        assert(t.status == X_STATUS_SUCCESS && t.information == 16);
        assert(Bytes(16) == data.substr(100, 16));

        t = Read(o.handle, 16);                       // no offset: carries on from there
        assert(t.status == X_STATUS_SUCCESS && Bytes(16) == data.substr(116, 16));

        t = Read(o.handle, 50, 990);                  // a short read at the end
        assert(t.status == X_STATUS_SUCCESS && t.information == 10);
        assert(Bytes(10) == data.substr(990));

        t = Read(o.handle, 16, 1000);                 // at the end: nothing, and EOF
        assert(t.status == X_STATUS_END_OF_FILE && t.information == 0);

        assert(QueryInfo(o.handle, 5) == X_STATUS_SUCCESS);          // FileStandardInformation
        assert(uint64_t(*GuestPtr<be64>(kInfo + 8)) == 1000);
        assert(QueryInfo(o.handle, 34) == X_STATUS_SUCCESS);         // FileNetworkOpenInformation
        assert(uint64_t(*GuestPtr<be64>(kInfo + 32)) == 1000);

        SetInfo(o.handle, 14, 500);                    // FilePositionInformation
        assert(Read(o.handle, 4).status == X_STATUS_SUCCESS && Bytes(4) == data.substr(500, 4));
        assert(QueryInfo(o.handle, 14) == X_STATUS_SUCCESS);
        assert(uint64_t(*GuestPtr<be64>(kInfo)) == 504);
        CloseHandle(o.handle);

        Opened e = Create("D:\\empty.bin", GENERIC_READ, FILE_OPEN);
        assert(Read(e.handle, 8).status == X_STATUS_END_OF_FILE);
        CloseHandle(e.handle);

        assert(Read(0xDEADBEEF, 4).status == X_STATUS_INVALID_PARAMETER);
    }

    // ---- the disc is read-only --------------------------------------------------
    {
        const fs::path onDisc = g_discRoot / "Zone" / "Sub" / "Data.bin";
        Opened o = Create("D:\\zone\\sub\\data.bin", GENERIC_READ | GENERIC_WRITE, FILE_OPEN);
        assert(o.status == X_STATUS_SUCCESS);
        Transfer t = Write(o.handle, "scribble", 0);   // accepted and discarded
        assert(t.status == X_STATUS_SUCCESS && t.information == 8);
        CloseHandle(o.handle);
        assert(Slurp(onDisc) == data);

        o = Create("D:\\zone\\sub\\data.bin", GENERIC_WRITE, FILE_OVERWRITE_IF);   // must not truncate
        assert(o.status == X_STATUS_SUCCESS);
        CloseHandle(o.handle);
        o = Create("D:\\zone\\sub\\data.bin", GENERIC_WRITE, FILE_SUPERSEDE);
        CloseHandle(o.handle);
        assert(Slurp(onDisc) == data);

        assert(Create("D:\\new.bin", GENERIC_WRITE, FILE_OPEN_IF).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
        assert(Create("D:\\new.bin", GENERIC_WRITE, FILE_CREATE).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
        assert(!fs::exists(g_discRoot / "new.bin"));
        assert(Create("D:\\zone\\sub\\data.bin", GENERIC_READ, FILE_CREATE).status == X_STATUS_OBJECT_NAME_COLLISION);
    }

    // ---- directories ------------------------------------------------------------
    {
        Opened d = Create("D:\\zone", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE);
        assert(d.status == X_STATUS_SUCCESS && d.information == FILE_OPENED);
        assert(QueryInfo(d.handle, 5) == X_STATUS_SUCCESS);
        assert(uint32_t(*GuestPtr<be32>(kInfo + 20)) == 0x01000000u);   // the directory flag
        CloseHandle(d.handle);
        assert(FullAttributes("D:\\zone") == X_STATUS_SUCCESS && uint32_t(*GuestPtr<be32>(kInfo + 48)) == 0x10);
    }

    // ---- full attributes -----------------------------------------------------------
    assert(FullAttributes("D:\\zone\\sub\\DATA.BIN") == X_STATUS_SUCCESS);
    assert(uint64_t(*GuestPtr<be64>(kInfo + 32)) == 1000 && uint64_t(*GuestPtr<be64>(kInfo + 40)) == 1000);
    assert(uint32_t(*GuestPtr<be32>(kInfo + 48)) == 0x80);
    assert(FullAttributes("D:\\patch.ff") == X_STATUS_OBJECT_NAME_NOT_FOUND);

    // ---- Harddisk0\Partition0, a read-only null device -------------------------------
    for (const char* name : { "\\Device\\Harddisk0\\Partition0", "\\device\\harddisk0\\PARTITION0" })
    {
        Opened o = Create(name, GENERIC_READ, FILE_OPEN);
        assert(o.status == X_STATUS_SUCCESS && o.handle && o.information == FILE_OPENED);
        Transfer t = Read(o.handle, 64);               // completes with nothing, not EOF
        assert(t.status == X_STATUS_SUCCESS && t.information == 0);
        t = Write(o.handle, "data");
        assert(t.status == ACCESS_DENIED && t.information == 0);
        CloseHandle(o.handle);
        assert(Create(name, GENERIC_WRITE, FILE_OPEN).status == ACCESS_DENIED);
        assert(Create(name, GENERIC_READ, FILE_OVERWRITE).status == ACCESS_DENIED);
        assert(Create(name, GENERIC_READ, FILE_SUPERSEDE).status == ACCESS_DENIED);
        assert(Create(name, GENERIC_READ, FILE_CREATE).status == X_STATUS_OBJECT_NAME_COLLISION);
        assert(FullAttributes(name) == X_STATUS_SUCCESS);
        assert(uint32_t(*GuestPtr<be32>(kInfo + 48)) == 0x80);
    }
    // Only that one object: a sibling is an ordinary missing file.
    assert(Create("\\Device\\Harddisk0\\Partition1", GENERIC_READ, FILE_OPEN).status ==
           X_STATUS_OBJECT_NAME_NOT_FOUND);

    // ---- a mounted device is writable ------------------------------------------------
    MountDevice("Cache0", saves);                      // names are not case sensitive
    {
        Opened o = Create("cache0:\\profile\\save.dat", GENERIC_READ | GENERIC_WRITE, FILE_OPEN_IF);
        assert(o.status == X_STATUS_SUCCESS && o.information == FILE_CREATED);
        assert(Write(o.handle, "hello").information == 5);
        assert(Write(o.handle, " world", 5).information == 6);
        assert(QueryInfo(o.handle, 5) == X_STATUS_SUCCESS);
        assert(uint64_t(*GuestPtr<be64>(kInfo + 8)) == 11);
        SetInfo(o.handle, 20, 5);                      // FileEndOfFileInformation
        CloseHandle(o.handle);
        assert(Slurp(saves / "profile" / "save.dat") == "hello");

        o = Create("CACHE0:\\Profile\\SAVE.DAT", GENERIC_READ, FILE_OPEN);   // found by any case
        assert(o.status == X_STATUS_SUCCESS && o.information == FILE_OPENED);
        assert(Read(o.handle, 16).information == 5 && Bytes(5) == "hello");
        CloseHandle(o.handle);

        o = Create("cache0:\\profile\\save.dat", GENERIC_WRITE, FILE_OVERWRITE_IF);
        assert(o.information == FILE_OVERWRITTEN);
        CloseHandle(o.handle);
        assert(Slurp(saves / "profile" / "save.dat").empty());

        // Marking a file for deletion removes it when the last handle goes.
        o = Create("cache0:\\profile\\temp.dat", GENERIC_WRITE, FILE_CREATE);
        assert(o.status == X_STATUS_SUCCESS);
        *GuestPtr<uint8_t>(kInfo) = 1;
        {
            PPCContext ctx = Ctx();
            ctx.r3.u64 = o.handle; ctx.r4.u64 = kIosb; ctx.r5.u64 = kInfo; ctx.r7.u64 = 13;
            __imp__NtSetInformationFile(ctx, guest::Base());
        }
        CloseHandle(o.handle);
        assert(!fs::exists(saves / "profile" / "temp.dat"));

        // A directory made under the mount.
        o = Create("cache0:\\profile\\slot1", GENERIC_READ, FILE_OPEN_IF, FILE_DIRECTORY_FILE);
        assert(o.status == X_STATUS_SUCCESS && o.information == FILE_CREATED);
        assert(fs::is_directory(saves / "profile" / "slot1"));
        CloseHandle(o.handle);

        // Nothing written through the mount lands outside it.
        o = Create("cache0:\\..\\..\\escape.txt", GENERIC_WRITE, FILE_CREATE);
        assert(o.status == X_STATUS_SUCCESS);
        CloseHandle(o.handle);
        assert(fs::exists(saves / "escape.txt"));
        assert(!fs::exists(base / "escape.txt") && !fs::exists(base.parent_path() / "escape.txt"));

        // Open-only dispositions still do not create.
        assert(Create("cache0:\\absent.dat", GENERIC_WRITE, FILE_OPEN).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
        assert(Create("cache0:\\absent.dat", GENERIC_WRITE, FILE_OVERWRITE).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
    }
    UnmountDevice("cache0");
    assert(Create("cache0:\\profile\\save.dat", GENERIC_READ, FILE_OPEN).status == X_STATUS_OBJECT_NAME_NOT_FOUND);

    // ---- a downloadable content package, read where it lies ------------------
    // A synthetic read-only STFS package: a marker, a folder, a file that runs
    // past the first hash table's 170 blocks, and two files whose blocks are
    // interleaved, so their chains have to be followed.
    {
        constexpr uint32_t kBlock = 0x1000, kPerTable = 0xAA;
        // Where the console puts a data block of a read-only package (after
        // Velocity's ComputeBackingDataBlockNumber), written out apart from
        // the runtime's own arithmetic.
        const auto blockAt = [](uint32_t block) -> uint64_t
        {
            uint64_t backing = (block + kPerTable) / kPerTable + block;
            if (block >= kPerTable) backing += (block + 0x70E4) / 0x70E4;
            return 0xB000 + backing * kBlock;
        };
        const std::string big = Pattern(0xB0 * kBlock - 100), odd = Pattern(2 * kBlock - 7), even(kBlock, 'e');
        constexpr uint32_t kBigFirst = 1, kOddA = 0xB1, kEven = 0xB2, kOddB = 0xB3, kBlocks = 0xB4;
        std::string package(size_t(blockAt(kBlocks)), '\0');
        const auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) package[at + i] = char(v >> (24 - 8 * i)); };
        const auto next = [&](uint32_t block, uint32_t after)
        {
            const size_t entry = size_t(blockAt(block - block % kPerTable) - kBlock) + (block % kPerTable) * 0x18;
            package[entry + 0x15] = char(after >> 16); package[entry + 0x16] = char(after >> 8); package[entry + 0x17] = char(after);
        };
        std::memcpy(package.data(), "LIVE", 4);
        put32(0x22C + 8, 0x00000001);                    // a licence, and its bits
        put32(0x340, 0xAD0E);                            // header size: a read-only package
        put32(0x344, 2);                                 // marketplace content
        put32(0x360, 0x41560817);
        package[0x37C] = 1;                             // one block of file table, block 0
        const char16_t title[] = u"Test Pack";
        for (size_t i = 0; title[i]; i++) package[0x411 + 2 * i + 1] = char(title[i]);
        const auto file = [&](size_t index, const char* name, uint8_t flags, uint32_t first, uint32_t size, uint16_t parent)
        {
            const size_t at = size_t(blockAt(0)) + index * 0x40;
            std::memcpy(&package[at], name, std::strlen(name));
            package[at + 0x28] = char(flags | std::strlen(name));
            package[at + 0x2F] = char(first); package[at + 0x30] = char(first >> 8);
            package[at + 0x32] = char(parent >> 8); package[at + 0x33] = char(parent);
            put32(at + 0x34, size);
        };
        file(0, "mappack1.id", 0, 0, 0, 0xFFFF);
        file(1, "Maps", 0x80, 0, 0, 0xFFFF);
        file(2, "Big.ff", 0x40, kBigFirst, uint32_t(big.size()), 1);
        file(3, "odd.bin", 0, kOddA, uint32_t(odd.size()), 0xFFFF);
        file(4, "even.bin", 0, kEven, uint32_t(even.size()), 0xFFFF);
        for (uint32_t i = 0; i < 0xB0; i++)
        {
            const size_t n = std::min<size_t>(kBlock, big.size() - size_t(i) * kBlock);
            std::memcpy(&package[size_t(blockAt(kBigFirst + i))], big.data() + size_t(i) * kBlock, n);
            if (i + 1 < 0xB0) next(kBigFirst + i, kBigFirst + i + 1);
        }
        std::memcpy(&package[size_t(blockAt(kOddA))], odd.data(), kBlock);
        std::memcpy(&package[size_t(blockAt(kOddB))], odd.data() + kBlock, odd.size() - kBlock);
        next(kOddA, kOddB);
        std::memcpy(&package[size_t(blockAt(kEven))], even.data(), kBlock);
        // What the console writes last: how many blocks there are, and the
        // SHA-1 of the header from the content type on.
        const auto seal = [&](std::string& bytes)
        {
            for (int i = 0; i < 4; i++) bytes[0x395 + i] = char(kBlocks >> (24 - 8 * i));
            const auto hash = install::crypto::Sha1(bytes.data() + 0x344, 0xB000 - 0x344);
            std::memcpy(&bytes[0x32C], hash.data(), hash.size());
        };
        seal(package);
        const fs::path packageFile = base / "0123456789ABCDEF0123456789ABCDEF01234567";
        { std::ofstream(packageFile, std::ios::binary) << package; }

        std::string error;
        const auto volume = ReadVolume::OpenPackage(packageFile, error);
        assert(volume && error.empty());
        assert(volume->Header().titleId == 0x41560817 && volume->Header().contentType == 2);
        assert(volume->Header().licenceBits == 1 && volume->Header().displayName == u"Test Pack");
        assert(volume->RootFiles().size() == 3);
        // What each folder holds, by the names the package has: the root's
        // files and its folder, the folder's one file, and nothing for a
        // folder that is not there.
        const auto* children = volume->Children("");
        assert(children && *children == (std::vector<std::string>{ "mappack1.id", "Maps", "odd.bin", "even.bin" }));
        children = volume->Children("MAPS\\");
        assert(children && *children == std::vector<std::string>{ "Big.ff" });
        assert(!volume->Children("sound"));
        MountVolume("mappack1", volume);

        Opened o = Create("mappack1:\\mappack1.id", GENERIC_READ, FILE_OPEN);
        assert(o.status == X_STATUS_SUCCESS && o.information == FILE_OPENED);
        assert(Read(o.handle, 16).status == X_STATUS_END_OF_FILE);
        CloseHandle(o.handle);
        assert(FullAttributes("mappack1:\\MAPS") == X_STATUS_SUCCESS && uint32_t(*GuestPtr<be32>(kInfo + 48)) == 0x10);
        assert(FullAttributes("mappack1:\\maps\\big.FF") == X_STATUS_SUCCESS);
        assert(uint64_t(*GuestPtr<be64>(kInfo + 40)) == big.size());

        // Across the first hash table, from the start, the middle and the end.
        o = Create("mappack1:\\maps\\big.ff", GENERIC_READ, FILE_OPEN);
        assert(o.status == X_STATUS_SUCCESS);
        for (const uint64_t at : { uint64_t(0), uint64_t(0xA8 * kBlock + 5), uint64_t(big.size() - 3000) })
        {
            const uint32_t want = uint32_t(std::min<uint64_t>(0x5000, big.size() - at));
            const Transfer t = Read(o.handle, 0x5000, int64_t(at));
            assert(t.status == X_STATUS_SUCCESS && t.information == want);
            assert(Bytes(want) == big.substr(size_t(at), want));
        }
        assert(Read(o.handle, 16, int64_t(big.size())).status == X_STATUS_END_OF_FILE);
        CloseHandle(o.handle);

        // A chain that is not consecutive, read in order without positions.
        o = Create("mappack1:\\odd.bin", GENERIC_READ, FILE_OPEN);
        assert(Read(o.handle, 5000).information == 5000 && Bytes(5000) == odd.substr(0, 5000));
        assert(Read(o.handle, 5000).information == odd.size() - 5000 && Bytes(uint32_t(odd.size() - 5000)) == odd.substr(5000));
        SetInfo(o.handle, 14, 4090);
        assert(Read(o.handle, 12).information == 12 && Bytes(12) == odd.substr(4090, 12));
        assert(QueryInfo(o.handle, 14) == X_STATUS_SUCCESS && uint64_t(*GuestPtr<be64>(kInfo)) == 4102);
        CloseHandle(o.handle);

        // Read-only: nothing is made in it, and a write is taken and dropped
        // as a write to the disc is.
        assert(Create("mappack1:\\new.bin", GENERIC_WRITE, FILE_OPEN_IF).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
        assert(Create("mappack1:\\even.bin", GENERIC_READ, FILE_CREATE).status == X_STATUS_OBJECT_NAME_COLLISION);
        o = Create("mappack1:\\even.bin", GENERIC_WRITE, FILE_OVERWRITE_IF);
        assert(o.status == X_STATUS_SUCCESS);
        assert(Write(o.handle, "scribble").status == X_STATUS_SUCCESS);
        assert(Read(o.handle, kBlock, 0).information == kBlock && Bytes(kBlock) == even);
        CloseHandle(o.handle);
        UnmountDevice("mappack1");
        assert(Create("mappack1:\\even.bin", GENERIC_READ, FILE_OPEN).status == X_STATUS_OBJECT_NAME_NOT_FOUND);

        // A folder's own files, by name: what a listing of the folder holds.
        assert(volume->FilesIn("maps") == std::vector<std::string>{ "Big.ff" });
        assert(volume->FilesIn("MAPS\\").size() == 1 && volume->FilesIn("nowhere").empty());
        // A folder whose files answer at the root as well (an update's
        // language folder), and never over a file the root has.
        MountVolume("update", volume, "Maps");
        assert(FullAttributes("update:\\big.ff") == X_STATUS_SUCCESS && uint64_t(*GuestPtr<be64>(kInfo + 40)) == big.size());
        assert(FullAttributes("update:\\maps\\big.ff") == X_STATUS_SUCCESS);
        assert(FullAttributes("update:\\odd.bin") == X_STATUS_SUCCESS && uint64_t(*GuestPtr<be64>(kInfo + 40)) == odd.size());
        UnmountDevice("update");
        assert(Slurp(packageFile) == package);

        // What is not such a package is refused, with the reason.
        { std::ofstream(base / "not-a-package", std::ios::binary) << Pattern(0x2000); }
        assert(!ReadVolume::OpenPackage(base / "not-a-package", error) && !error.empty());
        // A copy that stopped part-way says how much of it is there, by the
        // block count in its header and not by whichever file ends first.
        std::string cut = package.substr(0, size_t(blockAt(0x40)));
        { std::ofstream(base / "cut-short", std::ios::binary) << cut; }
        assert(!ReadVolume::OpenPackage(base / "cut-short", error) && error.find("is incomplete") == 0);
        { std::ofstream(base / "cut-in-header", std::ios::binary) << package.substr(0, 0x4000); }
        assert(!ReadVolume::OpenPackage(base / "cut-in-header", error) && error.find("is incomplete") == 0);
        // One whose header is not what it was hashed as is damaged: a byte of
        // its name, and a title id changed to pass for this game's.
        for (const size_t at : { size_t(0x420), size_t(0x363) })
        {
            std::string damaged = package;
            damaged[at] ^= 0x10;
            { std::ofstream(base / "damaged", std::ios::binary) << damaged; }
            assert(!ReadVolume::OpenPackage(base / "damaged", error) && error.find("is damaged") == 0);
        }
        // The newer of two copies of a pack is told by the version in the header.
        std::string newer = package;
        newer[0x35B] = 3;
        seal(newer);
        { std::ofstream(base / "newer", std::ios::binary) << newer; }
        assert(ReadVolume::OpenPackage(base / "newer", error)->Header().version == 3 && volume->Header().version == 0);

        // The same package lying inside another file, as Advanced Warfare's
        // lie in its install disc's image: read from its place there, by a
        // path and by a descriptor the caller holds open, and never past its
        // own end into what follows it.
        const size_t before = 0x3800;
        const fs::path holder = base / "holder.iso";
        { std::ofstream(holder, std::ios::binary) << Pattern(before) << package << Pattern(0x1000); }
        const int held = open(holder.string().c_str(), O_RDONLY);
        assert(held >= 0);
        for (const std::string& source : { holder.string(), "fd:" + std::to_string(held) })
        {
            const auto inside = ReadVolume::OpenPackage(source, before, package.size(), error);
            assert(inside && inside->Header().displayName == u"Test Pack" && inside->RootFiles().size() == 3);
            const ReadVolume::Entry* entry = inside->Find("maps\\big.ff");
            assert(entry && entry->size == big.size());
            std::string read(big.size() + 64, '\0');
            const int descriptor = inside->Acquire();
            assert(inside->Read(descriptor, *entry, 0, read.data(), read.size()) == big.size());
            inside->Release(descriptor);
            read.resize(big.size());
            assert(read == big);
        }
        close(held);
        // A range that is not a package, and one past the file's end.
        assert(!ReadVolume::OpenPackage(holder.string(), 0, before, error) && !error.empty());
        assert(!ReadVolume::OpenPackage(holder.string(), uint64_t(1) << 40, package.size(), error));
    }

    // ---- the disc image under the game folder --------------------------------
    // A synthetic XDVDFS image: two files at the root and one in a folder.
    {
        constexpr size_t kSector = 2048;
        const std::string common = Pattern(5000), top = Pattern(700), shadowed = Pattern(300);
        std::string image(64 * kSector, '\0');
        const auto le32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) image[at + i] = char(v >> (8 * i)); };
        // { left u16, right u16, sector u32, size u32, attributes u8, name length u8, name },
        // the next entry to the right of each.
        const auto entry = [&](size_t directory, size_t at, bool last, uint32_t sector, uint32_t size,
                               uint8_t attributes, const char* name)
        {
            const size_t length = std::strlen(name), end = (at + 14 + length + 3) & ~size_t(3);
            const size_t e = directory * kSector + at;
            image[e] = image[e + 1] = image[e + 3] = 0;
            image[e + 2] = char(last ? 0 : end / 4);
            le32(e + 4, sector); le32(e + 8, size);
            image[e + 12] = char(attributes); image[e + 13] = char(length);
            std::memcpy(&image[e + 14], name, length);
            return end;
        };
        std::memcpy(&image[32 * kSector], "MICROSOFT*XBOX*MEDIA", 20);
        le32(32 * kSector + 0x14, 33); le32(32 * kSector + 0x18, kSector);
        std::fill(image.begin() + 33 * kSector, image.begin() + 35 * kSector, char(0xFF));
        size_t at = entry(33, 0, false, 40, uint32_t(common.size()), 0x80, "Common.ff");
        at = entry(33, at, false, 34, kSector, 0x10, "highmip");
        entry(33, at, true, 44, uint32_t(shadowed.size()), 0x80, "empty.bin");
        entry(34, 0, true, 48, uint32_t(top.size()), 0x80, "Tex.hi");
        std::memcpy(&image[40 * kSector], common.data(), common.size());
        std::memcpy(&image[44 * kSector], shadowed.data(), shadowed.size());
        std::memcpy(&image[48 * kSector], top.data(), top.size());
        const fs::path imageFile = base / "disc.iso";
        { std::ofstream(imageFile, std::ios::binary) << image; }

        // Not there until the image is mounted; a folder is not an image.
        assert(Open("D:\\common.ff") == X_STATUS_OBJECT_NAME_NOT_FOUND);
        std::string error;
        assert(!MountDisc(g_discRoot.string(), error) && !Disc());
        assert(!MountDisc((base / "outside.txt").string(), error) && !error.empty());

        // By its path, and by a descriptor the caller holds open.
        const int held = open(imageFile.c_str(), O_RDONLY);
        assert(held >= 0);
        for (const std::string& source : { imageFile.string(), "fd:" + std::to_string(held) })
        {
            assert(MountDisc(source, error) && Disc());
            assert(Disc()->RootFiles().size() == 2);
            Opened o = Create("game:\\COMMON.FF", GENERIC_READ, FILE_OPEN);
            assert(o.status == X_STATUS_SUCCESS && o.information == FILE_OPENED);
            assert(QueryInfo(o.handle, 5) == X_STATUS_SUCCESS && uint64_t(*GuestPtr<be64>(kInfo + 8)) == common.size());
            assert(Read(o.handle, 3000).information == 3000 && Bytes(3000) == common.substr(0, 3000));
            assert(Read(o.handle, 3000).information == 2000 && Bytes(2000) == common.substr(3000));
            assert(Read(o.handle, 16).status == X_STATUS_END_OF_FILE);
            assert(Read(o.handle, 100, 4950).information == 50 && Bytes(50) == common.substr(4950));
            // A second handle on the same file keeps its own place.
            const Opened other = Create("D:\\common.ff", GENERIC_READ, FILE_OPEN);
            assert(Read(other.handle, 10).information == 10 && Bytes(10) == common.substr(0, 10));
            assert(Read(o.handle, 10, 100).information == 10);
            assert(Read(other.handle, 10).information == 10 && Bytes(10) == common.substr(10, 10));
            CloseHandle(other.handle);
            CloseHandle(o.handle);

            o = Create("D:\\highmip\\tex.hi", GENERIC_READ, FILE_OPEN);
            assert(o.status == X_STATUS_SUCCESS);
            assert(Read(o.handle, 4096).information == top.size() && Bytes(uint32_t(top.size())) == top);
            CloseHandle(o.handle);
            assert(FullAttributes("D:\\HighMip") == X_STATUS_SUCCESS && uint32_t(*GuestPtr<be32>(kInfo + 48)) == 0x10);
            assert(FullAttributes("D:\\highmip\\tex.hi") == X_STATUS_SUCCESS);
            assert(uint64_t(*GuestPtr<be64>(kInfo + 40)) == top.size());

            // The game folder's file comes before the image's of the same name.
            assert(FullAttributes("D:\\empty.bin") == X_STATUS_SUCCESS && uint64_t(*GuestPtr<be64>(kInfo + 40)) == 0);
            o = Create("D:\\zone\\sub\\data.bin", GENERIC_READ, FILE_OPEN);
            assert(o.status == X_STATUS_SUCCESS && Read(o.handle, 1000).information == 1000 && Bytes(1000) == data);
            CloseHandle(o.handle);

            // Still read-only, and what neither has is still not there.
            assert(Create("D:\\common.ff", GENERIC_READ, FILE_CREATE).status == X_STATUS_OBJECT_NAME_COLLISION);
            o = Create("D:\\common.ff", GENERIC_WRITE, FILE_OVERWRITE_IF);
            assert(o.status == X_STATUS_SUCCESS && Write(o.handle, "scribble").status == X_STATUS_SUCCESS);
            CloseHandle(o.handle);
            assert(Open("D:\\absent.ff") == X_STATUS_OBJECT_NAME_NOT_FOUND);
            assert(Open("D:\\highmip\\absent.hi") == X_STATUS_OBJECT_NAME_NOT_FOUND);
            assert(Open("D:\\..\\disc.iso") == X_STATUS_OBJECT_NAME_NOT_FOUND);
        }
        close(held);
        assert(Slurp(imageFile) == image);
    }

    fs::remove_all(base);
    std::puts("kernel-files-test ok");
    return 0;
}

// The mods folder: game scripts and raw files of the player's own, laid over
// the ones in the fastfiles (docs/mods.md).
//
//   <game>/mods/mods.txt          the mods that are on, one folder name a line
//   <game>/mods/<name>/...        a file here stands for the asset of its path:
//       maps/mp/gametypes/war.gsc     replaces that script
//       scripts/hello.gsc             a script of the mod's own; its main() and
//                                     init() run as each level starts
//       mp/basemaps.arena             replaces that raw file
//
// The title asks its asset database for a script (asset type 0x25) as it
// loads one, and for a raw file (0x24) when it reads one; DB_FindXAssetHeader
// is hooked, and for a path a mod has it answers with an asset made here. A
// script is compiled first (mods_gsc.cpp): this build of the game has no
// compiler and only ever sees compiled scripts. Nothing is picked up unasked:
// a folder not named in mods.txt is not read.
//
// The idea of a mods folder over the fastfiles' scripts is CoD Xe's
// (michaeloliverx/codxe, a plugin for the console); the code is this port's.
#if MW2_GSC

#include <ppc_recomp_shared.h>
#include "title.h"

#ifdef T_Scr_LoadScript

#include "mods.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "kernel/kernel.h"
#include "mw3_dlc_maps.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kRawFile = 0x24, kScriptFile = 0x25;
    // A script's name goes through a 64-byte buffer in the title's loader ("%s.gsc").
    constexpr size_t kLongestScriptName = 59;
    // What is set aside of the script pool's 2 MB for the mods' code. A
    // Team Deathmatch on a disc map leaves 1260 KB of it unused.
    constexpr uint32_t kCodeRoom = 384 * 1024;
#ifdef MW3_TITLE_MP
    constexpr const char* kOwnEntries = "scripts/mp/";
#else
    constexpr const char* kOwnEntries = "scripts/sp/";
#endif

    struct ModFile
    {
        fs::path path;
        std::string mod;
    };

    // An asset made for the title, in guest memory. A raw file is { name,
    // compressedLen, len, buffer }; a script file { name, compressedLen, len,
    // bytecodeLen, buffer, bytecode }, the buffer in both deflated (the
    // title's reader always inflates).
    struct Asset
    {
        uint32_t address = 0, room = 0;
        std::vector<uint8_t> bytecode;      // as compiled: the loader rewrites the guest's copy
        fs::path source;
        fs::file_time_type stamp{};
        uint32_t load = 0;                  // the level load it was last made ready for
        bool served = false;
    };

    struct Entry
    {
        std::string name;
        uint32_t main = 0, init = 0;
    };

    // Recursive: compiling a script asks for the scripts it includes.
    std::recursive_mutex g_lock;
    std::atomic<bool> g_any{ false };       // a mod is on and has files
    bool g_scanned = false;
    std::map<std::string, ModFile> g_files; // lower-case path below a mod's folder, '/' between -> the first mod's file
    std::unordered_map<std::string, Asset> g_assets;
    std::vector<Entry> g_entries;
    uint32_t g_load = 1;
    uint32_t g_scratch = 0;                 // a guest buffer for a name handed to the title
    // The room in the script pool the mods' code has, as positions in the
    // pool: [start, end), used up to `next` by the level that is loaded.
    uint32_t g_arenaStart = 0, g_arenaEnd = 0, g_arenaNext = 0;

    // A path as text whatever is in it (path::string() throws on Windows for
    // a name outside the code page).
    std::string Utf8(const fs::path& path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = c == '\\' ? '/' : char(std::tolower(uint8_t(c)));
        return text;
    }

    bool EndsWith(const std::string& text, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
    }

    fs::path Root()
    {
        // MW2_MOD_DIR for a mods folder that is not in the game's (the same
        // folder kernel/files.cpp lays over the game's read-only devices).
        if (const char* named = env::Text("MW2_MOD_DIR")) return fs::path(named);
        return kernel::GameRoot() / "mods";
    }

    bool ReadFile(const fs::path& path, std::vector<uint8_t>& data)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    // The mods mods.txt names, in its order: an earlier mod's file wins.
    void Scan()
    {
        g_scanned = true;
        g_files.clear();
        const fs::path root = Root();
        std::ifstream list(root / "mods.txt");
        std::string line;
        while (list && std::getline(list, line))
        {
            const size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == '#') continue;
            const std::string name = line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
            if (name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
            {
                LOGW("mods: \"%s\" in mods.txt is not a folder's name", name.c_str());
                continue;
            }
            const fs::path folder = root / name;
            std::error_code ec;
            if (!fs::is_directory(folder, ec))
            {
                LOGW("mods: mods.txt names \"%s\", and there is no such folder in %s", name.c_str(), Utf8(root).c_str());
                continue;
            }
            size_t count = 0;
            for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec)) continue;
                const std::string relative = Lower(Utf8(fs::relative(it->path(), folder, ec)));
                const auto [at, added] = g_files.try_emplace(relative, ModFile{ it->path(), name });
                if (added) count++;
                else LOGI("mods: %s/%s is not used: %s, listed first, has that file", name.c_str(), relative.c_str(),
                          at->second.mod.c_str());
            }
            LOGI("mods: %s is on, %zu file(s)", name.c_str(), count);
        }
        g_any.store(!g_files.empty(), std::memory_order_release);
    }

    void ScanOnce()
    {
        if (!g_scanned) Scan();
    }

    // A call of the title's, on a frame of its own below the hook's.
    uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* function, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                  uint32_t d = 0)
    {
        const PPCContext saved = ctx;
        ctx.r1.u64 = saved.r1.u32 - 0x100;
        ctx.r3.u64 = a;
        ctx.r4.u64 = b;
        ctx.r5.u64 = c;
        ctx.r6.u64 = d;
        function(ctx, base);
        const uint32_t result = ctx.r3.u32;
        ctx = saved;
        return result;
    }

    uint32_t GuestName(uint8_t* base, const std::string& name)
    {
        if (!g_scratch) g_scratch = kernel::AllocateGuest(128, 16);
        std::memcpy(base + g_scratch, name.c_str(), std::min<size_t>(name.size() + 1, 128));
        base[g_scratch + 127] = 0;
        return g_scratch;
    }

    // The path a script the title asks for has in a mod's folder: the title
    // names a stock script by its number ("308"), a mod by its path.
    std::string ScriptPath(const std::string& asked)
    {
        if (!asked.empty() && asked.find_first_not_of("0123456789") == std::string::npos && asked.size() < 9)
        {
            const std::string name = mods::script::NameOf(uint32_t(std::stoul(asked)));
            if (!name.empty()) return Lower(name);
        }
        return Lower(asked);
    }

    const ModFile* FindScript(const std::string& path)
    {
        auto it = g_files.find(path + ".gsc");
        if (it == g_files.end()) it = g_files.find(path + ".gscbin");
        return it == g_files.end() ? nullptr : &it->second;
    }

    bool Deflate(const std::vector<uint8_t>& data, std::vector<uint8_t>& out)
    {
        uLongf size = compressBound(uLong(data.size()));
        out.resize(size);
        if (compress2(out.data(), &size, data.data(), uLong(data.size()), Z_BEST_COMPRESSION) != Z_OK) return false;
        out.resize(size);
        return true;
    }

    // The fastfiles' own copy of a script, as the compiler takes an include.
    // Read from the database's table itself, { type, asset, zone (0: only a
    // stand-in), ... }: this is the server's thread loading a level's
    // scripts, with every zone the level needs already in. The title's two
    // lookups are not for a name that may be unknown: DB_FindXAssetHeader
    // waits seconds for a zone that might still bring it, and
    // DB_IsXAssetDefault keeps the database's read lock, after which the next
    // zone to load waits for ever.
    bool StockScript(PPCContext& ctx, uint8_t* base, const std::string& path, mods::script::File* file)
    {
        std::string names[2];
        if (const uint32_t id = mods::script::IdOf(path)) names[0] = std::to_string(id);
        names[1] = path;
        for (const std::string& name : names)
        {
            if (name.empty()) continue;
            const uint32_t guest = GuestName(base, name);
            const uint32_t entry = Call(ctx, base, GUEST_FUNC(T_DB_FindXAssetEntry), kScriptFile, guest);
            if (!entry || !PPC_LOAD_U8(entry + 8)) continue;
            const uint32_t asset = PPC_LOAD_U32(entry + 4);
            if (!asset) continue;
            if (!file) return true;
            const uint32_t compressed = PPC_LOAD_U32(asset + 4), length = PPC_LOAD_U32(asset + 8);
            const uint32_t codeLength = PPC_LOAD_U32(asset + 12);
            const uint32_t buffer = PPC_LOAD_U32(asset + 16), code = PPC_LOAD_U32(asset + 20);
            file->stack.resize(length);
            uLongf size = length;
            if (uncompress(file->stack.data(), &size, base + buffer, compressed) != Z_OK) return false;
            file->stack.resize(size);
            file->bytecode.assign(base + code, base + code + codeLength);
            return true;
        }
        return false;
    }

    // gsc-tool's own file of a compiled script: "GSC", then the three lengths
    // (little-endian), the deflated names and the code.
    bool ReadCompiled(const std::vector<uint8_t>& data, std::vector<uint8_t>& deflated, uint32_t& length,
                      std::vector<uint8_t>& bytecode)
    {
        const size_t name = std::find(data.begin(), data.end(), uint8_t(0)) - data.begin();
        if (name + 13 > data.size()) return false;
        uint32_t lengths[3];
        std::memcpy(lengths, data.data() + name + 1, 12);
        if (uint64_t(name) + 13 + lengths[0] + lengths[2] != data.size()) return false;
        length = lengths[1];
        const uint8_t* at = data.data() + name + 13;
        deflated.assign(at, at + lengths[0]);
        bytecode.assign(at + lengths[0], at + lengths[0] + lengths[2]);
        return true;
    }

    // Room for an asset: what it had when that is enough, as the title may
    // still hold the address.
    bool Place(Asset& asset, uint32_t size)
    {
        if (asset.room >= size) return true;
        const uint32_t at = kernel::AllocateGuest(size, 16);
        if (!at) { LOGE("mods: out of guest memory (%u bytes)", size); return false; }
        asset.address = at;
        asset.room = size;
        return true;
    }

    // Room for a script's code. The title runs code only out of its script
    // pool: a function's handle is its offset from the pool's start,
    // Scr_GetFunctionHandle answers 0 for code outside the pool's 2 MB, and a
    // call into another script is stored as a 22-bit distance. The pool is
    // { buffer, low { , count, pos, list }, high { ... } }, the fastfiles'
    // scripts filling it from both ends, and the room between the two is lent
    // to the streamed textures, which the title's allocator takes it back
    // from. Taken during a level load, that goes wrong in this port (code
    // written there unasked is under a texture seconds later; asked for
    // through PMem_Alloc, 64 KB at a time, the match crashed in the title's
    // audio code), so the mods' room is set aside once, inside one of the
    // title's own brackets: see the PMem_EndAlloc hook.
    uint32_t PoolAlloc(uint8_t* base, uint32_t size)
    {
        size = (size + 3) & ~3u;
        if (g_arenaNext + size > g_arenaEnd) return 0;
        const uint32_t at = g_arenaNext;
        g_arenaNext += size;
        return PPC_LOAD_U32(T_DATA_ScriptPool) + at;
    }

    // What the compiler reads for `#include`: a mod's script first, then the
    // game's own.
    bool FindInclude(PPCContext& ctx, uint8_t* base, const std::string& name, mods::script::File& file)
    {
        const std::string path = Lower(name);
        if (const ModFile* mod = FindScript(path); mod && EndsWith(Lower(Utf8(mod->path.filename())), ".gsc"))
            return ReadFile(mod->path, file.text);
        return StockScript(ctx, base, path, &file);
    }

    // -> the script file asset for `asked`, 0 when no mod has that script (or
    // has one that does not compile: the game's own is used then).
    uint32_t ServeScript(PPCContext& ctx, uint8_t* base, const std::string& asked)
    {
        const std::string path = ScriptPath(asked);
        const std::string key = "s:" + path;
        const ModFile* file = FindScript(path);
        const auto known = g_assets.find(key);
        if (!file)
        {
            if (known != g_assets.end()) known->second.served = false;
            return 0;
        }
        Asset& asset = known != g_assets.end() ? known->second : g_assets[key];
        if (asset.load == g_load) return asset.served ? asset.address : 0;
        asset.load = g_load;

        std::error_code ec;
        const fs::file_time_type stamp = fs::last_write_time(file->path, ec);
        if (asset.source != file->path || asset.stamp != stamp || !asset.served)
        {
            asset.source = file->path;
            asset.stamp = stamp;
            asset.served = false;
            const std::string shown = file->mod + "/" + path;
            std::vector<uint8_t> data, deflated, bytecode;
            uint32_t length = 0;
            if (!ReadFile(file->path, data)) { LOGE("mods: %s cannot be read", shown.c_str()); return 0; }
            if (EndsWith(Lower(Utf8(file->path.filename())), ".gscbin"))
            {
                if (!ReadCompiled(data, deflated, length, bytecode))
                {
                    LOGE("mods: %s.gscbin is not a compiled script (gsc-tool's file format)", shown.c_str());
                    return 0;
                }
            }
            else
            {
                std::vector<uint8_t> stack;
                std::string error;
                const auto includes = [&ctx, base](const std::string& name, mods::script::File& found) {
                    return FindInclude(ctx, base, name, found);
                };
                if (!mods::script::Compile(path, std::move(data), includes, bytecode, stack, error))
                {
                    LOGE("mods: %s.gsc does not compile: %s", shown.c_str(), error.c_str());
                    return 0;
                }
                length = uint32_t(stack.size());
                if (!Deflate(stack, deflated)) return 0;
            }
            const bool replaces = StockScript(ctx, base, path, nullptr);

            const uint32_t nameAt = 24, bufferAt = nameAt + uint32_t(asked.size()) + 1;
            if (!Place(asset, bufferAt + uint32_t(deflated.size()))) return 0;
            const uint32_t at = asset.address;
            std::memcpy(base + at + nameAt, asked.c_str(), asked.size() + 1);
            std::memcpy(base + at + bufferAt, deflated.data(), deflated.size());
            PPC_STORE_U32(at + 0, at + nameAt);
            PPC_STORE_U32(at + 4, uint32_t(deflated.size()));
            PPC_STORE_U32(at + 8, length);
            PPC_STORE_U32(at + 12, uint32_t(bytecode.size()));
            PPC_STORE_U32(at + 16, at + bufferAt);
            asset.bytecode = std::move(bytecode);
            asset.served = true;
            LOGI("mods: %s %s script %s (%zu bytes of code)", file->mod.c_str(), replaces ? "replaces" : "adds",
                 path.c_str(), asset.bytecode.size());
        }
        // Every load places the code again, as compiled: the title's loader
        // writes the strings and functions it resolves into the code it is
        // given.
        const uint32_t code = PoolAlloc(base, uint32_t(asset.bytecode.size()));
        if (!code)
        {
            if (g_arenaEnd)
                LOGE("mods: no room for %s (%zu bytes of code): the mods' scripts together are over %u KB",
                     path.c_str(), asset.bytecode.size(), kCodeRoom >> 10);
            else
                LOGE("mods: %s is not loaded: the room for the mods' code is set aside as the game starts, and "
                     "no mod was on then. Start the game again", path.c_str());
            asset.served = false;
            return 0;
        }
        std::memcpy(base + code, asset.bytecode.data(), asset.bytecode.size());
        PPC_STORE_U32(asset.address + 20, code);
        return asset.address;
    }

    uint32_t ServeRawFile(uint8_t* base, const std::string& asked)
    {
        const std::string path = Lower(asked);
        const std::string key = "r:" + path;
        const auto found = g_files.find(path);
        const auto known = g_assets.find(key);
        if (found == g_files.end())
        {
            if (known != g_assets.end()) known->second.served = false;
            return 0;
        }
        const ModFile& file = found->second;
        Asset& asset = known != g_assets.end() ? known->second : g_assets[key];
        if (asset.load == g_load) return asset.served ? asset.address : 0;
        asset.load = g_load;

        std::error_code ec;
        const fs::file_time_type stamp = fs::last_write_time(file.path, ec);
        if (asset.source == file.path && asset.stamp == stamp && asset.served) return asset.address;
        asset.source = file.path;
        asset.stamp = stamp;
        asset.served = false;
        std::vector<uint8_t> data, deflated;
        if (!ReadFile(file.path, data) || !Deflate(data, deflated))
        {
            LOGE("mods: %s/%s cannot be read", file.mod.c_str(), path.c_str());
            return 0;
        }
        const uint32_t nameAt = 16, bufferAt = nameAt + uint32_t(asked.size()) + 1;
        if (!Place(asset, bufferAt + uint32_t(deflated.size()))) return 0;
        const uint32_t at = asset.address;
        std::memcpy(base + at + nameAt, asked.c_str(), asked.size() + 1);
        std::memcpy(base + at + bufferAt, deflated.data(), deflated.size());
        PPC_STORE_U32(at + 0, at + nameAt);
        PPC_STORE_U32(at + 4, uint32_t(deflated.size()));
        PPC_STORE_U32(at + 8, uint32_t(data.size()));
        PPC_STORE_U32(at + 12, at + bufferAt);
        asset.served = true;
        LOGI("mods: %s replaces raw file %s (%zu bytes)", file.mod.c_str(), path.c_str(), data.size());
        return at;
    }

    // A level's scripts are about to load: the folders are read again, so a
    // file edited between two matches is the one that runs.
    void BeginLoad()
    {
        std::lock_guard held(g_lock);
        Scan();
        g_load++;
        g_entries.clear();
        mods::script::Begin();
        // The last load's code is the previous level's, which has ended.
        g_arenaNext = g_arenaStart;
    }

    bool IsEntry(const std::string& path)
    {
        if (!EndsWith(path, ".gsc") && !EndsWith(path, ".gscbin")) return false;
        if (path.rfind(kOwnEntries, 0) == 0) return true;
        return path.rfind("scripts/", 0) == 0 && path.find('/', 8) == std::string::npos;
    }

    // The mods' own scripts, loaded beside the game's while the title's
    // loader is open.
    void LoadEntries(PPCContext& ctx, uint8_t* base)
    {
        std::lock_guard held(g_lock);
        if (!g_any.load(std::memory_order_acquire)) return;
        const uint32_t mainId = mods::script::IdOf("main"), initId = mods::script::IdOf("init");
        std::string last;
        for (const auto& [relative, file] : g_files)
        {
            if (!IsEntry(relative)) continue;
            const std::string name = relative.substr(0, relative.rfind('.'));
            if (name == last) continue;         // x.gsc and x.gscbin: one script
            last = name;
            if (name.size() > kLongestScriptName)
            {
                LOGW("mods: %s/%s is not run: a script's path is at most %zu characters", file.mod.c_str(),
                     relative.c_str(), kLongestScriptName);
                continue;
            }
            // Compiled before the title is asked for it: a script the title
            // cannot find stops the level.
            if (!ServeScript(ctx, base, name)) continue;
            const uint32_t guest = GuestName(base, name);
            if (!Call(ctx, base, GUEST_FUNC(T_Scr_LoadScript), guest))
            {
                LOGW("mods: the game did not load %s/%s", file.mod.c_str(), relative.c_str());
                continue;
            }
            Entry entry;
            entry.name = name;
            entry.main = Call(ctx, base, GUEST_FUNC(T_Scr_GetFunctionHandle), GuestName(base, name), mainId);
            entry.init = Call(ctx, base, GUEST_FUNC(T_Scr_GetFunctionHandle), GuestName(base, name), initId);
            LOGI("mods: %s runs %s%s%s", file.mod.c_str(), name.c_str(), entry.main ? ", main()" : "",
                 entry.init ? ", init()" : "");
            if (!entry.main && !entry.init)
                LOGW("mods: %s has neither main() nor init(), so nothing in it starts by itself", name.c_str());
            g_entries.push_back(std::move(entry));
        }
    }

    void RunEntries(PPCContext& ctx, uint8_t* base, bool init)
    {
        std::vector<uint32_t> handles;
        {
            std::lock_guard held(g_lock);
            for (const Entry& entry : g_entries)
                if (const uint32_t handle = init ? entry.init : entry.main) handles.push_back(handle);
        }
        for (const uint32_t handle : handles)
        {
            const uint32_t thread = Call(ctx, base, GUEST_FUNC(T_Scr_ExecThread), handle, 0);
            Call(ctx, base, GUEST_FUNC(T_Scr_FreeThread), thread);
        }
    }
}

// MW2_SCRIPT_DUMP=<folder>: every script of the game's own that a level loads
// is written there as source, by its path. A mod that replaces a stock script
// starts from the stock script, and the fastfiles hold only compiled ones.
static void DumpScript(PPCContext& ctx, uint8_t* base, const std::string& asked)
{
    static const char* const folder = env::Text("MW2_SCRIPT_DUMP");
    static std::set<std::string> written;
    if (!folder) return;
    const std::string path = ScriptPath(asked);
    if (!written.insert(path).second) return;
    mods::script::File file;
    if (!StockScript(ctx, base, path, &file)) return;
    std::vector<uint8_t> text;
    std::string error;
    if (!mods::script::Decompile(file.bytecode, file.stack, text, error))
    {
        LOGW("mods: %s could not be written as source: %s", path.c_str(), error.c_str());
        return;
    }
    const fs::path to = fs::path(folder) / (path + ".gsc");
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(text.data()), std::streamsize(text.size()));
    if (!out) LOGW("mods: %s cannot be written", Utf8(to).c_str());
}

// The same for the raw files the game reads, as they are.
static void DumpRawFile(PPCContext& ctx, uint8_t* base, const std::string& asked)
{
    static const char* const folder = env::Text("MW2_SCRIPT_DUMP");
    static std::set<std::string> written;
    if (!folder || asked.find_first_of("*?:<>|\"") != std::string::npos || asked.find("..") != std::string::npos) return;
    if (!written.insert(asked).second) return;
    const uint32_t entry = Call(ctx, base, GUEST_FUNC(T_DB_FindXAssetEntry), kRawFile, GuestName(base, asked));
    if (!entry || !PPC_LOAD_U8(entry + 8)) return;
    const uint32_t asset = PPC_LOAD_U32(entry + 4);
    const uint32_t compressed = PPC_LOAD_U32(asset + 4), length = PPC_LOAD_U32(asset + 8), buffer = PPC_LOAD_U32(asset + 12);
    std::vector<uint8_t> data(length);
    uLongf size = length;
    if (length && uncompress(data.data(), &size, base + buffer, compressed) != Z_OK) return;
    const fs::path to = fs::path(folder) / asked;
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(size));
}

// (type, name, allowCreateDefault) -> the asset
GUEST_HOOK(T_DB_FindXAssetHeader)
{
    const uint32_t type = ctx.r3.u32, name = ctx.r4.u32;
    if ((type == kScriptFile || type == kRawFile) && name)
    {
        std::lock_guard held(g_lock);
        ScanOnce();
        const std::string asked(reinterpret_cast<const char*>(base + name));
        // MW2_TRACE_MODS: what the game asks for, each name once -- the
        // paths a mod's file can stand for.
        static const bool traced = env::Flag("MW2_TRACE_MODS");
        static std::set<std::string> seen;
        if (traced && seen.insert(char('0' + type - kRawFile) + asked).second)
            LOGI("mods: the game asks for %s %s", type == kScriptFile ? "script" : "raw file",
                 type == kScriptFile ? ScriptPath(asked).c_str() : asked.c_str());
        if (g_any.load(std::memory_order_relaxed))
        {
            const uint32_t asset = type == kScriptFile ? ServeScript(ctx, base, asked) : ServeRawFile(base, asked);
            if (asset) { ctx.r3.u64 = asset; return; }
        }
        if (type == kScriptFile) DumpScript(ctx, base, asked);
        else DumpRawFile(ctx, base, asked);
    }
    GUEST_ORIG(T_DB_FindXAssetHeader)(ctx, base);
}

// (type, name) -> whether the database has only its stand-in for the asset,
// which is what it has for a script or a raw file that only a mod has.
GUEST_HOOK(T_DB_IsXAssetDefault)
{
    const uint32_t type = ctx.r3.u32, name = ctx.r4.u32;
    if ((type == kScriptFile || type == kRawFile) && name && g_any.load(std::memory_order_acquire))
    {
        std::lock_guard held(g_lock);
        const std::string asked(reinterpret_cast<const char*>(base + name));
        const auto it = g_assets.find(type == kScriptFile ? "s:" + ScriptPath(asked) : "r:" + Lower(asked));
        if (it != g_assets.end() && it->second.served) { ctx.r3.u64 = 0; return; }
    }
    GUEST_ORIG(T_DB_IsXAssetDefault)(ctx, base);
}

GUEST_HOOK(T_G_LoadScripts)
{
    BeginLoad();
#ifdef T_UI_MapRowOfName
    mw3maps::LevelLoading(ctx, base);
#endif
    GUEST_ORIG(T_G_LoadScripts)(ctx, base);
}

// (name, end). The title brackets what it loads at the pools' low end (end
// 0): at start-up "$init", which it keeps for the whole run and closes before
// any fastfile is loaded and before anything is streamed, and later one
// bracket for each zone. The mods' room is taken inside a bracket, with the
// title's own allocator (source 3: the script pool; protection 4, read and
// write), so it is given away only when that bracket's memory is.
//
// The multiplayer takes it in "$init", under every zone's scripts. The
// campaign cannot: a saved game holds every script thread's position as its
// distance from the pool's start, the scripts of common.ff are the first thing
// in the pool (the level's own come down from the high end), and room under
// them moves them all by its size. A game saved without a mod then resumed
// with one on, or the other way round, had its threads wake in the middle of
// other functions and the server thread never came back (the screen stayed
// black after the briefing). So there the room is taken as "common" closes,
// right above its scripts, and the game's own code lies where it does with no
// mod; it is taken again should the zone ever be loaded again.
#ifdef T_GScr_LoadGameTypeScript
static constexpr const char* kRoomBracket = "$init";
#else
static constexpr const char* kRoomBracket = "common";
#endif

GUEST_HOOK(T_PMem_EndAlloc)
{
    const uint32_t name = ctx.r3.u32;
    if (name && ctx.r4.u32 == 0 && !std::strcmp(reinterpret_cast<const char*>(base + name), kRoomBracket))
    {
        std::lock_guard held(g_lock);
        ScanOnce();
        if (g_any.load(std::memory_order_relaxed) && PPC_LOAD_U32(T_DATA_ScriptPool))
        {
            const uint32_t address = Call(ctx, base, GUEST_FUNC(T_PMem_Alloc), kCodeRoom, 4, 4, 3);
            g_arenaStart = g_arenaNext = address - PPC_LOAD_U32(T_DATA_ScriptPool);
            g_arenaEnd = g_arenaStart + kCodeRoom;
            LOGI("mods: %u KB of the game's script memory set aside for the mods' code", kCodeRoom >> 10);
        }
    }
    GUEST_ORIG(T_PMem_EndAlloc)(ctx, base);
}

#ifdef T_GScr_LoadGameTypeScript

// The multiplayer. A mod's main() runs after the game type's own and before
// the map's; its init() once the game type has started (the players'
// callbacks are set by then).
GUEST_HOOK(T_GScr_LoadGameTypeScript)
{
    GUEST_ORIG(T_GScr_LoadGameTypeScript)(ctx, base);
    LoadEntries(ctx, base);
}

GUEST_HOOK(T_Scr_LoadGameType)
{
    GUEST_ORIG(T_Scr_LoadGameType)(ctx, base);
    RunEntries(ctx, base, false);
}

GUEST_HOOK(T_Scr_StartupGameType)
{
    GUEST_ORIG(T_Scr_StartupGameType)(ctx, base);
    RunEntries(ctx, base, true);
}

#else

// The campaign, which has no game type. A mod's main() runs before the
// level's own; its init() after the level's has run to its first wait.
GUEST_HOOK(T_G_LoadLevelScripts)
{
    GUEST_ORIG(T_G_LoadLevelScripts)(ctx, base);
    LoadEntries(ctx, base);
}

GUEST_HOOK(T_Scr_LoadLevel)
{
    RunEntries(ctx, base, false);
    GUEST_ORIG(T_Scr_LoadLevel)(ctx, base);
    RunEntries(ctx, base, true);
}

#endif

#endif
#endif

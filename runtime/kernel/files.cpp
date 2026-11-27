// File I/O. Guest paths look like "game:\\zone\\common.ff" or
// "\\Device\\Cdrom0\\...", and the console filesystem is case-insensitive, so
// paths are mapped onto the game root and resolved case-insensitively.
//
// The game root is the disc and is read-only: the game folder, and under it
// the disc image where the player plays from one (volume.h), so a file the
// folder has -- a mod's, an update's -- is read before the disc's own. The
// one writable place is a mounted content package -- "save0:" and friends,
// put there by the content layer in content.cpp -- so a path whose device
// names a mount resolves under that mount instead, and may be created,
// written, truncated and deleted. A downloadable content package is mounted
// too, read-only, and read where it lies.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "volume.h"
#include "../guest.h"
#include "../diagnostics.h"
#include "../env.h"
#include "../log.h"
#include "../movie_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <unistd.h>
#include <vector>

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    // By the path's own characters: on Windows a narrow path is in the ANSI
    // code page, which cannot spell every folder a player may have.
    std::FILE* Open(const fs::path& path, const char* mode)
    {
#ifdef _WIN32
        const std::wstring wide(mode, mode + std::strlen(mode));
        return _wfopen(path.c_str(), wide.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    // Positions past 2 GB: `long` is 32 bits on Windows.
    void Seek(std::FILE* f, int64_t offset)
    {
#ifdef _WIN32
        _fseeki64(f, offset, SEEK_SET);
#else
        fseeko(f, off_t(offset), SEEK_SET);
#endif
    }
    uint64_t Tell(std::FILE* f)
    {
#ifdef _WIN32
        return uint64_t(_ftelli64(f));
#else
        return uint64_t(ftello(f));
#endif
    }

    struct FileObject : Object
    {
        std::FILE* handle = nullptr;
        fs::path path;
        uint64_t size = 0;
        bool isDirectory = false;
        bool writable = false;
        bool deleteOnClose = false;
        // Xenia exposes Harddisk0\\Partition0 as a zero-length NullDevice;
        // it has a handle but no host FILE* backing it.
        bool nullDevice = false;
        // A file inside the disc image or a content package: no host FILE*
        // either, and its own place in the file, since the descriptor it
        // reads through may be other files' too.
        std::shared_ptr<ReadVolume> volume;
        const ReadVolume::Entry* entry = nullptr;
        int descriptor = -1;
        uint64_t position = 0;
        std::mutex lock;
        // A folder being listed (NtQueryDirectoryFile): what it holds that the
        // first call's pattern names, and how far the calls have got.
        struct Listed { std::string name; uint64_t size = 0; bool directory = false; };
        std::vector<Listed> listing;
        size_t listed = 0;
        bool listingRead = false;

        const char* TypeName() const override { return "file"; }
        ~FileObject() override
        {
            // Writable is only ever true inside a mounted save package -- the
            // disc is read-only and a write anywhere else is discarded before it
            // reaches a handle -- so this is paid on saves and nothing else, and
            // it puts the bytes on storage before the title is told the handle
            // has gone. Pointless for one that is about to be deleted.
            if (handle && writable && !deleteOnClose) kernel::SyncStream(handle);
            if (handle) std::fclose(handle);
            if (volume) volume->Release(descriptor);
            // NT has no delete call of its own: a caller marks the file and the
            // deletion happens when the last handle goes.
            if (deleteOnClose)
            {
                std::error_code ec;
                fs::remove_all(path, ec);
            }
        }
    };

    struct XAnsiString { be16 length; be16 maximumLength; be32 pointer; };
    struct XObjectAttributes { be32 rootDirectory; be32 namePointer; be32 attributes; };
    struct XIoStatusBlock { be32 status; be32 information; };

    std::string ReadObjectName(uint32_t objectAttributes)
    {
        auto* oa = GuestPtr<XObjectAttributes>(objectAttributes);
        if (!oa) return {};
        auto* s = GuestPtr<XAnsiString>(uint32_t(oa->namePointer));
        if (!s) return {};
        uint32_t ptr = s->pointer;
        uint16_t len = s->length;
        if (!ptr || !len) return {};
        return std::string(reinterpret_cast<const char*>(guest::Base() + ptr), len);
    }

    // A path below a device, as the object manager reads it (Xenia's
    // canonicalize_path): "." names the folder it is in, and ".." takes back the
    // name before it, so no path climbs above its device's root, here the game
    // folder or a save package. A guest path can carry text from the network --
    // the map a host picks names a fastfile -- so this is what keeps it inside.
    // Leading and doubled separators go too.
    std::string Canonical(const std::string& path)
    {
        std::vector<std::string> parts;
        std::string component;
        auto take = [&]
        {
            if (component == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!component.empty() && component != ".") parts.push_back(component);
            component.clear();
        };
        for (char c : path)
        {
            if (c == '\\' || c == '/') take();
            else component += c;
        }
        take();
        std::string out;
        for (const auto& part : parts) { if (!out.empty()) out += '\\'; out += part; }
        return out;
    }

    // A component the host would not read as one name below the folder it is
    // joined to. On Windows a drive ("C:") replaces the whole path it is
    // appended to; elsewhere every component without a separator is a name.
    bool IsHostName(const std::string& component)
    {
        return !fs::path(component).has_root_path();
    }

    // One path component at a time.
    bool ResolveInsensitive(const fs::path& root, const std::string& relative, fs::path& out)
    {
        fs::path current = root;
        std::string component;
        auto step = [&](const std::string& name) -> bool
        {
            if (name == "..") return false;
            if (name.empty() || name == ".") return true;
            if (!IsHostName(name)) return false;
            fs::path direct = current / name;
            std::error_code ec;
            if (fs::exists(direct, ec)) { current = direct; return true; }
            if (!fs::is_directory(current, ec)) return false;
            for (auto& e : fs::directory_iterator(current, ec))
            {
                std::string have = e.path().filename().string();
                if (have.size() != name.size()) continue;
                if (std::equal(have.begin(), have.end(), name.begin(),
                               [](char a, char b){ return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }))
                { current = e.path(); return true; }
            }
            return false;
        };

        for (char c : relative)
        {
            if (c == '\\' || c == '/') { if (!step(component)) return false; component.clear(); }
            else component += c;
        }
        if (!step(component)) return false;
        out = current;
        return true;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    }

    // "game:\\zone\\common.ff"        -> device "game",   path "zone\\common.ff"
    // "\\Device\\Cdrom0\\zone\\x.ff" -> device "cdrom0", path "zone\\x.ff"
    void SplitDevice(const std::string& guestPath, std::string& device, std::string& relative)
    {
        std::string p = guestPath;
        if (auto colon = p.find(':'); colon != std::string::npos)
        {
            device = Lower(p.substr(0, colon));
            p = p.substr(colon + 1);
        }
        else if (p.rfind("\\Device\\", 0) == 0 || p.rfind("\\device\\", 0) == 0)
        {
            size_t slash = p.find('\\', 8);
            device = Lower(p.substr(8, slash == std::string::npos ? slash : slash - 8));
            p = (slash == std::string::npos) ? std::string() : p.substr(slash + 1);
        }
        relative = Canonical(p);
    }

    // The mount table the content layer fills in: a folder that may be written
    // to (a save), or a volume that may not (downloadable content). Everything
    // not in it is the disc, which is read-only.
    struct Mount
    {
        fs::path root;                      // empty: there is only the volume
        std::shared_ptr<ReadVolume> volume;
        bool writable = false;
        // A folder of the volume whose files are answered at its root too
        // (MountVolume): empty for none.
        std::string alsoAtRoot;
        // Other volumes whose files of the language the game runs in answer
        // where this one has none (MountVolume).
        std::vector<std::shared_ptr<ReadVolume>> english;
        // Other packages whose files are found on this drive as well, and a
        // name the title asks for at its root that is answered with another
        // file of the volume (MountContent).
        std::vector<std::shared_ptr<ReadVolume>> joined;
        std::string marker, markerIs;
    };
    std::mutex g_mountLock;
    std::map<std::string, Mount> g_mounts;

    Mount DeviceOf(const std::string& device)
    {
#ifdef COD4_TITLE
        // Call of Duty 4's disc executable lists a map pack, mounts it and
        // closes it again at once: it only notes that the player has one.
        // It asks for every fastfile on the disc alone ("D:\%s.ff"); the
        // pack's drive in that name came with the title update the pack was
        // sold after. So a file the disc has not is looked for in the
        // player's packages. (Found before the lock: looking for them reads
        // folders, and may mount.)
        static const std::vector<std::shared_ptr<ReadVolume>> packs = DownloadedContentVolumes();
#endif
        std::lock_guard g(g_mountLock);
        auto it = g_mounts.find(device);
        if (it != g_mounts.end()) return it->second;
        Mount disc{ GameRoot(), Disc(), false };
#ifdef COD4_TITLE
        disc.joined = packs;
#endif
        return disc;
    }

    // A file or folder of the device's volume, where its folder has none of
    // that name.
    struct InVolumeFile
    {
        std::shared_ptr<ReadVolume> volume;
        const ReadVolume::Entry* entry = nullptr;
        explicit operator bool() const { return entry != nullptr; }
    };
    InVolumeFile InVolume(const Mount& at, const std::string& relative)
    {
        if (relative.empty()) return {};
        // A game played from a folder has no volume of its own, and may
        // still have packs' files joined to it (DeviceOf).
        if (at.volume)
        {
            if (const ReadVolume::Entry* entry = at.volume->Find(relative)) return { at.volume, entry };
            if (!at.alsoAtRoot.empty() && relative.find_first_of("\\/") == std::string::npos)
                if (const ReadVolume::Entry* entry = at.volume->Find(at.alsoAtRoot + "\\" + relative)) return { at.volume, entry };
            if (!at.marker.empty() && Lower(relative) == at.marker)
                if (const ReadVolume::Entry* entry = at.volume->Find(at.markerIs)) return { at.volume, entry };
        }
        for (const auto& other : at.joined)
            if (const ReadVolume::Entry* entry = other->Find(relative)) return { other, entry };
        if (Lower(relative).rfind("english\\", 0) == 0)
            for (const auto& other : at.english)
                if (const ReadVolume::Entry* entry = other->Find(relative)) return { other, entry };
        return {};
    }

    // Where a name that is not there yet would go. Only ever used under a mount,
    // so there is no case-insensitive match to make: this name is the one being
    // created. False when a component is not a name the host can create.
    bool HostPath(const fs::path& root, const std::string& relative, fs::path& out)
    {
        out = root;
        std::string component;
        auto take = [&]
        {
            if (component.empty()) return true;
            if (component == ".." || !IsHostName(component)) return false;
            out /= component;
            component.clear();
            return true;
        };
        for (char c : relative)
        {
            if (c == '\\' || c == '/') { if (!take()) return false; }
            else component += c;
        }
        return take();
    }

    std::mutex g_missingLock;
    std::map<std::string, uint64_t> g_missing;

    void NoteMissing(const char* operation, const std::string& guestPath,
                     const char* detail = "")
    {
        if constexpr (!diag::kOn) return;
        // An empty name means the open carried no ANSI_STRING at all, which is a
        // different bug from a genuinely absent file.
        const std::string key = guestPath.empty() ? "<no name in OBJECT_ATTRIBUTES>" : guestPath;
        std::lock_guard g(g_missingLock);
        auto [it, inserted] = g_missing.try_emplace(key, 0);
        it->second++;
        if (inserted)
            LOGK("file not found via %s: %s%s", operation, key.c_str(),
                 detail && *detail ? detail : "");
    }

    bool TraceFiles()
    {
        static const bool on = diag::Flag("MW2_TRACE_FILES");
        return on;
    }

    // NT create dispositions, and what the caller is told happened.
    // STATUS_ACCESS_DENIED (NTSTATUS 0xC0000022), not defined by the shared headers.
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022u;
    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2,
                       FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2,
                       FILE_OVERWRITTEN = 3;
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;

    bool WantsToWrite(uint32_t desiredAccess)
    {
        constexpr uint32_t GENERIC_WRITE = 0x40000000, GENERIC_ALL = 0x10000000;
        constexpr uint32_t FILE_WRITE_DATA = 0x0002, FILE_APPEND_DATA = 0x0004;
        return (desiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    }

    bool IsHarddiskPartition0(const std::string& device, const std::string& relative)
    {
        return device == "harddisk0" && Lower(relative) == "partition0";
    }

    uint32_t OpenFileCommon(const char* operation, const std::string& guestPath,
                            uint32_t desiredAccess, uint32_t disposition, uint32_t options,
                            uint32_t& handleOut, uint32_t& information)
    {
        std::string device, relative;
        SplitDevice(guestPath, device, relative);

        // Xenia registers Partition0 (along with cache placeholders) as a
        // NullDevice rather than mapping it onto the game directory. MW3 has
        // been observed opening this exact object read-only with NtCreateFile.
        if (IsHarddiskPartition0(device, relative))
        {
            if (WantsToWrite(desiredAccess) || disposition == FILE_SUPERSEDE ||
                disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF)
                return X_STATUS_ACCESS_DENIED;
            if (disposition == FILE_CREATE)
                return X_STATUS_OBJECT_NAME_COLLISION;

            auto f = std::make_shared<FileObject>();
            f->nullDevice = true;
            information = FILE_OPENED;
            handleOut = InsertHandle(f);
            if (TraceFiles()) LOGK("open %s -> Harddisk0\\Partition0 (null device)", guestPath.c_str());
            return X_STATUS_SUCCESS;
        }

        const Mount at = DeviceOf(device);
        const bool writable = at.writable;
        const fs::path& root = at.root;

#if defined(AW_TITLE) || defined(BO3_TITLE)
        // The root of a content package, opened as a folder to list it:
        // Advanced Warfare reads what an installed package holds that way
        // (docs/AW-BRINGUP.md). For this title only: the earlier ones were
        // told there is no such folder, and went on.
        if (relative.empty() && at.root.empty() && at.volume && (options & FILE_DIRECTORY_FILE) && disposition == FILE_OPEN)
        {
            auto f = std::make_shared<FileObject>();
            f->volume = at.volume;
            f->isDirectory = true;
            information = FILE_OPENED;
            handleOut = InsertHandle(f);
            if (TraceFiles()) LOGK("open %s -> the root of %s (to list)", guestPath.c_str(), at.volume->Name().c_str());
            return X_STATUS_SUCCESS;
        }
#endif

        fs::path resolved;
        bool existed = false;
        // A mods directory laid over the read-only devices, and only when
        // MW2_MOD_DIR names one: nothing beside the game is picked up unasked.
        static const char* const modDir = env::Text("MW2_MOD_DIR");
        if (modDir && !writable && !relative.empty())
        {
            const fs::path mods(modDir);
            std::error_code ec;
            if (fs::is_directory(mods, ec))
                existed = ResolveInsensitive(mods, relative, resolved);
        }
        if (!existed)
            existed = !relative.empty() && !root.empty() && ResolveInsensitive(root, relative, resolved);
        if (const InVolumeFile found = existed ? InVolumeFile{} : InVolume(at, relative))
        {
            const ReadVolume::Entry* entry = found.entry;
            if (disposition == FILE_CREATE) return X_STATUS_OBJECT_NAME_COLLISION;
            auto f = std::make_shared<FileObject>();
            f->path = relative;
            f->volume = found.volume;
            f->entry = entry;
            f->size = entry->size;
            f->isDirectory = entry->directory;
            if (!entry->directory) f->descriptor = found.volume->Acquire();
            information = FILE_OPENED;
            handleOut = InsertHandle(f);
            if (TraceFiles())
                LOGK("open %s -> %s in %s (read-only)", guestPath.c_str(), relative.c_str(), found.volume->Name().c_str());
            return X_STATUS_SUCCESS;
        }
        if (!existed)
        {
            // A name that is not there is only an error until something is allowed
            // to make it: the disc cannot be written to, and a disposition that
            // only opens does not create.
            if (relative.empty() || !writable ||
                disposition == FILE_OPEN || disposition == FILE_OVERWRITE ||
                !HostPath(root, relative, resolved))
            {
                char detail[192];
                std::snprintf(detail, sizeof(detail),
                              " (device=%s, relative=%s, access=%08X, disposition=%u, options=%08X)",
                              device.c_str(), relative.c_str(), desiredAccess, disposition, options);
                NoteMissing(operation, guestPath, detail);
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
        else if (disposition == FILE_CREATE)
        {
            return X_STATUS_OBJECT_NAME_COLLISION;
        }

        auto f = std::make_shared<FileObject>();
        f->path = resolved;
        std::error_code ec;
        if ((options & FILE_DIRECTORY_FILE) || (existed && fs::is_directory(resolved, ec)))
        {
            if (!existed && !fs::create_directories(resolved, ec))
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            f->isDirectory = true;
            information = existed ? FILE_OPENED : FILE_CREATED;
        }
        else
        {
            // Truncating dispositions and a file that is not there yet both mean
            // starting from empty; anything else keeps what is on disk. Only ever
            // under a mount: the disc is read-only, and a disposition asking to
            // overwrite one of its files must not be taken at its word.
            const bool truncate = writable &&
                                  (!existed || disposition == FILE_SUPERSEDE ||
                                   disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF);
            f->writable = writable && (truncate || WantsToWrite(desiredAccess));
            if (!existed) fs::create_directories(resolved.parent_path(), ec);

            const char* mode = truncate ? "w+b" : (f->writable ? "r+b" : "rb");
            f->handle = Open(resolved, mode);
            if (!f->handle)
            {
                NoteMissing(operation, guestPath, " (host file open failed)");
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            f->size = truncate ? 0 : fs::file_size(resolved, ec);
            information = !existed ? FILE_CREATED
                        : truncate ? (disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN)
                                   : FILE_OPENED;
        }
        handleOut = InsertHandle(f);
        if (TraceFiles())
            LOGK("open %s -> %s (%s)", guestPath.c_str(), resolved.string().c_str(),
                 f->writable ? "writable" : "read-only");
        return X_STATUS_SUCCESS;
    }
}

void kernel::MountDevice(const std::string& name, const fs::path& root)
{
    std::lock_guard g(g_mountLock);
    g_mounts[Lower(name)] = Mount{ root, nullptr, true };
}

void kernel::MountVolume(const std::string& name, std::shared_ptr<ReadVolume> volume, const std::string& alsoAtRoot,
                         std::vector<std::shared_ptr<ReadVolume>> english)
{
    std::lock_guard g(g_mountLock);
    g_mounts[Lower(name)] = Mount{ {}, std::move(volume), false, alsoAtRoot, std::move(english) };
}

void kernel::MountContent(const std::string& name, std::shared_ptr<ReadVolume> volume,
                          std::vector<std::shared_ptr<ReadVolume>> joined, const std::string& marker,
                          const std::string& markerIs)
{
    std::lock_guard g(g_mountLock);
    Mount mount{ {}, std::move(volume), false };
    mount.joined = std::move(joined);
    mount.marker = Lower(marker);
    mount.markerIs = markerIs;
    g_mounts[Lower(name)] = std::move(mount);
}

void kernel::UnmountDevice(const std::string& name)
{
    std::lock_guard g(g_mountLock);
    g_mounts.erase(Lower(name));
}

void kernel::ReportMissingFiles()
{
    if constexpr (!diag::kOn) return;
    std::lock_guard g(g_missingLock);
    if (g_missing.empty()) return;
    LOGI("files the guest asked for and did not get (%zu distinct):", g_missing.size());
    size_t n = 0;
    for (auto& [path, count] : g_missing)
    {
        LOGI("  %8llu  %s", (unsigned long long)count, path.c_str());
        if (++n >= 40) { LOGI("  ... and %zu more", g_missing.size() - n); break; }
    }
}

PPC_FUNC(__imp__NtCreateFile)
{
    auto* handleOut = GuestPtr<be32>(ctx.r3.u32);
    std::string name = ReadObjectName(ctx.r5.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r6.u32);
    const uint32_t options = kernel::StackArgument(ctx, 8);   // CreateOptions

    uint32_t handle = 0, information = 0;
    uint32_t status = OpenFileCommon("NtCreateFile", name, ctx.r4.u32, ctx.r10.u32,
                                     options, handle, information);
    if (handleOut && status == X_STATUS_SUCCESS) *handleOut = handle;
    if (iosb) { iosb->status = status; iosb->information = (status == X_STATUS_SUCCESS) ? information : 0u; }
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtOpenFile)
{
    auto* handleOut = GuestPtr<be32>(ctx.r3.u32);
    std::string name = ReadObjectName(ctx.r5.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r6.u32);

    uint32_t handle = 0, information = 0;
    uint32_t status = OpenFileCommon("NtOpenFile", name, ctx.r4.u32, FILE_OPEN,
                                     ctx.r8.u32, handle, information);
    if (handleOut && status == X_STATUS_SUCCESS) *handleOut = handle;
    if (iosb) { iosb->status = status; iosb->information = (status == X_STATUS_SUCCESS) ? information : 0u; }
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtReadFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto event = LookupHandleAs<Event>(ctx.r4.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r7.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    if (!file || (!file->handle && !file->nullDevice && !file->entry) || !buffer)
    {
        if (iosb) { iosb->status = X_STATUS_INVALID_PARAMETER; iosb->information = 0; }
        if (event) event->Set();
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    int64_t offset = offsetPtr ? int64_t(uint64_t(*GuestPtr<be64>(offsetPtr))) : -1;
    uint32_t apcRoutine = ctx.r5.u32 & ~3u;

    if (event) event->Reset();

    // Deferred to APC-delivery time when the caller asked for a completion
    // routine, so the buffer is filled at the moment the title is waiting for it
    // rather than while it is still decompressing out of the other half of its
    // double buffer.
    auto transfer = [file, event, buffer, length, offset, iosb]
    {
        std::lock_guard g(file->lock);
        // The Xbox cache/config placeholder is a zero-length NullDevice: reads
        // complete successfully without transferring bytes (not EOF).
        if (file->nullDevice)
        {
            if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = 0; }
            return;
        }
        // Into a buffer of our own and copied across, not read straight into
        // guest memory: a page the GPU's shadow watches is read-only, and the
        // kernel fails a read into it where a copy faults and carries on.
        thread_local std::vector<uint8_t> bounce;
        if (bounce.size() < length) bounce.resize(length);
        size_t got;
        const double began = movies::On() ? movies::Clock() : 0.0;
        if (file->entry)
        {
            const uint64_t at = offset >= 0 ? uint64_t(offset) : file->position;
            got = file->volume->Read(file->descriptor, *file->entry, at, bounce.data(), length);
            file->position = at + got;
        }
        else
        {
            // C requires a positioning call between a write and a read on the same
            // update stream; seeking to where it already is counts as one.
            if (offset >= 0) Seek(file->handle, offset);
            else if (file->writable) std::fseek(file->handle, 0, SEEK_CUR);
            got = std::fread(bounce.data(), 1, length, file->handle);
        }
        if (movies::On())
            movies::Read(file->path.filename().string().c_str(), length, offset, movies::Clock() - began);
        std::memcpy(guest::Base() + buffer, bounce.data(), got);
        uint32_t status = (got == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
        if (iosb) { iosb->status = status; iosb->information = uint32_t(got); }
        if (event) event->Set();
        if (TraceFiles())
            LOGK("read %s: %u bytes at %lld -> %zu into %08X (%s)", file->path.filename().string().c_str(),
                 length, (long long)offset, got, buffer, status ? "eof" : "ok");
    };

    if (!apcRoutine)
    {
        transfer();
        ctx.r3.u64 = iosb ? uint32_t(iosb->status) : X_STATUS_SUCCESS;
        return;
    }

    // The outcome lands in the status block when the APC runs. Bit 0 of the
    // routine is NT's APC-mode flag.
    if (iosb) { iosb->status = X_STATUS_PENDING; iosb->information = 0; }
    kernel::QueueUserApc(apcRoutine, ctx.r6.u32, ctx.r7.u32, std::move(transfer));
    ctx.r3.u64 = X_STATUS_PENDING;
}

PPC_FUNC(__imp__NtWriteFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto event = LookupHandleAs<Event>(ctx.r4.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r7.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    // Partition0 is a read-only NullDevice, not the ordinary discard-write
    // behavior used for the read-only game disc.
    if (file && file->nullDevice)
    {
        if (iosb) { iosb->status = X_STATUS_ACCESS_DENIED; iosb->information = 0; }
        ctx.r3.u64 = X_STATUS_ACCESS_DENIED;
        return;
    }

    // A write to anything but a mounted package is a write to the disc -- the
    // title's own logs go there. Accept and discard those, as before.
    if (!file || !file->handle || !file->writable || !buffer)
    {
        if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = length; }
        if (event) event->Set();
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }

    std::lock_guard g(file->lock);
    const int64_t offset = offsetPtr ? int64_t(uint64_t(*GuestPtr<be64>(offsetPtr))) : -1;
    if (offset >= 0) Seek(file->handle, offset);
    else std::fseek(file->handle, 0, SEEK_CUR);          // the same rule, the other way round
    const size_t put = std::fwrite(guest::Base() + buffer, 1, length, file->handle);
    std::fflush(file->handle);
    const uint64_t end = Tell(file->handle);
    if (end > file->size) file->size = end;

    const uint32_t status = (put == length) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
    if (iosb) { iosb->status = status; iosb->information = uint32_t(put); }
    if (event) event->Set();
    if (TraceFiles())
        LOGK("write %s: %u bytes at %lld -> %zu", file->path.filename().string().c_str(),
             length, (long long)offset, put);
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    if (!file) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    constexpr uint32_t FileStandardInformation = 5;
    constexpr uint32_t FilePositionInformation = 14;
    constexpr uint32_t FileNetworkOpenInformation = 34;

    if (infoClass == FileStandardInformation && buffer)
    {
        auto* p = GuestPtr<be64>(buffer);
        p[0] = file->size;   // AllocationSize
        p[1] = file->size;   // EndOfFile
        auto* extra = GuestPtr<be32>(buffer + 16);
        extra[0] = 1;                                  // NumberOfLinks
        extra[1] = file->isDirectory ? 0x01000000u : 0;// DeletePending / Directory
    }
    else if (infoClass == FilePositionInformation && buffer)
    {
        std::lock_guard g(file->lock);
        *GuestPtr<be64>(buffer) = file->entry ? file->position : file->handle ? Tell(file->handle) : 0ull;
    }
    else if (infoClass == FileNetworkOpenInformation && buffer)
    {
        auto* p = GuestPtr<be64>(buffer);
        for (int i = 0; i < 4; i++) p[i] = 0;   // timestamps
        p[4] = file->size;                      // AllocationSize
        p[5] = file->size;                      // EndOfFile
    }

    if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = 0; }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    std::string name = ReadObjectName(ctx.r3.u32);
    std::string device, relative;
    SplitDevice(name, device, relative);
    if (IsHarddiskPartition0(device, relative))
    {
        if (auto* p = GuestPtr<be64>(ctx.r4.u32))
        {
            for (int i = 0; i < 6; i++) p[i] = 0;
            *GuestPtr<be32>(ctx.r4.u32 + 48) = 0x80u; // FILE_ATTRIBUTE_NORMAL
        }
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    const Mount at = DeviceOf(device);
    fs::path resolved;
    const bool onHost = !relative.empty() && !at.root.empty() && ResolveInsensitive(at.root, relative, resolved);
    const ReadVolume::Entry* entry = onHost ? nullptr : InVolume(at, relative).entry;
    if (!onHost && !entry)
    {
        NoteMissing("NtQueryFullAttributesFile", name);
        ctx.r3.u64 = X_STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    std::error_code ec;
    const bool dir = entry ? entry->directory : fs::is_directory(resolved, ec);
    const uint64_t size = entry ? entry->size : dir ? 0 : fs::file_size(resolved, ec);
    if (auto* p = GuestPtr<be64>(ctx.r4.u32))
    {
        for (int i = 0; i < 4; i++) p[i] = 0;
        p[4] = size;
        p[5] = size;
        *GuestPtr<be32>(ctx.r4.u32 + 48) = dir ? 0x10u : 0x80u;  // DIRECTORY / NORMAL
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    constexpr uint32_t FileDispositionInformation = 13;
    constexpr uint32_t FilePositionInformation = 14;
    constexpr uint32_t FileEndOfFileInformation = 20;

    if (file && buffer)
    {
        std::lock_guard g(file->lock);
        if (infoClass == FilePositionInformation && file->entry)
        {
            file->position = uint64_t(*GuestPtr<be64>(buffer));
        }
        else if (infoClass == FilePositionInformation && file->handle)
        {
            Seek(file->handle, int64_t(uint64_t(*GuestPtr<be64>(buffer))));
        }
        else if (infoClass == FileEndOfFileInformation && file->handle && file->writable)
        {
            const uint64_t end = *GuestPtr<be64>(buffer);
            std::fflush(file->handle);
            if (ftruncate(fileno(file->handle), off_t(end)) == 0) file->size = end;
        }
        else if (infoClass == FileDispositionInformation)
        {
            // A single byte: non-zero means delete when the last handle closes.
            file->deleteOnClose = file->writable && *GuestPtr<uint8_t>(buffer) != 0;
        }
    }

    if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = 0; }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

namespace
{
    // A file name against a pattern of FindFirstFile's: `*` is any run of
    // characters and `?` any one, letters in either case.
    bool MatchesPattern(const char* pattern, const char* name)
    {
        if (!*pattern) return !*name;
        if (*pattern == '*')
        {
            for (;; name++)
            {
                if (MatchesPattern(pattern + 1, name)) return true;
                if (!*name) return false;
            }
        }
        if (!*name) return false;
        if (*pattern != '?' && std::tolower(uint8_t(*pattern)) != std::tolower(uint8_t(*name))) return false;
        return MatchesPattern(pattern + 1, name + 1);
    }

    // What a folder holds: a package's root from the package, a folder of the
    // host's from the host.
    void ReadListing(FileObject& folder, const std::string& pattern)
    {
        folder.listingRead = true;
        folder.listing.clear();
        folder.listed = 0;
        auto wanted = [&](const std::string& name) { return pattern.empty() || MatchesPattern(pattern.c_str(), name.c_str()); };
        if (folder.volume)
        {
#if defined(BO3_TITLE)
            // Black Ops III walks a package folder by folder (sub_82660FA8)
            // and keeps every file's name with the folders above it: a sound
            // bank asked for as D:\snd\en\<bank> is read from the package
            // whose snd folder has it, and without the folders listed the
            // title looked on the disc alone and gave up on the first bank
            // only the update has. Advanced Warfare is told a folder's files
            // alone, as its reading of a package was verified.
            const std::string within = folder.path.string();
            if (const auto* names = folder.volume->Children(within))
                for (const std::string& name : *names)
                {
                    const ReadVolume::Entry* entry = folder.volume->Find(within.empty() ? name : within + "\\" + name);
                    if (entry && wanted(name)) folder.listing.push_back({ name, entry->size, entry->directory });
                }
#else
            // The package's root, or one of its folders (a language's): the
            // title adds every name it is told to one list of 1440, and a
            // folder answered with the root's files filled it twice over.
            const std::string in = folder.path.string();
            for (const std::string& name : in.empty() ? folder.volume->RootFiles() : folder.volume->FilesIn(in))
                if (const ReadVolume::Entry* entry = folder.volume->Find(in.empty() ? name : in + "\\" + name); entry && wanted(name))
                    folder.listing.push_back({ name, entry->size, entry->directory });
#endif
            return;
        }
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder.path, ec))
        {
            const std::string name = entry.path().filename().string();
            if (!wanted(name)) continue;
            std::error_code entryError;
            const bool directory = entry.is_directory(entryError);
            folder.listing.push_back({ name, directory ? 0 : uint64_t(entry.file_size(entryError)), directory });
        }
    }
}

// NtQueryDirectoryFile(handle, event, apc routine, apc context, status block,
// information, length, name pattern, restart): FindFirstFile and FindNextFile.
// One FILE_DIRECTORY_INFORMATION a call -- { next entry, index, four times,
// size, allocated size, attributes, name length, name } -- which is as much
// as the console's own FindNextFile asks for.
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006u, X_STATUS_NO_SUCH_FILE = 0xC000000Fu,
                       X_STATUS_INVALID_HANDLE_ = 0xC0000008u, X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004u;
    constexpr uint32_t kNameOffset = 0x40;
    auto folder = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r7.u32);
    const uint32_t information = ctx.r8.u32, length = ctx.r9.u32;
    const bool restart = kernel::StackArgument(ctx, 8) != 0;

    uint32_t status = X_STATUS_SUCCESS, written = 0;
    if (!folder || !folder->isDirectory) status = X_STATUS_INVALID_HANDLE_;
    else if (!information || length < kNameOffset) status = X_STATUS_INFO_LENGTH_MISMATCH;
    else
    {
        std::lock_guard held(folder->lock);
        const bool first = !folder->listingRead || restart;
        if (first)
        {
            std::string pattern;
            if (auto* name = GuestPtr<XAnsiString>(ctx.r10.u32); name && uint32_t(name->pointer) && uint16_t(name->length))
                pattern.assign(GuestPtr<char>(uint32_t(name->pointer)), uint16_t(name->length));
            ReadListing(*folder, pattern);
        }
        if (folder->listed >= folder->listing.size())
            status = first ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
        else if (const auto& entry = folder->listing[folder->listed]; kNameOffset + entry.name.size() > length)
            status = X_STATUS_INFO_LENGTH_MISMATCH;
        else
        {
            std::memset(GuestPtr<uint8_t>(information), 0, kNameOffset);
            *GuestPtr<be32>(information + 0x04) = uint32_t(folder->listed);
            *GuestPtr<be64>(information + 0x28) = entry.size;
            *GuestPtr<be64>(information + 0x30) = (entry.size + 0xFFF) & ~uint64_t(0xFFF);
            *GuestPtr<be32>(information + 0x38) = entry.directory ? 0x10u : 0x80u;   // DIRECTORY / NORMAL
            *GuestPtr<be32>(information + 0x3C) = uint32_t(entry.name.size());
            std::memcpy(GuestPtr<char>(information + kNameOffset), entry.name.data(), entry.name.size());
            written = kNameOffset + uint32_t(entry.name.size());
            folder->listed++;
        }
    }
    if (iosb) { iosb->status = status; iosb->information = written; }
    if (TraceFiles()) LOGK("list handle %08X -> status %08X, %u bytes", ctx.r3.u32, status, written);
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    if (auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32)))
    {
        std::lock_guard g(file->lock);
        // The title asking for its save to be on disk, and the only chance it
        // gives to make that true. fflush on its own hands the bytes to the
        // kernel and returns; the kernel writes them back when it chooses, so a
        // kill in between loses a save the title was told had landed.
        if (file->handle) kernel::SyncStream(file->handle);
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtReadFileScatter) { ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED; }

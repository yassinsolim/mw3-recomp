// Saved games.
//
// The title's save system is two steps. Memcard_* asks XContentCreate to mount
// a content package as a lettered drive -- "save0:" -- and then opens
// "save0:\savegame.svg" with the ordinary file API and writes it like any other
// file. Reporting no device is what puts the save-device prompt in front of the
// player before every level.
//
// A package on the console is an STFS container, a signed archive with the save
// inside it. Nothing here writes that format: a package is a directory under
// `saves/`, and mounting it is just an entry in the file layer's device table.
// The title never looks inside the container -- it only asks XAM to mount one
// and then reads and writes files through the drive letter -- so a directory is
// enough for it to save, load and delete.
//
// Downloadable content is the other kind of package, and the other way round:
// a map pack is a package the player bought, read-only, which the multiplayer
// lists (XamContentCreateEnumerator, type 2), mounts ("mappack1:", "dlc1:")
// and reads fastfiles out of. Those are the player's own files from their
// console, kept in a `dlc` folder as the console has them, and read where
// they lie (volume.h, docs/dlc.md).
#include <algorithm>
#include <ppc_recomp_shared.h>
#include "title.h"
#include "kernel.h"
#include "objects.h"
#include "volume.h"
#include "../guest.h"
#include "../log.h"
#include "../env.h"
#include "../../launcher/disc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    // XCONTENT_DATA, as the title builds it: the device it chose, what kind of
    // content this is, a display name for the dashboard and the name on disk.
    constexpr uint32_t kContentDeviceId   = 0;
    constexpr uint32_t kContentType       = 4;
    constexpr uint32_t kContentFileName   = 264;
    constexpr uint32_t kContentFileMax    = 42;

    // XCONTENTFLAG_*: the low nibble of the flags is a create disposition, the
    // same idea as NT's.
    constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3,
                       kOpenAlways = 4, kTruncate = 5;

    std::string GuestString(uint32_t address, size_t limit)
    {
        if (!address) return {};
        const char* p = reinterpret_cast<const char*>(guest::Base() + address);
        size_t n = 0;
        while (n < limit && p[n]) n++;
        return std::string(p, n);
    }

    // The name goes on the host filesystem, so it may only be a name.
    std::string SafeName(const std::string& in)
    {
        std::string out;
        for (char c : in)
        {
            const bool ok = std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.';
            out += ok ? c : '_';
        }
        while (!out.empty() && out.front() == '.') out.erase(out.begin());
        return out;
    }


    // Memcard_InitializeSystem keeps the device each controller saved to in a
    // table of one word per controller, and everything else asks that table
    // first: a zero there means "nobody has chosen yet", which is the state the
    // save-device prompt exists to get out of. On the console the choice is
    // remembered in the player's profile, so it is made once ever; here the
    // table starts empty on every launch, so the prompt would come back on every
    // launch. There is only one device to choose, so choose it.
    constexpr uint32_t kDeviceTable = T_DATA_DeviceTable;
    constexpr uint32_t kControllers = 4;

    bool ChooseDeviceForPlayer()
    {
        static const bool on = !env::Flag("MW2_NO_AUTO_SAVE_DEVICE");
        return on;
    }

    // A clearing disposition means the title wants the package empty to write a
    // new save into. Deleting the old one outright is what the console does,
    // but it opens a window as long as the title's own write in which the
    // player has no save at all -- and on Android the low memory killer can
    // take the process away inside it, which costs a campaign. The old package
    // moves aside instead, one generation deep, so some whole save is always on
    // disk under one name or the other.
    //
    // Nothing reads the backup automatically: the save is the title's own
    // format and there is no way from here to tell a truncated one from a
    // short one. Recovery is renaming "<name>.bak" back over "<name>".
    void RotatePackage(const fs::path& package)
    {
        std::error_code ec;
        fs::path backup = package;
        backup += ".bak";

        fs::remove_all(backup, ec);
        ec.clear();
        fs::rename(package, backup, ec);
        if (!ec)
        {
            LOGK("content: kept the previous %s as %s", package.filename().string().c_str(),
                 backup.filename().string().c_str());
            return;
        }

        // Could not move it aside, and the title still needs the name empty.
        LOGW("content: cannot keep a backup of %s (%s); emptying it in place",
             package.string().c_str(), ec.message().c_str());
        ec.clear();
        fs::remove_all(package, ec);
    }

    // XCONTENTTYPE_MARKETPLACE: what a downloaded map pack is.
    constexpr uint32_t kMarketplaceContent = 2;
    constexpr uint32_t kContentDisplayName = 8, kContentDisplayMax = 128;
    constexpr uint32_t kContentDataSize = 0x134;

    // XCONTENTTYPE_INSTALLER: what a title update is, which the console keeps
    // under Content\0000000000000000\<title>\000B0000.
    constexpr uint32_t kTitleUpdate = 0x000B0000;

    struct Downloaded
    {
        std::string name;               // as XCONTENT_DATA names it
        std::shared_ptr<ReadVolume> volume;
    };

    struct Packages
    {
        std::vector<Downloaded> content;
        std::shared_ptr<ReadVolume> update;     // the title update's, for a build of its executable
    };

    // Takes `volume`, the package `where` opened under the name `host`: this
    // title's downloadable content when it is not listed already, or its
    // title update's package.
    void AddPackage(const std::shared_ptr<ReadVolume>& volume, const std::string& host, const std::string& where,
                    const std::string& error, Packages& packages)
    {
        if (!volume)
        {
            LOGW("content: %s %s; it is not used", where.c_str(), error.c_str());
            return;
        }
        const ReadVolume::Package& header = volume->Header();
        if (header.titleId == T_TITLE_ID && header.contentType == kTitleUpdate)
        {
#if defined(AW_TITLE_TU17)
            // A console has one. Of several (updates of two pressings of
            // the disc make the same executable and differ in a language's
            // folder) the first found serves.
            if (!packages.update)
            {
                LOGI("content: title update %s (%zu files)", where.c_str(), volume->RootFiles().size());
                packages.update = volume;
            }
#else
            LOGI("content: %s is a title update, whose files are for the executable it makes; this is the disc's",
                 where.c_str());
#endif
            return;
        }
        if (header.titleId != T_TITLE_ID || header.contentType != kMarketplaceContent)
        {
            LOGW("content: %s is for title %08X, content type %08X, not this title's downloadable content",
                 where.c_str(), header.titleId, header.contentType);
            return;
        }
        std::vector<Downloaded>& found = packages.content;
        const std::string name = SafeName(host).substr(0, kContentFileMax - 1);
        bool listed = false;
        for (const Downloaded& other : found) listed = listed || other.name == name;
        if (listed || name.empty()) return;
        LOGI("content: downloadable content %s (%zu files, licence bits %08X)", where.c_str(),
             volume->RootFiles().size(), header.licenceBits);
        found.push_back({ name, volume });
    }

    // Every package of this title's downloadable content under `folder`, which
    // may be the console's own tree (Content\0000000000000000\<title>\00000002)
    // copied whole, or the packages alone; and its title update's package.
    void FindDownloaded(const fs::path& folder, Packages& packages)
    {
        constexpr int kDeepest = 6;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (it.depth() >= kDeepest) it.disable_recursion_pending();
            std::error_code entryError;
            if (!it->is_regular_file(entryError)) continue;
            std::string error;
            const auto volume = ReadVolume::OpenPackage(it->path(), error);
            AddPackage(volume, it->path().filename().string(), it->path().string(), error, packages);
        }
    }

    // The packages a disc image holds where a console's hard disk has them
    // after the disc's install (content/0000000000000000/<title>/00000002/):
    // Advanced Warfare's install disc, read where it lies with nothing copied
    // out. `image` is named as the disc image is (volume.h).
    void FindInImage(const std::string& image, Packages& found)
    {
        std::string error;
        const auto listing = install::Source::Open(install::FromUtf8(image), error);
        if (!listing)
        {
            LOGW("content: the install disc's image %s: %s; its content is not used", image.c_str(), error.c_str());
            return;
        }
        char folder[64];
        std::snprintf(folder, sizeof folder, "content/0000000000000000/%08x/00000002/", unsigned(T_TITLE_ID));
        const size_t length = std::strlen(folder);
        // By name, as a folder of them is listed: the order the image's own
        // tree gives them is not the order the title has met them in.
        std::vector<const install::Source::File*> packages;
        for (const auto& file : listing->DeepFiles())
        {
            std::string lower = file.name;
            for (char& c : lower) c = char(std::tolower(uint8_t(c)));
            if (lower.size() > length && lower.compare(0, length, folder) == 0 &&
                lower.find('/', length) == std::string::npos)
                packages.push_back(&file);
        }
        std::sort(packages.begin(), packages.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
        for (const auto* file : packages)
        {
            const auto volume = ReadVolume::OpenPackage(image, file->offset, file->size, error);
            AddPackage(volume, file->name.substr(length), image + ":" + file->name, error, found);
        }
    }

    // Looked for once, when the title first asks: MW2_DLC_DIR, the `dlc`
    // folder beside the game folder, the one beside the disc image, and the
    // install disc's image (MW2_INSTALL_DISC_IMAGE). Copies in a folder come
    // first: a player who has them need not have the image in. `gameRoot` is
    // for the one that asks before the kernel is told the game folder
    // (ReadTitleUpdateFile).
    const Packages& InstalledPackages(const fs::path* gameRoot = nullptr)
    {
        static const Packages packages = [gameRoot]
        {
            Packages found;
            // One folder or several, with semicolons between: the install
            // disc's packages and the player's packs need not be in one.
            if (const char* named = env::Text("MW2_DLC_DIR"))
            {
                const std::string folders = named;
                for (size_t at = 0; at <= folders.size();)
                {
                    const size_t end = std::min(folders.find(';', at), folders.size());
                    if (end > at) FindDownloaded(fs::path(folders.substr(at, end - at)), found);
                    at = end + 1;
                }
            }
            FindDownloaded(fs::absolute(gameRoot ? *gameRoot : GameRoot()).parent_path() / "dlc", found);
            if (const auto& disc = Disc(); disc && !disc->Path().empty())
                FindDownloaded(fs::absolute(disc->Path()).parent_path() / "dlc", found);
            if (const char* image = env::Text("MW2_INSTALL_DISC_IMAGE")) FindInImage(image, found);
            return found;
        }();
        return packages;
    }

    const std::vector<Downloaded>& DownloadedContent() { return InstalledPackages().content; }
}

#if defined(AW_TITLE_TU17)
namespace
{
    // An update is made for one pressing of the disc and carries the
    // localised fastfiles of that pressing's language alone, in the
    // language's folder. The folders and the three letters their files begin
    // with are the title's own (it asks for each language's
    // <letters>_code_post_gfx_mp.ff as it starts). The game runs in English:
    // the game disc has no other.
    struct UpdateFolder { std::string folder, letters; };
    const UpdateFolder& ForeignFolder()
    {
        static const UpdateFolder found = []() -> UpdateFolder
        {
            static constexpr const char* kLanguages[][2] = {
                { "french", "fra" }, { "german", "deu" }, { "italian", "ita" }, { "spanish", "spa" },
                { "british", "bri" }, { "russian", "rus" }, { "polish", "pol" }, { "korean", "kor" },
                { "taiwanese", "tai" }, { "japanese", "jpn" }, { "chinese", "chi" }, { "thai", "tha" },
                { "czech", "cze" }, { "spanishna", "sna" }, { "portuguese", "por" }, { "arabic", "ara" },
            };
            const auto& update = InstalledPackages().update;
            if (!update || update->Find("english")) return {};
            for (const auto& language : kLanguages)
                if (update->Find(language[0])) return { language[0], language[1] };
            return {};
        }();
        return found;
    }
}

// The executable is made from the disc's and the update's patch before the
// kernel starts (install.cpp), so the packages are looked for then, beside
// the game folder it names. No disc image is mounted yet: this title is not
// played from one.
bool kernel::ReadTitleUpdateFile(const fs::path& gameRoot, const std::string& name, std::vector<uint8_t>& bytes)
{
    const auto& update = InstalledPackages(&gameRoot).update;
    const ReadVolume::Entry* entry = update ? update->Find(name) : nullptr;
    if (!entry || entry->directory) return false;
    bytes.resize(size_t(entry->size));
    const int descriptor = update->Acquire();
    const size_t read = update->Read(descriptor, *entry, 0, bytes.data(), bytes.size());
    update->Release(descriptor);
    return read == bytes.size();
}

const std::string& kernel::UpdateLanguage() { return ForeignFolder().letters; }

bool kernel::UpdateHasZone(const std::string& zone)
{
    const auto& update = InstalledPackages().update;
    return update && !ForeignFolder().folder.empty() && update->Find(ForeignFolder().folder + "\\" + zone + ".ff");
}

// The console mounts a title update as `update:` before the title starts, and
// the title looks there first for every file of its own. This executable is
// the update's, and loads the update's fastfiles (patch_*.ff, the zombies' UI)
// on its way to the menu: without them its menus do not start.
void kernel::MountTitleUpdate()
{
    if (const auto& update = InstalledPackages().update)
    {
        // English the update would have brought and a pack has: the zombies'
        // text, which the title asks for before it mounts a pack
        // (english\eng_code_post_gfx_zm_mp.ff, in Compatibility Pack 1).
        std::vector<std::shared_ptr<ReadVolume>> english;
        if (!ForeignFolder().folder.empty())
            for (const Downloaded& pack : DownloadedContent())
                if (pack.volume->Find("english")) english.push_back(pack.volume);
        MountVolume("update", update, ForeignFolder().folder, std::move(english));
        LOGK("content: mounted update: on %s (the title update, read-only)", update->Name().c_str());
        if (!ForeignFolder().folder.empty())
            LOGI("content: the update is of another pressing than the disc: its localised fastfiles are %s's,"
                 " loaded for their tables and not their text (aw_update_language.cpp)", ForeignFolder().folder.c_str());
    }
    else
        LOGE("content: no title update's package was found beside the downloadable content; this is the"
             " update's executable and needs its files (docs/dlc.md)");
}
#endif

uint32_t kernel::ListDownloadedContent(uint32_t buffer, uint32_t bufferSize, size_t& next)
{
    const auto& content = DownloadedContent();
    uint32_t written = 0;
    for (; next < content.size() && (written + 1) * kContentDataSize <= bufferSize; next++, written++)
    {
        const uint32_t at = buffer + written * kContentDataSize;
        std::memset(GuestPtr<uint8_t>(at), 0, kContentDataSize);
        *GuestPtr<be32>(at + kContentDeviceId) = kSaveDeviceId;
        *GuestPtr<be32>(at + kContentType) = kMarketplaceContent;
        const std::u16string& display = content[next].volume->Header().displayName;
        auto* wide = GuestPtr<be16>(at + kContentDisplayName);
        for (size_t i = 0; i < display.size() && i + 1 < kContentDisplayMax; i++) wide[i] = uint16_t(display[i]);
        std::memcpy(GuestPtr<char>(at + kContentFileName), content[next].name.data(), content[next].name.size());
    }
    return written;
}

const fs::path& kernel::SaveRoot()
{
    // Beside the runtime, not inside the game root: the game root is the disc.
    static const fs::path root = fs::path("saves");
    return root;
}

// Memcard_InitializeSystem. The table is clear when it returns, so this is the
// moment to answer the question the prompt would ask.
#if !defined(MW3_TITLE) // T_Memcard_InitializeSystem uses MW2-only device-table offsets: MW2-only hook
GUEST_HOOK(T_Memcard_InitializeSystem)
{
    GUEST_ORIG(T_Memcard_InitializeSystem)(ctx, base);
    if (!ChooseDeviceForPlayer() || !kDeviceTable) return;
    for (uint32_t i = 0; i < kControllers; i++)
    {
        auto* chosen = GuestPtr<be32>(kDeviceTable + i * 4);
        if (chosen && uint32_t(*chosen) == 0) *chosen = kSaveDeviceId;
    }
    LOGK("content: the hard disk is the save device for every controller");
}

#endif

// Which storage devices exist. The title asks before it offers to save, and a
// "no" here is the save-device prompt.
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    const uint32_t deviceId = ctx.r3.u32;
    const uint32_t result = (deviceId == kSaveDeviceId) ? X_ERROR_SUCCESS
                                                        : X_ERROR_DEVICE_NOT_CONNECTED;
    CompleteOverlapped(ctx.r4.u32, result);
    ctx.r3.u64 = result;
}

// XDEVICE_DATA { id, type, totalBytes, freeBytes, friendlyName[28] }. The title
// compares the free space with the size it is about to write and refuses to
// save if it does not fit, so the numbers have to be real enough to pass.
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    const uint32_t deviceId = ctx.r3.u32;
    if (deviceId != kSaveDeviceId) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

    if (auto* data = GuestPtr<be32>(ctx.r4.u32))
    {
        constexpr uint64_t kTotal = 16ull * 1024 * 1024 * 1024;
        constexpr uint64_t kFree  =  8ull * 1024 * 1024 * 1024;
        std::memset(data, 0, 80);
        data[0] = kSaveDeviceId;
        data[1] = 1;                                   // XCONTENTDEVICETYPE_HDD
        *GuestPtr<be64>(ctx.r4.u32 + 8)  = kTotal;
        *GuestPtr<be64>(ctx.r4.u32 + 16) = kFree;
        auto* name = GuestPtr<be16>(ctx.r4.u32 + 24);  // 28 wide characters
        const char* friendly = "Hard Drive";
        for (int i = 0; friendly[i] && i < 27; i++) name[i] = uint16_t(friendly[i]);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// Mount a package as a drive. The disposition in the low nibble of the flags
// says whether it may be created, and the caller is told which of the two
// happened so it knows whether there is a save in there to read.
PPC_FUNC(__imp__XamContentCreateEx)
{
    const uint32_t rootName    = ctx.r4.u32;
    const uint32_t contentData = ctx.r5.u32;
    const uint32_t flags       = ctx.r6.u32;
    const uint32_t dispositionOut = ctx.r7.u32;
    const uint32_t licenseOut  = ctx.r8.u32;
    const uint32_t overlapped  = kernel::StackArgument(ctx, 8);

    const std::string drive = GuestString(rootName, 32);
    const std::string file  = SafeName(GuestString(contentData + kContentFileName, kContentFileMax));
    if (drive.empty() || file.empty() || !contentData)
    {
        CompleteOverlapped(overlapped, X_ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
        return;
    }

    const uint32_t type = *GuestPtr<be32>(contentData + kContentType);
    const uint32_t device = *GuestPtr<be32>(contentData + kContentDeviceId);
    if (type == kMarketplaceContent)
    {
        // A map pack: one of the packages listed, as a drive of its files.
        // The licence it reports is the package's own, every bit its header
        // grants: the console would match them against the profile and the
        // console the package was bought on, and neither is here.
        std::shared_ptr<ReadVolume> volume;
        for (const auto& [name, found] : DownloadedContent())
            if (name == file) volume = found;
        const uint32_t result = volume ? X_ERROR_SUCCESS : X_ERROR_PATH_NOT_FOUND;
        if (volume)
        {
            MountVolume(drive, volume);
            LOGK("content: mounted %s: on %s (downloadable content, read-only)", drive.c_str(), volume->Name().c_str());
        }
        else LOGK("content: there is no downloadable content named %s (%s: asked to open it)", file.c_str(), drive.c_str());
        if (auto* d = GuestPtr<be32>(dispositionOut)) *d = 2u;      // XCONTENT_EXISTING
        if (auto* l = GuestPtr<be32>(licenseOut)) *l = volume ? volume->Header().licenceBits : 0u;
        CompleteOverlapped(overlapped, result);
        ctx.r3.u64 = result;
        return;
    }
    const fs::path package = SaveRoot() / file;
    std::error_code ec;
    const bool existed = fs::is_directory(package, ec);

    // "Create always" and "truncate" both mean the package the title gets is a
    // new empty one. Keeping the old contents instead is not a smaller version
    // of that: the title saves by creating its file with CREATE_NEW, so a
    // leftover file from the last save collides and the save fails.
    const uint32_t disposition = flags & 0xF;
    const bool clear = disposition == kCreateAlways || disposition == kTruncate;

    uint32_t result = X_ERROR_SUCCESS;
    switch (disposition)
    {
    case kCreateNew:    if (existed) result = X_ERROR_ALREADY_EXISTS; break;
    case kOpenExisting:
    case kTruncate:     if (!existed) result = X_ERROR_PATH_NOT_FOUND; break;
    case kCreateAlways:
    case kOpenAlways:
    default:            break;
    }
    if (!result && clear && existed) RotatePackage(package);
    if (!result && !fs::create_directories(package, ec) && !fs::is_directory(package, ec))
        result = X_ERROR_PATH_NOT_FOUND;

    if (!result)
    {
        MountDevice(drive, package);
        LOGK("content: mounted %s: on %s (device %u, type %u, %s)", drive.c_str(),
             package.string().c_str(), device, type,
             !existed ? "new" : clear ? "emptied" : "existing");
    }
    // Asked to open a package nobody has saved yet: the first look for a new
    // player's stats, or for a save before the first.
    else if (result == X_ERROR_PATH_NOT_FOUND && !existed)
        LOGK("content: there is no %s yet (%s: asked to open it)", package.string().c_str(), drive.c_str());
    else
    {
        LOGK("content: refused %s: on %s -- error %u", drive.c_str(),
             package.string().c_str(), result);
    }

    // XCONTENT_NEW / XCONTENT_EXISTING, and a licence mask of nothing, which is
    // what a package made on this console rather than bought carries.
    if (auto* d = GuestPtr<be32>(dispositionOut)) *d = existed ? 2u : 1u;
    if (auto* l = GuestPtr<be32>(licenseOut)) *l = 0;
    CompleteOverlapped(overlapped, result);
    ctx.r3.u64 = result;
}

PPC_FUNC(__imp__XamContentClose)
{
    const std::string drive = GuestString(ctx.r3.u32, 32);
    if (!drive.empty()) UnmountDevice(drive);
    CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__XamContentDelete)
{
    const uint32_t contentData = ctx.r4.u32;
    const std::string file = SafeName(GuestString(contentData + kContentFileName, kContentFileMax));
    uint32_t result = X_ERROR_FILE_NOT_FOUND;
    if (!file.empty())
    {
        std::error_code ec;
        const fs::path package = SaveRoot() / file;
        if (fs::is_directory(package, ec))
        {
            fs::remove_all(package, ec);
            result = ec ? X_ERROR_PATH_NOT_FOUND : X_ERROR_SUCCESS;
            // The player asked for this save to be gone, so the copy
            // RotatePackage kept goes with it -- leaving it would be a save
            // that is still on disk after being deleted.
            if (!result)
            {
                std::error_code also;
                fs::path backup = package;
                backup += ".bak";
                fs::remove_all(backup, also);
                LOGK("content: deleted %s", package.string().c_str());
            }
        }
    }
    CompleteOverlapped(ctx.r5.u32, result);
    ctx.r3.u64 = result;
}

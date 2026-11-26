#include "install.h"
#include "crypto.h"
#include "xex.h"
#include "spawn.h"
#include "../log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef MW2_USE_SDL
#include <SDL3/SDL.h>
#endif
#ifdef MW2_HAVE_XEX_PATCHER
#include <xex_patcher.h>
#endif
#if defined(AW_TITLE_TU17) || defined(BO3_TITLE)
#include "../kernel/kernel.h"
#endif
#ifdef MW2_ANDROID
#include "../android/android.h"
#include "../kernel/volume.h"
#include "../../launcher/disc.h"
#include "../../launcher/package.h"
// A TU6 build configured without the update's hashes (no mw2/update/ and no
// -DMW2_UPDATE_SHA256_*): ApplyTu6 refuses at run time rather than trusting
// any bytes, and this keeps it compiling.
#ifndef MW2_UPDATE_SHA256_PATCH_MP
#define MW2_UPDATE_SHA256_PATCH_MP ""
#endif
#ifndef MW2_UPDATE_SHA256_DLC1_UI_MP
#define MW2_UPDATE_SHA256_DLC1_UI_MP ""
#endif
#ifndef MW2_UPDATE_SHA256_DLC2_UI_MP
#define MW2_UPDATE_SHA256_DLC2_UI_MP ""
#endif
#endif

namespace fs = std::filesystem;

// The executable this build was recompiled from, by its SHA-256, which CMake
// reads from mw2/ at configure time. Any other -- another region, another
// version -- has other code at other addresses, so it is not run.
namespace
{
#if defined(BO3_TITLE_ZM)
    constexpr const char* kXex = "default_zm.xex";
    constexpr const char* kSha256 = MW2_XEX_SHA256_MP;
#elif defined(MW2_TITLE_MP) || defined(MW3_TITLE_MP) || defined(COD4_TITLE_MP) || defined(AW_TITLE_MP)
    constexpr const char* kXex = "default_mp.xex";
    constexpr const char* kSha256 = MW2_XEX_SHA256_MP;
#else
    constexpr const char* kXex = "default.xex";
    constexpr const char* kSha256 = MW2_XEX_SHA256_SP;
#endif
    constexpr const char* kGameFolder = "game";
    std::filesystem::path g_gameFolder = "game";
#ifdef _WIN32
    constexpr const char* kLauncher = "mw2-launcher.exe";
#else
    constexpr const char* kLauncher = "mw2-launcher";
#endif

    // Paths as UTF-8, which SDL and the log take, whatever the system's code page.
    std::string Utf8(const fs::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path ExecutableFolder()
    {
#ifdef _WIN32
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        return fs::path(std::wstring(path, length)).parent_path();
#else
        std::error_code ec;
        return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
    }

    void ShowError(const std::string& text)
    {
        LOGE("%s", text.c_str());
        std::fprintf(stderr, "%s\n", text.c_str());
#ifdef MW2_USE_SDL
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Modern Warfare 2", text.c_str(), nullptr);
#endif
#ifdef MW2_ANDROID
        android::SetStatus(text.c_str());
#endif
    }

    // The launcher beside this executable, left running on its own.
    bool StartLauncher(const std::vector<std::string>& arguments = {})
    {
#ifdef MW2_USE_SDL
        std::error_code ec;
        const fs::path launcher = fs::absolute(kLauncher, ec);
        if (!fs::is_regular_file(launcher, ec)) return false;
        const std::string path = Utf8(launcher);
        std::vector<const char*> args{ path.c_str() };
        for (const std::string& argument : arguments) args.push_back(argument.c_str());
        args.push_back(nullptr);
        SDL_Process* process = spawn::Start(args.data());
        if (!process) { LOGE("cannot start %s: %s", path.c_str(), SDL_GetError()); return false; }
        SDL_DestroyProcess(process);    // forgets it; the launcher keeps running
        return true;
#else
        (void)arguments;
        return false;
#endif
    }

    // The program of each of the title's executables, beside this one.
    const char* ProgramFor(const std::string& xex)
    {
#ifdef _WIN32
        if (xex == "default.xex") return "mw2-sp.exe";
        if (xex == "default_mp.xex") return "mw2-mp.exe";
#else
        if (xex == "default.xex") return "mw2-sp";
        if (xex == "default_mp.xex") return "mw2-mp";
#endif
        return nullptr;
    }

    constexpr const char* kLaunchDataVariable = "MW2_LAUNCH_DATA";
    bool g_playerStart = false;
    std::string g_nextTitle;

    enum class Installed { Yes, No, Unreadable };

#if defined(AW_TITLE)
    // The image of Advanced Warfare's multiplayer as title update 17 makes
    // it, the one awmp17 is recompiled from, by its SHA-256: updates of two
    // pressings (base versions 0x10 and 0x15) make the same image and differ
    // in the header.
    constexpr const char* kUpdateImageSha256 = "9112629a870d860e17f4f30abe3c1c60ae2919d9697e58ddf12552ab74efaf55";
#elif defined(BO3_TITLE_ZM)
    // Black Ops III's two executables as title update 5 makes them, which
    // are the only ones recompiled: the disc's own refuse the fastfiles the
    // game was completed with (docs/BO3-BRINGUP.md).
    constexpr const char* kUpdateImageSha256 = "c30f6d88a19390dea7515442289d42188b866e327098eddede1785ec536c86f8";
#elif defined(BO3_TITLE)
    constexpr const char* kUpdateImageSha256 = "1368d16af20790dd735b82edf700fdf2dfacdbb42f4755fc55f62938fd31d454";
#endif

#if defined(AW_TITLE_TU17) || defined(BO3_TITLE)
#define MW2_UPDATE_MADE_EXECUTABLE 1
#if defined(BO3_TITLE_ZM)
    constexpr const char* kUpdatePatch = "default_zm.xexp";
    constexpr const char* kUpdateName = "title update 5";
    constexpr const char* kUpdateMissing = "Black Ops III needs its title update 5, and no update package is in the dlc folder.";
#elif defined(BO3_TITLE)
    constexpr const char* kUpdatePatch = "default.xexp";
    constexpr const char* kUpdateName = "title update 5";
    constexpr const char* kUpdateMissing = "Black Ops III needs its title update 5, and no update package is in the dlc folder.";
#else
    constexpr const char* kUpdatePatch = "default_mp.xexp";
    constexpr const char* kUpdateName = "title update 17";
    constexpr const char* kUpdateMissing = "The map packs' multiplayer needs the game's title update, and no update package is in the dlc folder (docs/dlc.md).";
#endif
    // The game folder has the disc's executable and the dlc folder the
    // player's own update package; the update's executable is the first with
    // the second's patch applied, made here at every start and kept nowhere.
    bool ApplyTitleUpdate(std::vector<uint8_t>& file, const fs::path& gameRoot, std::string& error)
    {
        std::vector<uint8_t> patch, updated;
        if (!kernel::ReadTitleUpdateFile(gameRoot, kUpdatePatch, patch))
        {
            error = kUpdateMissing;
            return false;
        }
        if (XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), updated, false) != XexPatcher::Result::Success)
        {
            error = std::string("The title update in the dlc folder does not apply to this disc's ") + kXex + ".";
            return false;
        }
        file = std::move(updated);
        return true;
    }

    bool IsUpdateImage(const std::vector<uint8_t>& image, std::string& error)
    {
        install::crypto::Sha256 hash;
        hash.Update(image.data(), image.size());
        if (hash.FinalHex() == kUpdateImageSha256) return true;
        error = std::string("the title update in the dlc folder makes another executable than the one this build is for (") +
                kUpdateName + "'s)";
        return false;
    }
#endif

    // The title's image from game/, if the executable there is the one this
    // build was made from.
    Installed LoadInstalled(std::vector<uint8_t>& image, std::vector<uint8_t>& xexHeader, std::string& error)
    {
        const fs::path path = fs::path(g_gameFolder) / kXex;
        std::vector<uint8_t> file;
        {
            std::ifstream in(path, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (file.empty()) return Installed::No;
        install::crypto::Sha256 hash;
        hash.Update(file.data(), file.size());
        if (hash.FinalHex() != kSha256) return Installed::No;
#if defined(MW2_UPDATE_MADE_EXECUTABLE)
        if (!ApplyTitleUpdate(file, g_gameFolder, error)) return Installed::Unreadable;
#endif
        if (!install::xex::Header(file, xexHeader, error) ||
#if defined(MW2_UPDATE_MADE_EXECUTABLE)
            !install::xex::Image(file, image, error) || !IsUpdateImage(image, error))
#else
            !install::xex::Image(file, image, error))
#endif
        {
            error = Utf8(path) + ": " + error;
            return Installed::Unreadable;
        }
        return Installed::Yes;
    }

#ifdef MW2_ANDROID
    struct Title { const char* xex; const char* sha256; const char* disc; };
#ifdef MW3_TITLE
#if defined(BO3_TITLE)
    constexpr uint32_t kTitleId = 0x4156091D;
    constexpr const char* kTitleName = "Call of Duty: Black Ops III";
#elif defined(AW_TITLE)
    constexpr uint32_t kTitleId = 0x41560914;
    constexpr const char* kTitleName = "Call of Duty: Advanced Warfare";
#elif defined(COD4_TITLE)
    constexpr uint32_t kTitleId = 0x415607E6;
    constexpr const char* kTitleName = "Call of Duty 4: Modern Warfare";
#else
    constexpr uint32_t kTitleId = 0x415608CB;
    constexpr const char* kTitleName = "Modern Warfare 3";
#endif
#ifdef MW2_BOTH_TITLES
    // The app that carries both titles: one install puts both of the disc's
    // executables in the one game folder, each checked, so the folder either
    // runs the campaign and the multiplayer or is not installed.
    constexpr bool kEveryTitleNeeded = true;
    constexpr Title kTitles[] = {
        { "default.xex", MW2_XEX_SHA256_SP, MW2_DISC_SHA256_SP },
#if defined(BO3_TITLE)
        // Its second executable is the zombies'.
        { "default_zm.xex", MW2_XEX_SHA256_MP, MW2_DISC_SHA256_MP },
#else
        { "default_mp.xex", MW2_XEX_SHA256_MP, MW2_DISC_SHA256_MP },
#endif
    };
#else
    // MW3, an app of one title: the campaign's executable (default.xex, with
    // Special Ops) or the multiplayer's (default_mp.xex); the other is
    // neither checked nor copied.
    constexpr bool kEveryTitleNeeded = false;
    constexpr Title kTitles[] = {
#if defined(MW3_TITLE_MP) || defined(COD4_TITLE_MP) || defined(AW_TITLE_MP)
        { "default_mp.xex", MW2_XEX_SHA256_MP, MW2_DISC_SHA256_MP },
#else
        { "default.xex", MW2_XEX_SHA256_SP, MW2_DISC_SHA256_SP },
#endif
    };
#endif
#else
    constexpr uint32_t kTitleId = 0x41560817;
    constexpr const char* kTitleName = "Modern Warfare 2";
    // The app that carries both titles needs both, as Modern Warfare 3's
    // does; an app of one title takes the other's executable when the disc
    // has it and does without when not.
#ifdef MW2_BOTH_TITLES
    constexpr bool kEveryTitleNeeded = true;
#else
    constexpr bool kEveryTitleNeeded = false;
#endif
    constexpr Title kTitles[] = {
        { "default.xex", MW2_XEX_SHA256_SP, MW2_DISC_SHA256_SP },
        { "default_mp.xex", MW2_XEX_SHA256_MP, MW2_DISC_SHA256_MP },
    };
#endif

    std::string Sha256(const std::vector<uint8_t>& bytes)
    {
        install::crypto::Sha256 hash;
        hash.Update(bytes.data(), bytes.size());
        return hash.FinalHex();
    }

    std::string WrongDisc(const std::string& name, const std::vector<uint8_t>& file)
    {
        install::xex::Info info;
        if (!install::xex::ReadInfo(file, info))
            return name + " is not an Xbox 360 executable.";
        char text[320];
        if (info.titleId != kTitleId)
            std::snprintf(text, sizeof(text), "This is not %s (title %08X).", kTitleName, info.titleId);
        else
#if defined(MW3_TITLE)
            std::snprintf(text, sizeof(text),
                          "This is %s, but not a disc this build was made from (executable "
                          "version %u). Install from an image of the retail disc itself.",
                          kTitleName, info.version);
#elif defined(MW2_VERSION_TU0)
            std::snprintf(text, sizeof(text),
                          "This is %s, but not the version this build was made from "
                          "(executable version %u, expected the disc's 10). A title update cannot be "
                          "used: install from an image of the disc itself.", kTitleName, info.version);
#else
            std::snprintf(text, sizeof(text),
                          "This is %s, but not the disc this build installs from "
                          "(executable version %u, expected the disc's 10). Install from an image of "
                          "the disc itself; the update is applied afterwards.", kTitleName, info.version);
#endif
        return text;
    }

    bool IsGameFile(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        auto endsWith = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        // .bikbe: Call of Duty 4's two movies shown on a screen inside a level.
        return endsWith(".ff") || endsWith(".pak") || endsWith(".bik") || endsWith(".bikbe") || lower == "nxeart";
    }

    // The one folder of the disc a title reads besides its root, and the
    // files of it that are the game's. Call of Duty 4's high mips: the top mip
    // of each streamed texture, a file each in the disc's highmip folder,
    // which the title opens as D:\highmip\<name>.hi; without them it draws
    // the lower mips. Advanced Warfare's language: the fastfiles, sound packs
    // and movies it opens as D:\english\<name>, and does not start without.
#ifdef AW_TITLE
    constexpr const char* kDiscFolder = "english";
#else
    constexpr const char* kDiscFolder = "highmip";
#endif
    bool IsFolderFile(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        const std::string prefix = std::string(kDiscFolder) + "/";
        if (lower.size() <= prefix.size() + 3 || lower.compare(0, prefix.size(), prefix) != 0) return false;
        const std::string file = lower.substr(prefix.size());
        if (file.find_first_of("/\\:") != std::string::npos || file == "." || file == "..") return false;
#ifdef AW_TITLE
        return IsGameFile(file);
#else
        return file.compare(file.size() - 3, 3, ".hi") == 0;
#endif
    }

#ifdef AW_TITLE
    // The install disc's two content packages, which lie in the image where
    // a console's hard disk has them after the install, and go beside the
    // game folder, where the runtime looks for a title's installed content
    // (runtime/kernel/content.cpp).
    constexpr const char* kPackagePath = "content/0000000000000000/41560914/00000002/";
    constexpr const char* kPackageFolder = "dlc";
    bool IsPackage(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        const size_t prefix = std::strlen(kPackagePath);
        return lower.size() > prefix && lower.compare(0, prefix, kPackagePath) == 0 &&
               lower.find_first_of("/\\:", prefix) == std::string::npos;
    }
#endif

    struct Progress
    {
        std::mutex lock;
        std::string file;
        uint64_t done = 0, total = 0;
        std::atomic<bool> cancel{ false };
    };

    // The picture of the game the launcher shows behind its menu (DiscArt.kt).
    bool IsArt(const std::string& name)
    {
        return name.size() == 6 && std::equal(name.begin(), name.end(), "nxeart",
                                              [](char a, char b) { return std::tolower(uint8_t(a)) == b; });
    }

    // What an install copies: a file of the source and where it goes.
    struct Wanted
    {
        const install::Source::File* file = nullptr;
        fs::path to;
    };

    // Whether the file is at `to` already, byte for byte.
    bool AlreadyThere(install::Source& source, const install::Source::File& file, const fs::path& to)
    {
        std::error_code ec;
        if (!fs::is_regular_file(to, ec) || fs::file_size(to, ec) != file.size) return false;
        std::ifstream in(to, std::ios::binary);
        std::vector<char> bufDisk(64 << 10), bufSrc(64 << 10);
        for (uint64_t at = 0; at < file.size; at += bufDisk.size())
        {
            const size_t checkLen = size_t(std::min<uint64_t>(bufDisk.size(), file.size - at));
            if (!in.read(bufDisk.data(), checkLen) || size_t(in.gcount()) != checkLen ||
                !source.Read(file, at, bufSrc.data(), checkLen) ||
                std::memcmp(bufDisk.data(), bufSrc.data(), checkLen) != 0)
                return false;
        }
        return true;
    }

    // Copies what is not there yet, each file under a .part name until it is whole.
    bool Copy(install::Source& source, const fs::path& from, const std::vector<Wanted>& wanted,
              const fs::path& folder, Progress& progress, std::string& error)
    {
        std::error_code ec;
        uint64_t needed = 0;
        std::vector<const Wanted*> copy;
        for (const Wanted& item : wanted)
        {
            if (AlreadyThere(source, *item.file, item.to)) continue;
            copy.push_back(&item);
            needed += item.file->size;
        }
        {
            std::lock_guard lock(progress.lock);
            progress.total = needed;
        }
        const auto space = fs::space(folder, ec);
        if (!ec && space.available < needed)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Not enough disk space: %.1f GB needed, %.1f GB free.",
                          needed / 1e9, space.available / 1e9);
            error = text;
            return false;
        }

        std::vector<uint8_t> buffer(8 << 20);
        for (const Wanted* item : copy)
        {
            const install::Source::File* file = item->file;
            {
                std::lock_guard lock(progress.lock);
                progress.file = file->name;
            }
            const fs::path to = item->to, partial = fs::path(to) += ".part";

            struct PartCleaner
            {
                fs::path target;
                bool disarmed = false;
                ~PartCleaner()
                {
                    if (!disarmed && !target.empty())
                    {
                        std::error_code ec;
                        fs::remove(target, ec);
                    }
                }
            } cleaner{ partial };

            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) { error = "Cannot write " + Utf8(fs::absolute(partial)); return false; }
            for (uint64_t at = 0; at < file->size;)
            {
                if (progress.cancel) { out.close(); error = "cancelled"; return false; }
                const size_t take = size_t(std::min<uint64_t>(buffer.size(), file->size - at));
                if (!source.Read(*file, at, buffer.data(), take))
                {
                    out.close();
                    error = "Could not read " + file->name + " from " + Utf8(from) + ".";
                    return false;
                }
                out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(take));
                if (!out)
                {
                    out.close();
                    error = "Could not write " + Utf8(fs::absolute(partial)) + " (disk full?).";
                    return false;
                }
                at += take;
                std::lock_guard lock(progress.lock);
                progress.done += take;
            }
            out.close();
            fs::rename(partial, to, ec);
            if (ec) { error = "Cannot rename " + Utf8(partial) + ": " + ec.message(); return false; }
            cleaner.disarmed = true;
        }
        return true;
    }

    bool Install(const fs::path& from, Progress& progress, std::string& error, bool link)
    {
        auto source = install::Source::Open(from, error);
        if (!source) return false;

#ifdef AW_TITLE
        // The install disc, told by its packages: they are all it gives, and
        // its executable is the installer's, which no title here is.
        {
            const fs::path packages = fs::path(g_gameFolder).parent_path() / kPackageFolder;
            std::vector<Wanted> wanted;
            for (const auto& file : source->DeepFiles())
                if (IsPackage(file.name))
                    wanted.push_back({ &file, packages / install::FromUtf8(file.name.substr(std::strlen(kPackagePath))) });
            if (!wanted.empty())
            {
                // Played from the image, the packages stay in it, where the
                // runtime reads them (kernel/content.cpp): nothing is copied,
                // and each is only seen to be a package.
                if (link)
                {
                    for (const Wanted& item : wanted)
                    {
                        char magic[4];
                        if (item.file->size < sizeof magic || !source->Read(*item.file, 0, magic, sizeof magic) ||
                            (std::memcmp(magic, "LIVE", 4) != 0 && std::memcmp(magic, "PIRS", 4) != 0))
                        {
                            error = "The install disc's image is damaged: " + item.file->name + " is not a content package.";
                            return false;
                        }
                    }
                    return true;
                }
                std::error_code ec;
                fs::create_directories(packages, ec);
                if (ec) { error = "Cannot create " + Utf8(fs::absolute(packages)) + ": " + ec.message(); return false; }
                return Copy(*source, from, wanted, packages, progress, error);
            }
        }
#endif

        std::vector<const install::Source::File*> files;
        for (const Title& title : kTitles)
        {
            const auto* file = source->Find(title.xex);
            if (!file)
            {
                if (kEveryTitleNeeded || std::string(title.xex) == kXex)
                {
                    error = std::string("There is no ") + title.xex + " in " + Utf8(from) +
                            ": it is not a " + kTitleName + " disc.";
                    return false;
                }
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!source->ReadAll(*file, bytes))
            {
                error = std::string("Could not read ") + title.xex + " from " + Utf8(from);
                return false;
            }
            // A TU6 build installs from the disc's executables (the update is
            // applied afterwards by ApplyTu6); a TU0 build installs what it
            // was recompiled from.
#ifdef MW2_VERSION_TU0
            if (Sha256(bytes) != title.sha256) { error = WrongDisc(title.xex, bytes); return false; }
#else
            if (Sha256(bytes) != title.disc) { error = WrongDisc(title.xex, bytes); return false; }
#endif
        }
        // Played from the image, the game folder has the executables and the
        // picture; everything else stays in the image.
        for (const auto& file : source->Files())
            if (link ? IsArt(file.name) : IsGameFile(file.name)) files.push_back(&file);
        if (!link)
            for (const auto& file : source->FolderFiles())
                if (IsFolderFile(file.name)) files.push_back(&file);
        for (const Title& title : kTitles)
            if (const auto* file = source->Find(title.xex)) files.push_back(file);

        const fs::path folder = g_gameFolder;
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec) { error = "Cannot create " + Utf8(fs::absolute(folder)) + ": " + ec.message(); return false; }

        std::vector<Wanted> wanted;
        const fs::path canonicalFolder = folder.lexically_normal();
        const fs::path discFolder = (folder / kDiscFolder).lexically_normal();
        for (const auto* file : files)
        {
            // One file in the game folder, or one in its folder of the disc's
            // (kDiscFolder): a name with any other path in it is not copied.
            const bool inFolder = IsFolderFile(file->name);
            if (file->name.empty() || file->name == "." || file->name == ".." ||
                file->name.find('\\') != std::string::npos ||
                (!inFolder && file->name.find('/') != std::string::npos))
                continue;
            const fs::path to = (folder / install::FromUtf8(file->name)).lexically_normal();
            if (to.parent_path() != (inFolder ? discFolder : canonicalFolder)) continue;
            if (inFolder)
            {
                fs::create_directories(discFolder, ec);
                if (ec) { error = "Cannot create " + Utf8(fs::absolute(discFolder)) + ": " + ec.message(); return false; }
            }
            wanted.push_back({ file, folder / install::FromUtf8(file->name) });
        }
        return Copy(*source, from, wanted, folder, progress, error);
    }

    std::mutex g_installLock;
    Progress* g_installing = nullptr;
#endif
}

#ifdef MW2_ANDROID
namespace install
{
    namespace
    {
        std::string TuSha256(const std::vector<uint8_t>& bytes)
        {
            install::crypto::Sha256 hash;
            hash.Update(bytes.data(), bytes.size());
            return hash.FinalHex();
        }

        // Through a temporary name, so a file present is a file written whole.
        bool TuWriteWhole(const fs::path& to, const std::vector<uint8_t>& bytes, std::string& error)
        {
            const fs::path partial = fs::path(to) += ".part";
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            std::error_code ec;
            if (out) fs::rename(partial, to, ec);
            if (!out || ec) { error = "Cannot write " + install::Utf8(fs::absolute(to)) + " (disk full?)."; return false; }
            return true;
        }
    }

    // Applies downloaded title update 6 to an installed disc (TU6 builds):
    // patches both executables in memory exactly as the PC launcher's install
    // does (launcher/setup.cpp), then lays the update's fastfiles beside the
    // disc's. Executables go last: both present and patched is what an update
    // that finished looks like.
    bool ApplyTu6(const fs::path& package, std::string& error)
    {
#ifdef MW2_VERSION_TU0
        (void)package;
        error = "This build is the disc version; it needs no update.";
        return false;
#else
#ifndef MW2_HAVE_XEX_PATCHER
        (void)package;
        error = "This APK was built without the title-update patcher; install a CI-built TU6 APK.";
        return false;
#else
        static const char* kUpdateHashes[][2] = {
            { "patch_mp.ff", MW2_UPDATE_SHA256_PATCH_MP },
            { "dlc1_ui_mp.ff", MW2_UPDATE_SHA256_DLC1_UI_MP },
            { "dlc2_ui_mp.ff", MW2_UPDATE_SHA256_DLC2_UI_MP },
        };
        for (const auto& [name, hash] : kUpdateHashes)
            if (!*hash)
            {
                error = "This build does not know title update 6's files; install a CI-built TU6 APK.";
                return false;
            }
        std::vector<uint8_t> packed;
        {
            std::ifstream in(package, std::ios::binary);
            if (!in) { error = "Cannot read the downloaded update."; return false; }
            packed.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::map<std::string, std::vector<uint8_t>> files;
        std::string why;
        if (!install::ReadPackage(packed, files, why)) { error = "The downloaded update: " + why + "."; return false; }
        for (const auto& [name, hash] : kUpdateHashes)
        {
            const auto it = files.find(name);
            if (it == files.end() || TuSha256(it->second) != hash)
            {
                error = std::string("This is not title update 6: its ") + name + " is missing or another version's.";
                return false;
            }
        }
        const char* kPatches[][3] = {
            { "default.xex", "default.xexp", MW2_XEX_SHA256_SP },
            { "default_mp.xex", "default_mp.xexp", MW2_XEX_SHA256_MP },
        };
        const fs::path folder = install::GameFolder();
        std::vector<uint8_t> patched[2];
        for (int i = 0; i < 2; i++)
        {
            const auto px = files.find(kPatches[i][1]);
            if (px == files.end() || px->second.empty())
            {
                error = std::string("This is not title update 6: its ") + kPatches[i][1] + " is missing.";
                return false;
            }
            std::vector<uint8_t> base;
            {
                std::ifstream in(folder / kPatches[i][0], std::ios::binary);
                if (!in) { error = "Install the disc first: its executables are not here yet."; return false; }
                base.assign(std::istreambuf_iterator<char>(in), {});
            }
            const auto result = XexPatcher::apply(base.data(), base.size(), px->second.data(),
                                                  px->second.size(), patched[i], false);
            if (result != XexPatcher::Result::Success || TuSha256(patched[i]) != kPatches[i][2])
            {
                error = std::string("This is not title update 6: its ") + kPatches[i][1] +
                        " does not make the expected executable.";
                return false;
            }
        }
        for (const auto& [name, hash] : kUpdateHashes)
            if (!TuWriteWhole(folder / name, files[name], error)) return false;
        for (int i = 0; i < 2; i++)
            if (!TuWriteWhole(folder / kPatches[i][0], patched[i], error)) return false;
        LOGI("install: title update 6 applied");
        return true;
#endif
#endif
    }

#if (defined(AW_TITLE) || defined(BO3_TITLE)) && defined(MW2_HAVE_XEX_PATCHER)
    // The file the recompiler needs to build an update-made title's game
    // code on the device: what the runtime makes in memory at every start
    // (ApplyTitleUpdate), written out once. The patch is the one of the
    // executable's name (default_mp.xexp for default_mp.xex). Black Ops
    // III's app builds both of its titles through the library of the first,
    // so the zombies' image is known here by its name as well.
    bool MakeUpdatedExecutable(const fs::path& package, const fs::path& disc, const fs::path& to, std::string& error)
    {
        const std::string name = Utf8(to.filename());
        const std::string patchName = name + "p";
#if defined(BO3_TITLE)
        const char* expected = name == "default_zm.xex"
            ? "c30f6d88a19390dea7515442289d42188b866e327098eddede1785ec536c86f8"
            : "1368d16af20790dd735b82edf700fdf2dfacdbb42f4755fc55f62938fd31d454";
        const char* update = "title update 5";
#else
        const char* expected = kUpdateImageSha256;
        const char* update = "title update 17";
#endif
        std::vector<uint8_t> base, patch, updated, image;
        {
            std::ifstream in(disc, std::ios::binary);
            base.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (base.empty()) { error = "The disc's " + name + " is not here yet."; return false; }
        std::string why;
        const auto volume = kernel::ReadVolume::OpenPackage(package, why);
        const kernel::ReadVolume::Entry* entry = volume ? volume->Find(patchName) : nullptr;
        if (!entry || entry->directory)
        {
            error = "The title update's package has no " + patchName + (why.empty() ? std::string(".") : ": " + why + ".");
            return false;
        }
        patch.resize(size_t(entry->size));
        const int descriptor = volume->Acquire();
        const size_t read = volume->Read(descriptor, *entry, 0, patch.data(), patch.size());
        volume->Release(descriptor);
        if (read != patch.size()) { error = "The title update's package could not be read to its end."; return false; }
        if (XexPatcher::apply(base.data(), base.size(), patch.data(), patch.size(), updated, false) != XexPatcher::Result::Success)
        {
            error = "This title update does not apply to this disc's " + name + ".";
            return false;
        }
        if (!install::xex::Image(updated, image, why) || TuSha256(image) != expected)
        {
            error = std::string("This title update makes another executable than the one this app is for (") + update + "'s).";
            return false;
        }
        return TuWriteWhole(to, updated, error);
    }
#endif
}
#endif

bool install::PlayerStart(int argc, char**)
{
    return argc == 1;
}

fs::path install::Folder(int argc, char** argv)
{
    return PlayerStart(argc, argv) ? ExecutableFolder() : fs::path();
}

bool install::Prepare(int, char**, Launch& launch, int& exitCode)
{
    exitCode = 1;
    g_playerStart = true;
    // Everything a player's copy keeps -- game/, saves/ -- is beside the executable.
    std::error_code ec;
    if (const fs::path folder = ExecutableFolder(); !folder.empty()) fs::current_path(folder, ec);
    // ...unless the launcher put the game files on another drive (INSTALL
    // LOCATION): same file it wrote, beside this program too.
    if (const fs::path at = InstallLocation(ExecutableFolder()); !at.empty())
        SetGameFolder(at / kGameFolder);

    std::string error;
    switch (LoadInstalled(launch.image, launch.xexHeader, error))
    {
    case Installed::Yes:
        launch.gameRoot = g_gameFolder;
        return true;
    case Installed::Unreadable:
        ShowError(error);
        return false;
    case Installed::No:
        break;
    }
    // Not installed, or installed for another version of this build: both are
    // the launcher's to put right.
    if (StartLauncher()) { exitCode = 0; return false; }
    ShowError(std::string("The game is not installed here, and ") + kLauncher +
              " is not beside this program to install it.");
    return false;
}

bool install::LoadImageFile(const fs::path& path, std::vector<uint8_t>& image, std::vector<uint8_t>& xexHeader)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { LOGE("cannot open image %s", Utf8(path).c_str()); return false; }
    std::vector<uint8_t> file(std::istreambuf_iterator<char>(in), {});
    if (file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0)
    {
        std::string error;
#if defined(MW2_UPDATE_MADE_EXECUTABLE)
        // The disc's executable, in its game folder: the update is applied.
        // One that is the update's already is taken as it is.
        crypto::Sha256 hash;
        hash.Update(file.data(), file.size());
        if (hash.FinalHex() == kSha256 && !ApplyTitleUpdate(file, fs::absolute(path).parent_path(), error))
        {
            LOGE("%s: %s", Utf8(path).c_str(), error.c_str());
            return false;
        }
        if (!xex::Header(file, xexHeader, error) || !xex::Image(file, image, error) || !IsUpdateImage(image, error))
#else
        if (!xex::Header(file, xexHeader, error) || !xex::Image(file, image, error))
#endif
        {
            LOGE("%s: %s", Utf8(path).c_str(), error.c_str());
            return false;
        }
        return true;
    }
    xexHeader.clear();
    image = std::move(file);
    return true;
}

void install::SetGameFolder(const fs::path& folder)
{
    g_gameFolder = folder;
    LOGI("install: the game folder is %s", Utf8(fs::absolute(folder)).c_str());
}

const fs::path& install::GameFolder() { return g_gameFolder; }

bool install::Installed()
{
    std::error_code ec;
    return fs::is_regular_file(g_gameFolder / kXex, ec);
}

void install::CancelInstall()
{
#ifdef MW2_ANDROID
    std::lock_guard lock(g_installLock);
    if (g_installing) g_installing->cancel = true;
#endif
}

bool install::InstallFrom(const fs::path& from, std::string& error, Report report, void* user, bool link)
{
#ifdef MW2_ANDROID
    Progress progress;
    {
        std::lock_guard lock(g_installLock);
        if (g_installing) { error = "An install is already running."; return false; }
        g_installing = &progress;
    }

    std::atomic<bool> finished{ false };
    std::thread reporter;
    if (report)
        reporter = std::thread([&] {
            while (!finished.load(std::memory_order_acquire))
            {
                {
                    std::lock_guard lock(progress.lock);
                    report(progress.file.c_str(), progress.done, progress.total, user);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::lock_guard lock(progress.lock);
            report(progress.file.c_str(), progress.done, progress.total, user);
        });

    const bool ok = Install(from, progress, error, link);
    finished.store(true, std::memory_order_release);
    if (reporter.joinable()) reporter.join();
    {
        std::lock_guard lock(g_installLock);
        g_installing = nullptr;
    }
    if (!ok && error.empty()) error = "The game could not be installed.";
    if (ok) LOGI("install: done, into %s", Utf8(fs::absolute(g_gameFolder)).c_str());
    else    LOGE("install: %s", error.c_str());
    return ok;
#else
    (void)link;
    error = "Not implemented on desktop";
    return false;
#endif
}

uint64_t install::CopiedFiles(const fs::path& from, bool remove)
{
    uint64_t bytes = 0;
#ifdef MW2_ANDROID
    std::string error;
    const auto source = install::Source::Open(from, error);
    if (!source) return 0;
    std::error_code ec;
    const auto count = [&](const install::Source::File& file, const fs::path& copy)
    {
        if (!fs::is_regular_file(copy, ec) || fs::file_size(copy, ec) != file.size) return;
        bytes += file.size;
        if (remove) fs::remove(copy, ec);
    };
#ifdef AW_TITLE
    // The install disc gave its packages, copied beside the game folder
    // (Install), and nothing else: the files at its root have the names of
    // some of the game disc's, whose copies are not this disc's to remove.
    const fs::path packages = fs::path(g_gameFolder).parent_path() / kPackageFolder;
    bool installDisc = false;
    for (const auto& file : source->DeepFiles())
    {
        if (!IsPackage(file.name)) continue;
        installDisc = true;
        count(file, packages / install::FromUtf8(file.name.substr(std::strlen(kPackagePath))));
    }
    if (installDisc)
    {
        if (remove) fs::remove(packages, ec);
        if (remove) LOGI("install: removed %.1f GB of copies of the install disc's packages", bytes / 1e9);
        return bytes;
    }
#endif
    for (const auto& file : source->Files())
        if (IsGameFile(file.name) && !IsArt(file.name)) count(file, g_gameFolder / install::FromUtf8(file.name));
    for (const auto& file : source->FolderFiles())
        if (IsFolderFile(file.name)) count(file, g_gameFolder / install::FromUtf8(file.name));
    // The disc's folder goes when it is left empty, and only then.
    if (remove) fs::remove(g_gameFolder / kDiscFolder, ec);
    if (remove) LOGI("install: removed %.1f GB of copies of the disc image's files", bytes / 1e9);
#else
    (void)from; (void)remove;
#endif
    return bytes;
}

std::vector<uint8_t>& install::LaunchData()
{
    static std::vector<uint8_t> data = [] {
        std::vector<uint8_t> bytes;
        const char* hex = std::getenv(kLaunchDataVariable);
        for (; hex && hex[0] && hex[1]; hex += 2)
        {
            unsigned value = 0;
            if (std::sscanf(hex, "%2x", &value) != 1) return std::vector<uint8_t>{};
            bytes.push_back(uint8_t(value));
        }
        return bytes;
    }();
    return data;
}

bool install::CompileShaders(const std::string& device, const std::string& why)
{
    // The launcher names the device it compiled for in the game it starts
    // after a pass (launcher/main.cpp).
    const char* compiledFor = std::getenv("MW2_SHADERS_COMPILED_FOR");
    if (compiledFor && device == compiledFor)
    {
        ShowError("The shaders the launcher has just compiled do not fit this game: " + why +
                  "\n\nThe game starts anyway, and compiles shaders as it meets them, which stutters. "
                  "Please report this (REPORT A BUG in the launcher).");
        return false;
    }
    LOGI("shaders: %s The launcher compiles them for device %s, then starts the game.", why.c_str(), device.c_str());
#if defined(MW2_TITLE_MP) || defined(MW3_TITLE_MP) || defined(COD4_TITLE_MP) || defined(AW_TITLE_MP)
    const char* title = "multiplayer";
#else
    const char* title = "campaign";
#endif
    if (StartLauncher({ "--play", title, "--device", device })) return true;
    ShowError(why + "\n\n" + kLauncher + " is not beside this program to compile them. The game starts anyway, "
              "and compiles shaders as it meets them, which stutters.");
    return false;
}

void install::SetNextTitle(const std::string& xex) { g_nextTitle = xex; }

#if defined(MW2_ANDROID) || defined(MW2_USE_SDL)
namespace
{
    // The launch data as it crosses to the next program: hexadecimal text.
    std::string LaunchDataHex()
    {
        std::string hex;
        for (const uint8_t byte : install::LaunchData())
        {
            char text[3];
            std::snprintf(text, sizeof(text), "%02x", byte);
            hex += text;
        }
        return hex;
    }
}
#endif

void install::StartNextTitle()
{
    if (g_nextTitle.empty()) return;
    const char* program = ProgramFor(g_nextTitle);
    if (!program) { LOGE("the title asked for %s, which is not one of its executables", g_nextTitle.c_str()); return; }
#ifdef MW2_ANDROID
    // The app is what starts a run here, and it holds the other executable's
    // library when it carries both titles: it is told, and starts that one
    // once this process has ended (GameActivity.onNextTitle).
    LOGI("the title asked for %s: handing over to the app", g_nextTitle.c_str());
    android::StartTitle(g_nextTitle, LaunchDataHex());
#else
    // A development run was given its image and game folder by hand, and
    // nothing says where the other title's are.
    if (!g_playerStart)
    {
        LOGW("the title asked for %s: start that build yourself, a development run does not", g_nextTitle.c_str());
        return;
    }
#ifdef MW2_USE_SDL
    std::error_code ec;
    const fs::path file = fs::absolute(program, ec);
    if (!fs::is_regular_file(file, ec)) { ShowError(std::string(program) + " is not beside this program."); return; }
    const std::string hex = LaunchDataHex();
    SDL_Environment* environment = SDL_GetEnvironment();
    if (hex.empty()) SDL_UnsetEnvironmentVariable(environment, kLaunchDataVariable);
    else SDL_SetEnvironmentVariable(environment, kLaunchDataVariable, hex.c_str(), true);
    // It writes on in this run's log, where there is one (main.cpp).
    SDL_SetEnvironmentVariable(environment, "MW2_LOG_APPEND", "1", true);
    const std::string path = Utf8(file);
    const char* const args[] = { path.c_str(), nullptr };
    SDL_Process* process = spawn::Start(args, environment);
    if (!process) { ShowError("cannot start " + path + ": " + SDL_GetError()); return; }
    SDL_DestroyProcess(process);
    LOGI("started %s", path.c_str());
#else
    LOGE("this build cannot start %s", program);
#endif
#endif
}

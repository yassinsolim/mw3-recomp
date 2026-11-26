#pragma once
// How a player starts the game: with no arguments. The executable works from
// its own folder, where the launcher (launcher/) installed the game's files
// into game/. With no install there, or another version's, it starts the
// launcher instead.
//
// The game folder can live on another drive: the launcher's INSTALL LOCATION
// names a folder in install_location.txt beside the executables, and both
// the launcher and the game read it (InstallLocation, below). Absent, the
// game folder is game/ beside the executables. Saves stay beside them in
// saves/ either way: they are small, the game files are not.
//
// A run given an image and a game folder on the command line is a
// development run and skips all of this.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace install
{
    struct Launch
    {
        std::vector<uint8_t> image;         // the title's memory image
        std::vector<uint8_t> xexHeader;    // original XEX2 header, for RTL/module metadata
        std::filesystem::path gameRoot;
    };

    // The install-location file both sides read: one UTF-8 line naming the
    // folder the game files live in. Written by the launcher after checking
    // the folder is absolute and creatable; writers, not readers, validate.
    constexpr const char* kInstallLocationFile = "install_location.txt";

    // That folder, or empty when there is no file, it is blank, or it does
    // not name an absolute path. `beside` is the executables' own folder.
    inline std::filesystem::path InstallLocation(const std::filesystem::path& beside)
    {
        std::ifstream in(beside / kInstallLocationFile, std::ios::binary);
        if (!in) return {};
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) return {};
        const char8_t* bytes = reinterpret_cast<const char8_t*>(line.data());
        const std::filesystem::path folder(std::u8string(bytes, line.size()));
        if (!folder.is_absolute()) return {};
        return folder.lexically_normal();
    }

    // True when the command line asks for a player's start.
    bool PlayerStart(int argc, char** argv);

    // The folder this start runs in: the executable's for a player's start,
    // which Prepare moves to, and the current directory (empty) otherwise.
    std::filesystem::path Folder(int argc, char** argv);

    // Loads the installed title. Returns false when the run should end, with
    // `exitCode` saying how: the launcher was started, or a failure was shown.
    bool Prepare(int argc, char** argv, Launch& launch, int& exitCode);

    // A development run's image: a flat PE, or an XEX decrypted on the way in.
    bool LoadImageFile(const std::filesystem::path& path, std::vector<uint8_t>& image,
                       std::vector<uint8_t>& xexHeader);

    // Starts the launcher to compile every shader of the game for `device`
    // (vk::pipeline::DeviceId) and then start this title, when the shaders
    // the launcher compiled do not fit this run, for the reason `why`. False
    // when that is no help: the launcher is not there, or it has just
    // compiled for this device and they still do not fit -- the run goes on
    // then, after saying so.
    bool CompileShaders(const std::string& device, const std::string& why);

    // ---- installing from somewhere other than a terminal or a desktop -----
    // Android has no current directory a player can reach and no file dialog
    // of this runtime's; the app picks the disc image and says where the
    // files go, and drives the copy itself.

    // Where the game's files are, "game" under the current directory unless
    // this says otherwise. Set before anything else in this namespace.
    void SetGameFolder(const std::filesystem::path& folder);
    const std::filesystem::path& GameFolder();

    // Whether that folder holds this build's executable: the check a launcher
    // makes before offering to play.
    bool Installed();

    // Copies the game out of `from` -- a disc image or an extracted disc
    // folder -- into the game folder, checking the executables against the
    // disc this build was recompiled from. With `link`, only what the game
    // folder must hold itself is copied, the executables and the launcher's
    // picture, for a game that reads everything else from the image where it
    // lies (kernel/volume.h); of Advanced Warfare's install disc nothing is
    // copied, and its packages are only checked. Returns false with `error` set, which is a
    // sentence to show a player. `report` is called from a thread of this
    // function's while the copy runs.
    using Report = void (*)(const char* file, uint64_t done, uint64_t total, void* user);
    bool InstallFrom(const std::filesystem::path& from, std::string& error,
                     Report report = nullptr, void* user = nullptr, bool link = false);
    // The bytes of the app's copies of the image's files, which a game
    // played from the image no longer needs: a file of the same name and
    // size as the image's, in the game folder, or beside it for a package of
    // Advanced Warfare's install disc. With `remove`, they are deleted. A
    // mod's files and an update's have other names or sizes, and stay.
    uint64_t CopiedFiles(const std::filesystem::path& from, bool remove);
    // Stops the copy going on now, if there is one. What was copied stays,
    // and the next install carries on from there.
    void CancelInstall();

#ifdef MW2_ANDROID
    // Applies downloaded title update 6 to an installed disc (TU6 builds
    // only): patches both executables in memory exactly as the PC launcher's
    // install does, and lays the update's fastfiles beside the disc's. The
    // package is the STFS file the app downloaded at install time; game files
    // never ride in the APK. False with `error` set, a sentence to show.
    bool ApplyTu6(const std::filesystem::path& package, std::string& error);
#if defined(AW_TITLE) || defined(BO3_TITLE)
    // An executable as the player's title update makes it: the disc's (say
    // default_mp.xex) with the patch of its name in the update's package
    // (default_mp.xexp) applied, written to `to` when its image is the one
    // this app's game code is for (Advanced Warfare's title update 17, Black
    // Ops III's 5). For building that game code on the device (GameBuild.kt).
    // False with `error` set, a sentence to show.
    bool MakeUpdatedExecutable(const std::filesystem::path& package, const std::filesystem::path& disc,
                               const std::filesystem::path& to, std::string& error);
#endif
#endif

    // The data one executable of the title leaves for the next (the console's
    // XamLoaderSetLaunchData): the campaign and the multiplayer are two
    // programs here as there, and it crosses from one to the other in the
    // environment. Empty when nothing left any.
    std::vector<uint8_t>& LaunchData();

    // What the title asked to be started in its place, by the executable's
    // name on the disc ("default_mp.xex"), and the start itself, made once
    // this run has let go of the window and the sound.
    void SetNextTitle(const std::string& xex);
    void StartNextTitle();
}

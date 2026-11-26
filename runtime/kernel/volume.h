#pragma once
// A read-only container the title's files are read out of where they lie,
// with nothing copied out first: the disc image (XDVDFS), laid under the game
// folder, and a downloadable content package (STFS), mounted as a drive.
//
// Either is one host file. A file inside it is a run of that file's bytes for
// a disc, and a chain of 4 KB blocks with hash tables between them for a
// package; the file layer (files.cpp) asks for bytes at a position of an entry
// and never sees the difference.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kernel
{
    class ReadVolume
    {
    public:
        struct Entry
        {
            uint64_t size = 0;
            // A disc's file: where it starts in the image.
            uint64_t offset = 0;
            // A package's file: its blocks, as runs of consecutive block
            // numbers (one run for nearly every file of a downloaded package).
            struct Run { uint32_t first = 0, count = 0; };
            std::vector<Run> runs;
            bool directory = false;
            bool package = false;
        };

        // A package's own account of itself, from its header.
        struct Package
        {
            uint32_t titleId = 0;
            uint32_t contentType = 0;
            // Every licence the header carries, together. The console matches
            // each against the profile and the console it is on; neither is
            // here, and the package is the player's own.
            uint32_t licenceBits = 0;
            std::u16string displayName;
            // The package's own version: of two copies of one pack, the
            // higher is the newer.
            uint32_t version = 0;
        };

        // The disc image at `source`: a path, or "fd:<number>" for a file the
        // app holds open (one chosen with Android's picker has no path). Its
        // root's files and those of the root's own folders, which is every
        // file of the three discs. Null with `error` set when it is not one.
        static std::shared_ptr<ReadVolume> OpenDisc(const std::string& source, std::string& error);
        // A read-only content package (LIVE or PIRS), as a console keeps a
        // downloaded one under Content\0000000000000000\<title>\00000002.
        static std::shared_ptr<ReadVolume> OpenPackage(const std::filesystem::path& file, std::string& error);
        // The same lying inside another file, `size` bytes of `source` from
        // `offset`: a package of a disc image, read where it is (Advanced
        // Warfare's install disc holds the two a console copies to its hard
        // disk). `source` is named as OpenDisc's.
        static std::shared_ptr<ReadVolume> OpenPackage(const std::string& source, uint64_t offset, uint64_t size,
                                                       std::string& error);

        ~ReadVolume();
        ReadVolume(const ReadVolume&) = delete;
        ReadVolume& operator=(const ReadVolume&) = delete;

        // By the guest's path below the volume's root, in either case and
        // with either separator; null when it has no such file or folder.
        const Entry* Find(const std::string& relative) const;
        // The names of the files at the root.
        const std::vector<std::string>& RootFiles() const { return rootFiles_; }
        // The names of the files in one of a package's folders.
        std::vector<std::string> FilesIn(const std::string& folder) const;
        const Package& Header() const { return package_; }
        const std::string& Name() const { return name_; }
        // Where the volume's file is, or empty for one known only by a descriptor.
        const std::filesystem::path& Path() const { return path_; }

        // A descriptor to read one open file through. The volume's file is
        // opened again where it has a path, so that the system reads ahead for
        // each of the title's files as it does for a file of its own; a
        // volume known only by a descriptor shares that one.
        int Acquire() const;
        void Release(int descriptor) const;
        // Up to `size` bytes of `entry` from `position`; the count read, less
        // at the file's end.
        size_t Read(int descriptor, const Entry& entry, uint64_t position, void* out, size_t size) const;

    private:
        ReadVolume() = default;
        bool OpenFile(const std::string& source);
        size_t ReadAt(int descriptor, uint64_t at, void* out, size_t size) const;

        std::string name_;
        std::filesystem::path path_;    // empty: only the descriptor is known
        int descriptor_ = -1;
        uint64_t base_ = 0;             // where the volume starts in its file
        uint64_t size_ = 0;
        std::unordered_map<std::string, Entry> entries_;
        std::vector<std::string> rootFiles_;
        std::vector<std::string> folderFiles_;  // a package's other files, by their paths
        Package package_;
    };

    // The disc image under the game folder, or null when the game's files are
    // in the folder itself. Mounted once, before the title starts.
    bool MountDisc(const std::string& source, std::string& error);
    const std::shared_ptr<ReadVolume>& Disc();
}

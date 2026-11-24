#pragma once
// Where the installer reads the game from: an Xbox 360 disc image (XDVDFS, as
// tools/xdvdfs.py reads it) or a folder a disc was already extracted to.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace install
{
    // Paths as UTF-8, which SDL and the log take, whatever the system's code page.
    inline std::string Utf8(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }
    inline std::filesystem::path FromUtf8(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // The open file an "fd:<number>" source names, or -1 for a path. Android
    // hands a file the player chose with the system's picker over as a
    // descriptor, not a path, and one on a memory card may be readable no
    // other way.
    int SourceDescriptor(const std::string& source);

    class Source
    {
    public:
        struct File
        {
            std::string name;       // as the disc spells it; the root holds all the title reads
            uint64_t size = 0;
            uint64_t offset = 0;    // in the image; unused for a folder
        };

        virtual ~Source() = default;
        // The files at the root of the disc.
        virtual const std::vector<File>& Files() const = 0;
        // The files of the root's own folders, one level down, each named
        // "<folder>/<file>": Call of Duty 4 keeps its textures' top mips in
        // one (highmip/). Apart from Files(), so that what takes the root's
        // files by name never meets a path.
        virtual const std::vector<File>& FolderFiles() const = 0;
        // The files further down than that, each named by its whole path
        // ("<folder>/<folder>/<file>"): the content packages of Advanced
        // Warfare's install disc, which lie where a console's hard disk has
        // them (content/0000000000000000/<title>/00000002/).
        virtual const std::vector<File>& DeepFiles() const = 0;
        // Reads `size` bytes of `file` from `offset` into `out`.
        virtual bool Read(const File& file, uint64_t offset, void* out, size_t size) = 0;

        const File* Find(const std::string& name) const;
        bool ReadAll(const File& file, std::vector<uint8_t>& out);

        // An image file (by path, or "fd:<number>" for one already open) or a
        // folder; null with `error` set if it is neither.
        static std::unique_ptr<Source> Open(const std::filesystem::path& path, std::string& error);
    };
}

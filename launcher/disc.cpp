#include "disc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using install::Utf8;
using install::FromUtf8;

namespace
{
    constexpr uint64_t kSector = 2048;
    constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

    // XDVDFS. The volume descriptor is in sector 32 of the game partition,
    // which starts at one of a few offsets depending on how the disc was dumped
    // (a full dump carries the video partition first).
    class Image final : public install::Source
    {
    public:
        ~Image() override { if (file_) std::fclose(file_); }

        bool Open(const fs::path& path, std::string& error)
        {
            file_ = OpenFile(path);
            if (!file_) { error = "cannot open " + Utf8(path); return false; }
            Seek(0, SEEK_END);
#ifdef _WIN32
            const int64_t endPos = _ftelli64(file_);
#else
            const int64_t endPos = ftello(file_);
#endif
            if (endPos <= 0) { error = "empty disc image"; return false; }
            imageSize_ = uint64_t(endPos);

            for (uint64_t base : { 0x0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull, 0x18310000ull })
            {
                char magic[20];
                if (ReadAt(base + 32 * kSector, magic, 20) && std::memcmp(magic, kMagic, 20) == 0)
                {
                    base_ = base;
                    break;
                }
                if (base == 0x18310000ull) { error = "not an Xbox 360 disc image"; return false; }
            }

            uint8_t volume[kSector];
            if (!ReadAt(base_ + 32 * kSector, volume, kSector)) { error = "the disc image is cut short"; return false; }
            const uint32_t rootSector = Le32(volume + 0x14), rootSize = Le32(volume + 0x18);
            if (rootSize == 0 || rootSize > 32 * 1024 * 1024 ||
                base_ + uint64_t(rootSector) * kSector > imageSize_ ||
                rootSize > imageSize_ - (base_ + uint64_t(rootSector) * kSector))
            {
                error = "the disc image has invalid root directory metadata";
                return false;
            }
            std::vector<uint8_t> root(rootSize);
            if (!ReadAt(base_ + rootSector * kSector, root.data(), root.size()))
            {
                error = "the disc image is cut short";
                return false;
            }
            Walk(root, "", 0);
            if (files_.empty()) { error = "the disc image has no files"; return false; }
            return true;
        }

        const std::vector<File>& Files() const override { return files_; }
        const std::vector<File>& FolderFiles() const override { return folderFiles_; }
        const std::vector<File>& DeepFiles() const override { return deepFiles_; }

        bool Read(const File& file, uint64_t offset, void* out, size_t size) override
        {
            if (offset > file.size || size > file.size - offset) return false;
            if (file.offset > imageSize_ || file.size > imageSize_ - file.offset) return false;
            return ReadAt(file.offset + offset, out, size);
        }

    private:
        static uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
        static uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

        // By the path's own characters (a narrow path on Windows is in the
        // ANSI code page), or a copy of the descriptor an "fd:<number>"
        // source names: the caller keeps its own.
        static std::FILE* OpenFile(const fs::path& path)
        {
#ifdef _WIN32
            return _wfopen(path.c_str(), L"rb");
#else
            const int descriptor = install::SourceDescriptor(path.string());
            if (descriptor < 0) return std::fopen(path.c_str(), "rb");
            const int copy = dup(descriptor);
            std::FILE* file = copy < 0 ? nullptr : fdopen(copy, "rb");
            if (!file && copy >= 0) close(copy);
            return file;
#endif
        }

        // Positions past 2 GB: `long` is 32 bits on Windows.
        void Seek(int64_t offset, int whence)
        {
#ifdef _WIN32
            _fseeki64(file_, offset, whence);
#else
            fseeko(file_, off_t(offset), whence);
#endif
        }

        bool ReadAt(uint64_t at, void* out, size_t size)
        {
            if (at > imageSize_ || size > imageSize_ - at) return false;
            Seek(int64_t(at), SEEK_SET);
            return std::fread(out, 1, size, file_) == size;
        }

        // The setup copies a disc's file to the game folder under the name the
        // image gives it, so a name has to be one file in that folder: not a
        // path ("..\\x.ff", "/home/x.ff"), not a drive ("C:x.ff"). An
        // XDVDFS name never holds a separator or a colon; one that does is not
        // from a disc, and is skipped.
        static bool IsFileName(const std::string& name)
        {
            return !name.empty() && name != "." && name != ".." &&
                   name.find_first_of(std::string("/\\:\0", 4)) == std::string::npos;
        }

        // A directory is a binary tree of entries at 4-byte offsets:
        // { left u16, right u16, sector u32, size u32, attributes u8, name length u8, name }.
        // The root's files are what nearly every title reads; the files of
        // the root's own folders are kept apart, each named "<folder>/<file>"
        // (FolderFiles), and those of folders further down apart again, by
        // their whole paths (DeepFiles), to kDeepest folders below the root.
        static constexpr int kDeepest = 5;
        void Walk(const std::vector<uint8_t>& dir, const std::string& folder, int depth)
        {
            std::vector<File>& files = depth == 0 ? files_ : depth == 1 ? folderFiles_ : deepFiles_;
            std::vector<uint32_t> pending{ 0 }, seen;
            while (!pending.empty())
            {
                const uint32_t at = pending.back() * 4;
                pending.pop_back();
                if (std::find(seen.begin(), seen.end(), at) != seen.end() || at + 14 > dir.size()) continue;
                seen.push_back(at);
                const uint8_t* e = dir.data() + at;
                const uint16_t left = Le16(e), right = Le16(e + 2);
                if (left == 0xFFFF) continue;
                if (left) pending.push_back(left);
                if (right) pending.push_back(right);
                const uint8_t attributes = e[12], length = e[13];
                if (at + 14 + length > dir.size()) continue;
                std::string name(reinterpret_cast<const char*>(e + 14), length);
                if (!IsFileName(name)) continue;

                const uint64_t offset = base_ + uint64_t(Le32(e + 4)) * kSector;
                const uint32_t size = Le32(e + 8);
                if (offset > imageSize_ || size > imageSize_ - offset) continue;

                if (attributes & 0x10)   // a directory
                {
                    if (depth >= kDeepest || size == 0 || size > 32 * 1024 * 1024) continue;
                    std::vector<uint8_t> table(size);
                    if (ReadAt(offset, table.data(), table.size())) Walk(table, folder + name + "/", depth + 1);
                    continue;
                }
                files.push_back({ folder + name, size, offset });
            }
        }

        std::FILE* file_ = nullptr;
        uint64_t base_ = 0;
        uint64_t imageSize_ = 0;
        std::vector<File> files_, folderFiles_, deepFiles_;
    };

    // A disc already extracted: the files at the top of the folder, and apart
    // from them those of its own folders.
    class Folder final : public install::Source
    {
    public:
        bool Open(const fs::path& path, std::string& error)
        {
            root_ = path;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(path, ec))
            {
                if (entry.is_regular_file(ec))
                    files_.push_back({ Utf8(entry.path().filename()), entry.file_size(ec), 0 });
                else if (entry.is_directory(ec))
                {
                    std::error_code inner;
                    for (auto file = fs::recursive_directory_iterator(entry.path(), inner);
                         !inner && file != fs::recursive_directory_iterator(); file.increment(inner))
                    {
                        if (!file->is_regular_file(inner)) continue;
                        const std::string name = Utf8(fs::relative(file->path(), path, inner).generic_string());
                        (file.depth() == 0 ? folderFiles_ : deepFiles_).push_back({ name, file->file_size(inner), 0 });
                    }
                }
            }
            if (ec || files_.empty()) { error = "cannot read the folder " + Utf8(path); return false; }
            return true;
        }

        const std::vector<File>& Files() const override { return files_; }
        const std::vector<File>& FolderFiles() const override { return folderFiles_; }
        const std::vector<File>& DeepFiles() const override { return deepFiles_; }

        bool Read(const File& file, uint64_t offset, void* out, size_t size) override
        {
            if (open_ != file.name)
            {
                stream_ = std::ifstream(root_ / install::FromUtf8(file.name), std::ios::binary);
                open_ = file.name;
            }
            stream_.clear();
            stream_.seekg(std::streamoff(offset));
            stream_.read(static_cast<char*>(out), std::streamsize(size));
            return size_t(stream_.gcount()) == size;
        }

    private:
        fs::path root_;
        std::vector<File> files_, folderFiles_, deepFiles_;
        std::ifstream stream_;
        std::string open_;
    };

    bool SameName(const std::string& a, const std::string& b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
    }
}

const install::Source::File* install::Source::Find(const std::string& name) const
{
    for (const File& file : Files())
        if (SameName(file.name, name)) return &file;
    return nullptr;
}

bool install::Source::ReadAll(const File& file, std::vector<uint8_t>& out)
{
    out.resize(file.size);
    return Read(file, 0, out.data(), out.size());
}

int install::SourceDescriptor(const std::string& source)
{
    if (source.rfind("fd:", 0) != 0 || source.size() == 3) return -1;
    char* end = nullptr;
    const long number = std::strtol(source.c_str() + 3, &end, 10);
    return (*end || number < 0) ? -1 : int(number);
}

std::unique_ptr<install::Source> install::Source::Open(const fs::path& path, std::string& error)
{
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        auto folder = std::make_unique<Folder>();
        if (!folder->Open(path, error)) return nullptr;
        return folder;
    }
    if (SourceDescriptor(path.string()) < 0 && !fs::is_regular_file(path, ec))
    {
        error = Utf8(path) + " is neither a disc image nor a folder";
        return nullptr;
    }
    auto image = std::make_unique<Image>();
    if (!image->Open(path, error)) return nullptr;
    return image;
}

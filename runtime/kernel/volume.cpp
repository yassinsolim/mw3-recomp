#include "volume.h"
#include "../log.h"
#include "../../launcher/disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    // An entry's key: the guest's file system does not tell cases apart, and
    // a path reaches here with either separator.
    std::string Key(const std::string& path)
    {
        std::string key;
        for (char c : path)
        {
            if (c == '/') c = '\\';
            if (c == '\\' && (key.empty() || key.back() == '\\')) continue;
            key += char(std::tolower(uint8_t(c)));
        }
        while (!key.empty() && key.back() == '\\') key.pop_back();
        return key;
    }

    // ---- STFS, the read-only kind (tools/stfs.py) ---------------------------
    constexpr uint64_t kBlock = 0x1000;
    constexpr uint32_t kPerTable = 0xAA;        // data blocks one hash table covers
    constexpr uint32_t kPerLevel1 = 0x70E4;     // and one second-level table
    constexpr uint64_t kFirstBlock = 0xC000;
    constexpr size_t kTableEntry = 0x18, kFileEntry = 0x40;
    // A file table of more blocks than this is not a map pack's (it would name
    // sixteen thousand files), and is not read.
    constexpr uint32_t kMostTableBlocks = 0x100;

    // Where a data block is in the package: the hash tables sit between them.
    uint64_t BlockOffset(uint32_t block)
    {
        uint64_t tables = 0;
        if (block >= kPerTable) tables += block / kPerTable + 1;
        if (block >= kPerLevel1) tables += block / kPerLevel1 + 1;
        return kFirstBlock + (uint64_t(block) + tables) * kBlock;
    }

    uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    uint32_t Be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
    uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

    std::shared_ptr<ReadVolume> g_disc;
}

ReadVolume::~ReadVolume()
{
    if (descriptor_ >= 0) close(descriptor_);
}

bool ReadVolume::OpenFile(const std::string& source)
{
    name_ = source;
#ifdef _WIN32
    path_ = install::FromUtf8(source);
    descriptor_ = _wopen(path_.c_str(), _O_RDONLY | _O_BINARY);
#else
    if (const int held = install::SourceDescriptor(source); held >= 0) descriptor_ = dup(held);
    else
    {
        path_ = source;
        descriptor_ = open(source.c_str(), O_RDONLY | O_CLOEXEC);
    }
#endif
    if (descriptor_ < 0) return false;
#ifdef _WIN32
    const int64_t end = _lseeki64(descriptor_, 0, SEEK_END);
#else
    const int64_t end = lseek(descriptor_, 0, SEEK_END);
#endif
    size_ = end > 0 ? uint64_t(end) : 0;
    return size_ != 0;
}

int ReadVolume::Acquire() const
{
    if (path_.empty()) return descriptor_;
#ifdef _WIN32
    const int own = _wopen(path_.c_str(), _O_RDONLY | _O_BINARY);
#else
    const int own = open(path_.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    // Out of descriptors, or the file has gone: the volume's own still reads.
    return own >= 0 ? own : descriptor_;
}

void ReadVolume::Release(int descriptor) const
{
    if (descriptor >= 0 && descriptor != descriptor_) close(descriptor);
}

size_t ReadVolume::ReadAt(int descriptor, uint64_t at, void* out, size_t size) const
{
    if (at >= size_) return 0;
    size = size_t(std::min<uint64_t>(size, size_ - at));
    size_t done = 0;
    while (done < size)
    {
#ifdef _WIN32
        // No shared descriptor here: every reader has its own, and the one
        // the volume keeps is read only while it is being opened.
        if (_lseeki64(descriptor, int64_t(base_ + at + done), SEEK_SET) < 0) break;
        const int got = _read(descriptor, static_cast<char*>(out) + done,
                              unsigned(std::min<size_t>(size - done, 1u << 30)));
#else
        // At a position, not from the descriptor's own: one descriptor may
        // serve several of the title's threads at once.
        const ssize_t got = pread(descriptor, static_cast<char*>(out) + done, size - done, off_t(base_ + at + done));
#endif
        if (got <= 0) break;
        done += size_t(got);
    }
    return done;
}

size_t ReadVolume::Read(int descriptor, const Entry& entry, uint64_t position, void* out, size_t size) const
{
    if (entry.directory || position >= entry.size) return 0;
    size = size_t(std::min<uint64_t>(size, entry.size - position));
    if (!entry.package) return ReadAt(descriptor, entry.offset + position, out, size);

    // The run the position is in, then as far as the blocks lie together:
    // to the run's end, or to the next hash table.
    size_t done = 0;
    uint64_t index = position / kBlock;
    uint64_t within = position % kBlock;
    for (const Entry::Run& run : entry.runs)
    {
        if (index >= run.count) { index -= run.count; continue; }
        while (index < run.count && done < size)
        {
            const uint32_t block = run.first + uint32_t(index);
            const uint64_t together = std::min<uint64_t>(run.count - index, kPerTable - block % kPerTable);
            const size_t take = size_t(std::min<uint64_t>(size - done, together * kBlock - within));
            const size_t got = ReadAt(descriptor, BlockOffset(block) + within, static_cast<char*>(out) + done, take);
            done += got;
            if (got != take) return done;
            index += (within + take) / kBlock;
            within = (within + take) % kBlock;
        }
        if (done == size) break;
        index = 0;
    }
    return done;
}

std::vector<std::string> ReadVolume::FilesIn(const std::string& folder) const
{
    const std::string in = Key(folder) + "\\";
    std::vector<std::string> names;
    for (const std::string& path : folderFiles_)
        if (path.size() > in.size() && Key(path.substr(0, in.size())) + "\\" == in && path.find('\\', in.size()) == std::string::npos)
            names.push_back(path.substr(in.size()));
    return names;
}

const ReadVolume::Entry* ReadVolume::Find(const std::string& relative) const
{
    const auto it = entries_.find(Key(relative));
    return it == entries_.end() ? nullptr : &it->second;
}

const std::vector<std::string>* ReadVolume::Children(const std::string& folder) const
{
    const auto it = children_.find(Key(folder));
    return it == children_.end() ? nullptr : &it->second;
}

std::shared_ptr<ReadVolume> ReadVolume::OpenDisc(const std::string& source, std::string& error)
{
    const fs::path path = install::FromUtf8(source);
    std::error_code ec;
    if (fs::is_directory(path, ec)) { error = source + " is a folder, not a disc image"; return nullptr; }
    const auto listing = install::Source::Open(path, error);
    if (!listing) return nullptr;

    std::shared_ptr<ReadVolume> volume(new ReadVolume);
    if (!volume->OpenFile(source)) { error = "cannot open " + source; return nullptr; }
    for (const auto& file : listing->Files())
    {
        volume->entries_[Key(file.name)] = Entry{ file.size, file.offset };
        volume->rootFiles_.push_back(file.name);
    }
    for (const auto& file : listing->FolderFiles())
    {
        volume->entries_[Key(file.name)] = Entry{ file.size, file.offset };
        Entry folder;
        folder.directory = true;
        volume->entries_.try_emplace(Key(file.name.substr(0, file.name.find('/'))), folder);
    }
#if defined(BO3_TITLE)
    // Black Ops III's disc keeps its sound banks two folders down
    // (snd\en\<bank>), and the title lists a folder before it reads from it
    // (files.cpp), as it does a package's: so the files further down are
    // here too, and every folder knows what it holds.
    for (const auto& file : listing->DeepFiles()) volume->entries_[Key(file.name)] = Entry{ file.size, file.offset };
    for (const auto* files : { &listing->Files(), &listing->FolderFiles(), &listing->DeepFiles() })
        for (const auto& file : *files)
        {
            // The file in its folder and each folder in the one above it; a
            // name that is there already is a folder met before, and so are
            // those above it.
            std::string path = file.name;
            while (true)
            {
                const size_t cut = path.rfind('/');
                const std::string name = path.substr(cut == std::string::npos ? 0 : cut + 1);
                auto& names = volume->children_[cut == std::string::npos ? std::string() : Key(path.substr(0, cut))];
                if (std::find(names.begin(), names.end(), name) != names.end()) break;
                names.push_back(name);
                if (cut == std::string::npos) break;
                path.resize(cut);
                Entry folder;
                folder.directory = true;
                volume->entries_.try_emplace(Key(path), folder);
            }
        }
#endif
    return volume;
}

std::shared_ptr<ReadVolume> ReadVolume::OpenPackage(const fs::path& file, std::string& error)
{
    return OpenPackage(install::Utf8(file), 0, UINT64_MAX, error);
}

std::shared_ptr<ReadVolume> ReadVolume::OpenPackage(const std::string& source, uint64_t offset, uint64_t size,
                                                    std::string& error)
{
    std::shared_ptr<ReadVolume> volume(new ReadVolume);
    if (!volume->OpenFile(source) || offset >= volume->size_) { error = "cannot be opened"; return nullptr; }
    // From here every position is the package's own: a range past the
    // file's end is cut to it, and a package cut short says so below.
    volume->base_ = offset;
    volume->size_ = std::min(size, volume->size_ - offset);
    const int fd = volume->descriptor_;

    uint8_t header[0x1000];
    if (volume->ReadAt(fd, 0, header, sizeof header) != sizeof header ||
        (std::memcmp(header, "LIVE", 4) != 0 && std::memcmp(header, "PIRS", 4) != 0))
    {
        error = "is not a downloaded content package (LIVE or PIRS)";
        return nullptr;
    }
    // One hash table per group in a read-only package; a console-signed one
    // keeps two and lays its blocks out differently.
    if (((Be32(header + 0x340) + 0xFFF) & 0xF000) >> 12 != 0xB)
    {
        error = "is not a read-only package";
        return nullptr;
    }
    Package& package = volume->package_;
    package.contentType = Be32(header + 0x344);
    package.titleId = Be32(header + 0x360);
    for (size_t at = 0x22C; at < 0x22C + 16 * 16; at += 16) package.licenceBits |= Be32(header + at + 8);
    for (size_t at = 0x411; at < 0x411 + 2 * 127; at += 2)
    {
        const char16_t c = char16_t(header[at] << 8 | header[at + 1]);
        if (!c) break;
        package.displayName += c;
    }

    // A file's blocks, by the chain its hash tables keep: each block's entry
    // in its group's table names the block after it.
    uint8_t table[kBlock];
    uint32_t tableOf = UINT32_MAX;
    const auto chain = [&](uint32_t start, uint32_t count, std::vector<Entry::Run>& runs)
    {
        uint32_t block = start;
        for (uint32_t i = 0; i < count; i++)
        {
            if (BlockOffset(block) + kBlock > volume->size_) return false;
            if (!runs.empty() && runs.back().first + runs.back().count == block) runs.back().count++;
            else runs.push_back({ block, 1 });
            if (i + 1 == count) break;
            const uint32_t group = block - block % kPerTable;
            if (group != tableOf)
            {
                if (volume->ReadAt(fd, BlockOffset(group) - kBlock, table, kBlock) != kBlock) return false;
                tableOf = group;
            }
            block = Be24(table + size_t(block % kPerTable) * kTableEntry + 0x15);
        }
        return true;
    };

    const uint32_t tableBlocks = uint32_t(header[0x37C]) | uint32_t(header[0x37D]) << 8;
    Entry listing;
    listing.package = true;
    listing.size = uint64_t(tableBlocks) * kBlock;
    if (tableBlocks == 0 || tableBlocks > kMostTableBlocks || !chain(Le24(header + 0x37E), tableBlocks, listing.runs))
    {
        error = "has a file table that cannot be read";
        return nullptr;
    }
    std::vector<uint8_t> files(size_t(listing.size));
    if (volume->Read(fd, listing, 0, files.data(), files.size()) != files.size())
    {
        error = "has a file table that is cut short";
        return nullptr;
    }

    // { name[40], flags (length, 0x80 a folder), blocks u24, blocks u24,
    //   first block u24, the folder it is in u16 (0xFFFF: the root), size u32, times }
    const size_t count = files.size() / kFileEntry;
    const auto nameOf = [&](size_t index)
    {
        const uint8_t* e = files.data() + index * kFileEntry;
        return std::string(reinterpret_cast<const char*>(e), std::min<size_t>(e[0x28] & 0x3F, 0x28));
    };
    for (size_t index = 0; index < count && files[index * kFileEntry]; index++)
    {
        const uint8_t* e = files.data() + index * kFileEntry;
        std::string path = nameOf(index);
        // Up through the folders it is in; a table that loops is cut off.
        uint32_t parent = uint32_t(e[0x32]) << 8 | e[0x33];
        for (int depth = 0; parent != 0xFFFF && parent < count && depth < 16; depth++)
        {
            path = nameOf(parent) + "\\" + path;
            const uint8_t* up = files.data() + size_t(parent) * kFileEntry;
            parent = uint32_t(up[0x32]) << 8 | up[0x33];
        }
        Entry entry;
        entry.package = true;
        if (e[0x28] & 0x80) entry.directory = true;
        else
        {
            entry.size = Be32(e + 0x34);
            const uint32_t blocks = uint32_t((entry.size + kBlock - 1) / kBlock);
            if (blocks && !chain(Le24(e + 0x2F), blocks, entry.runs))
            {
                error = "is cut short: " + path + " ends past the file's end";
                return nullptr;
            }
            if (path.find('\\') == std::string::npos) volume->rootFiles_.push_back(path);
            else volume->folderFiles_.push_back(path);
        }
        const size_t cut = path.rfind('\\');
        volume->children_[cut == std::string::npos ? std::string() : Key(path.substr(0, cut))].push_back(nameOf(index));
        if (entry.directory) volume->children_.try_emplace(Key(path));
        volume->entries_[Key(path)] = std::move(entry);
    }
    volume->children_.try_emplace(std::string());
    return volume;
}

bool kernel::MountDisc(const std::string& source, std::string& error)
{
    g_disc = ReadVolume::OpenDisc(source, error);
    if (!g_disc) return false;
    LOGI("disc image: %s (%zu files at its root); the game's files are read from it", source.c_str(),
         g_disc->RootFiles().size());
    return true;
}

const std::shared_ptr<ReadVolume>& kernel::Disc() { return g_disc; }

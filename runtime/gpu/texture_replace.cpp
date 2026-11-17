// Texture replacement and the keyed texture dump (texture_replace.h).
#include "texture_replace.h"

#include "../env.h"
#include "../install/install.h"
#include "../log.h"
#include "../png.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
    namespace fs = std::filesystem;

    // Paths as UTF-8, which the log and the switches take, whatever the
    // system's code page.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path FromUtf8(const std::string& text)
    {
        return fs::path(std::u8string(text.begin(), text.end()));
    }

    // How much decoded picture is kept for the next image made from it. A
    // replacement is wanted again whenever the cache lets its image go and the
    // title binds the texture once more, and decoding a file a second time
    // holds a frame up as long as the first did; but a folder of large skins
    // decoded and kept whole would be most of a phone's memory.
#if defined(MW2_ANDROID) || defined(__ANDROID__)
    constexpr size_t kHeldMost = size_t(32) << 20;
#else
    constexpr size_t kHeldMost = size_t(128) << 20;
#endif

    struct Entry
    {
        fs::path file;
        // Would not decode, or is for a texture that is not replaced: said
        // once, and not tried again.
        bool failed = false;
        std::shared_ptr<const gpu::TextureData> decoded;
        bool gamma = false;   // how `decoded`'s levels were averaged
        uint64_t used = 0;    // State::clock when it was last asked for
    };

    struct State
    {
        // Everything below but `index`'s keys and files, which are settled
        // before anything else reads them (Start).
        std::mutex lock;
        std::unordered_map<uint64_t, Entry> index;
        fs::path folder, dumps;
        bool dumping = false;
        std::unordered_set<uint64_t> dumped;
        size_t held = 0;       // bytes of decoded pictures in `index`
        uint64_t clock = 0;
        uint64_t applied = 0, failed = 0;
    };
    State s;

    // The folder's files by the key in their names. PNG is preferred where a
    // key has both, as the one that can hold alpha.
    bool Start()
    {
        std::error_code ec;
        if (const char* named = env::Text("MW2_TEXTURE_REPLACE")) s.folder = FromUtf8(named);
        else if (const fs::path beside = install::GameFolder() / "replace"; fs::is_directory(beside, ec))
            s.folder = beside;
        if (!s.folder.empty())
        {
            const fs::recursive_directory_iterator end;
            for (fs::recursive_directory_iterator at(
                     s.folder, fs::directory_options::skip_permission_denied, ec);
                 !ec && at != end; at.increment(ec))
            {
                if (!at->is_regular_file(ec)) continue;
                const fs::path file = at->path();
                std::string extension = Utf8(file.extension());
                for (char& c : extension) c = char(std::tolower(uint8_t(c)));
                const bool png = extension == ".png";
                if (!png && extension != ".pnm") continue;
                const std::string stem = Utf8(file.stem());
                if (stem.empty() || stem.size() > 16 ||
                    !std::all_of(stem.begin(), stem.end(),
                                 [](char c) { return std::isxdigit(uint8_t(c)) != 0; }))
                    continue;
                const uint64_t key = std::strtoull(stem.c_str(), nullptr, 16);
                if (!key) continue;
                Entry& entry = s.index[key];
                if (entry.file.empty() || png) entry.file = file;
            }
            LOGI("textures: %zu replacements in %s", s.index.size(),
                 Utf8(fs::absolute(s.folder, ec)).c_str());
        }
        if (const char* dumps = env::Text("MW2_DUMP_TEXTURES"))
        {
            s.dumps = FromUtf8(dumps);
            fs::create_directories(s.dumps, ec);
            s.dumping = true;
        }
        return s.dumping || !s.index.empty();
    }

    // Called with the lock held.
    void Fail(Entry& entry, const char* why)
    {
        entry.failed = true;
        if (entry.decoded) s.held -= entry.decoded->bytes.size();
        entry.decoded.reset();
        s.failed++;
        LOGW("textures: replacement %s is not used: %s", Utf8(entry.file).c_str(), why);
    }

    // What is replaced: one flat picture. A cube's six faces, a volume's
    // slices and an array's layers have no single picture to stand for them.
    bool Flat(const gpu::TextureData& data, const gpu::TextureFetch& fetch)
    {
        return fetch.Dimension() != gpu::TextureDimension::Cube &&
               fetch.Dimension() != gpu::TextureDimension::D3 && !fetch.Stacked() &&
               data.layers == 1 && !data.volume;
    }

    // A binary PNM, grey (P5) or RGB (P6), eight bits a channel: what the
    // frame dumps write, and what a script makes without a library.
    bool ReadPnm(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba, uint32_t& width,
                 uint32_t& height, const char*& error)
    {
        if (file.size() < 2 || file[0] != 'P' || (file[1] != '5' && file[1] != '6'))
        {
            error = "neither a PNG nor a binary PNM (P5 or P6)";
            return false;
        }
        size_t at = 2;
        // The header's numbers, with white space and `#` comments between.
        const auto number = [&](uint32_t& value) {
            for (;;)
            {
                while (at < file.size() && std::isspace(file[at])) at++;
                if (at >= file.size() || file[at] != '#') break;
                while (at < file.size() && file[at] != '\n') at++;
            }
            if (at >= file.size() || !std::isdigit(file[at])) return false;
            uint64_t read = 0;
            for (; at < file.size() && std::isdigit(file[at]); at++)
            {
                read = read * 10 + (file[at] - '0');
                if (read > 0xFFFFFFFFull) return false;
            }
            value = uint32_t(read);
            return true;
        };
        uint32_t most = 0;
        if (!number(width) || !number(height) || !number(most) || !most || most > 255 ||
            at >= file.size())
        {
            error = "a PNM whose header does not parse, or of more than eight bits a channel";
            return false;
        }
        at++;   // the one byte of white space that ends the header
        const size_t channels = file[1] == '6' ? 3 : 1;
        const uint64_t pixels = uint64_t(width) * height;
        if (!pixels || pixels > (64ull << 20) || (file.size() - at) / channels < pixels)
        {
            error = "a PNM of no size, too large, or shorter than its header says";
            return false;
        }
        rgba.resize(size_t(pixels) * 4);
        const uint8_t* in = file.data() + at;
        uint8_t* out = rgba.data();
        for (uint64_t i = 0; i < pixels; i++, in += channels, out += 4)
        {
            out[0] = uint8_t(in[0] * 255u / most);
            out[1] = uint8_t(in[channels == 3 ? 1 : 0] * 255u / most);
            out[2] = uint8_t(in[channels == 3 ? 2 : 0] * 255u / most);
            out[3] = 255;
        }
        return true;
    }

    // The sRGB curve both ways, by table: a level of a gamma texture is the
    // average of the light its four texels stand for, not of their codes,
    // which would darken every level a little more than the one above it.
    struct Curve
    {
        float linear[256];
        uint8_t encoded[4096];
        Curve()
        {
            for (int i = 0; i < 256; i++)
            {
                const float c = float(i) / 255.0f;
                linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            for (int i = 0; i < 4096; i++)
            {
                const float l = float(i) / 4095.0f;
                const float c = l <= 0.0031308f ? l * 12.92f
                                                : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                encoded[i] = uint8_t(c * 255.0f + 0.5f);
            }
        }
    };

    // One level from the one above: each texel the average of the two by two
    // it covers. A side of odd length gives up its last row or column.
    void Halve(const uint8_t* from, const gpu::TextureData::Level& above, uint8_t* to,
               const gpu::TextureData::Level& level, bool gamma)
    {
        static const Curve curve;
        for (uint32_t y = 0; y < level.height; y++)
        {
            const uint32_t rows[2] = { std::min(2 * y, above.height - 1),
                                       std::min(2 * y + 1, above.height - 1) };
            for (uint32_t x = 0; x < level.width; x++, to += 4)
            {
                const uint32_t columns[2] = { std::min(2 * x, above.width - 1),
                                              std::min(2 * x + 1, above.width - 1) };
                const uint8_t* texel[4];
                for (int i = 0; i < 4; i++)
                    texel[i] = from + (size_t(rows[i >> 1]) * above.width + columns[i & 1]) * 4;
                for (int channel = 0; channel < 4; channel++)
                {
                    // Alpha is linear whatever the colour is.
                    if (gamma && channel < 3)
                    {
                        float light = 0.0f;
                        for (int i = 0; i < 4; i++) light += curve.linear[texel[i][channel]];
                        to[channel] = curve.encoded[size_t(light * (4095.0f / 4.0f) + 0.5f)];
                    }
                    else
                    {
                        uint32_t sum = 2;
                        for (int i = 0; i < 4; i++) sum += texel[i][channel];
                        to[channel] = uint8_t(sum / 4);
                    }
                }
            }
        }
    }

    // A replacement's file as the texture the cache uploads in the original's
    // place. `format` is the original's.
    bool Decode(const fs::path& path, uint32_t format, bool gamma, gpu::TextureData& out,
                const char*& error)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { error = "it cannot be opened"; return false; }
        std::vector<uint8_t> file(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(file.data()), std::streamsize(file.size()));
        if (!in) { error = "it cannot be read"; return false; }

        uint32_t width = 0, height = 0;
        const bool isPng = file.size() >= 4 && std::memcmp(file.data(), "\x89PNG", 4) == 0;
        if (!(isPng ? png::Read(file, out.bytes, width, height, error)
                    : ReadPnm(file, out.bytes, width, height, error)))
            return false;

        // The picture is in the order the dump wrote it, which for 8_8_8_8 is
        // not the order the bytes are held in: DecodeToRgba shows red and blue
        // exchanged, as the title's swizzle for these reads them, and the view
        // of the replacement is made with that same swizzle. Every other
        // format the dump decodes is shown channel for channel.
        if (format == 6 || format == 14 || format == 50)
            for (size_t at = 0; at + 4 <= out.bytes.size(); at += 4)
                std::swap(out.bytes[at], out.bytes[at + 2]);

        out.width = width;
        out.height = height;
        out.blocksWide = width;
        out.blocksHigh = height;
        out.bytesPerBlock = 4;
        out.expanded = true;
        // Every level down to one texel: the title's sampler state for the
        // texture expects a chain, and a picture of another size cannot borrow
        // the original's.
        size_t end = 0;
        for (uint32_t w = width, h = height;; w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
        {
            gpu::TextureData::Level level;
            level.width = level.blocksWide = w;
            level.height = level.blocksHigh = h;
            level.offset = (end + 15) & ~size_t(15);
            level.size = size_t(w) * h * 4;
            end = level.offset + level.size;
            out.levels.push_back(level);
            if (w == 1 && h == 1) break;
        }
        out.bytes.resize(end);
        for (size_t i = 1; i < out.levels.size(); i++)
            Halve(out.bytes.data() + out.levels[i - 1].offset, out.levels[i - 1],
                  out.bytes.data() + out.levels[i].offset, out.levels[i], gamma);
        out.ok = true;
        return true;
    }

    // The decoded pictures back under the bound, the least recently asked for
    // going first; never `keep`, which is the one just decoded. Called with
    // the lock held. An image being made from one that goes still holds it.
    void Trim(uint64_t keep)
    {
        while (s.held > kHeldMost)
        {
            Entry* oldest = nullptr;
            for (auto& [key, entry] : s.index)
                if (entry.decoded && key != keep && (!oldest || entry.used < oldest->used))
                    oldest = &entry;
            if (!oldest) return;
            s.held -= oldest->decoded->bytes.size();
            oldest->decoded.reset();
        }
    }
}

bool gpu::replace::On()
{
    static const bool on = Start();
    return on;
}

uint32_t gpu::replace::KeyedLevel(const TextureData& data)
{
    return data.levels.empty() ? 0
                               : std::min(data.minLevel, uint32_t(data.levels.size() - 1));
}

uint64_t gpu::replace::KeyOf(const TextureData& data, uint32_t format)
{
    if (!data.ok || data.levels.empty()) return 0;
    const uint32_t keyed = KeyedLevel(data);
    const TextureData::Level& level = data.levels[keyed];
    if (level.offset + level.size > data.bytes.size()) return 0;
    const uint8_t* bytes = data.bytes.data() + level.offset;

    // Four lanes, to keep the multiplies off one dependency chain, each
    // rotated after its multiply: a multiply alone carries a word's top bit
    // nowhere but the lane's top bit, where the same change in a later word
    // would cancel it.
    constexpr uint64_t kPrime = 0x100000001B3ull;
    uint64_t lane[4] = { 0xCBF29CE484222325ull, 0x84222325CBF29CE4ull,
                         0x9E3779B97F4A7C15ull, 0x7F4A7C159E3779B9ull };
    size_t at = 0;
    for (; at + 32 <= level.size; at += 32)
    {
        uint64_t words[4];
        std::memcpy(words, bytes + at, sizeof words);
        for (int i = 0; i < 4; i++) lane[i] = std::rotl((lane[i] ^ words[i]) * kPrime, 31);
    }
    for (; at < level.size; at++)
        lane[at & 3] = std::rotl((lane[at & 3] ^ bytes[at]) * kPrime, 31);

    uint64_t key = uint64_t(level.size) * 0x9E3779B97F4A7C15ull;
    for (const uint64_t word : { lane[0], lane[1], lane[2], lane[3], uint64_t(format),
                                 uint64_t(level.width), uint64_t(level.height),
                                 uint64_t(level.layers), uint64_t(data.levels.size() - keyed),
                                 uint64_t(data.volume) })
        key = std::rotl((key ^ word) * kPrime, 31);
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDull;
    key ^= key >> 33;
    key *= 0xC4CEB9FE1A85EC53ull;
    key ^= key >> 33;
    return key ? key : 1;
}

std::shared_ptr<const gpu::TextureData> gpu::replace::Load(uint64_t key,
                                                           const TextureData& original,
                                                           const TextureFetch& fetch, bool gamma)
{
    if (!key) return nullptr;
    // An entry is never removed, so it stays where it is while the file is
    // decoded outside the lock: two textures decode side by side.
    Entry* entry = nullptr;
    {
        std::lock_guard lock(s.lock);
        const auto found = s.index.find(key);
        if (found == s.index.end() || found->second.failed) return nullptr;
        entry = &found->second;
        if (!Flat(original, fetch))
        {
            Fail(*entry, "cube, volume and array textures are not replaced");
            return nullptr;
        }
        if (entry->decoded && entry->gamma == gamma)
        {
            entry->used = ++s.clock;
            return entry->decoded;
        }
    }

    const char* error = nullptr;
    const auto decoded = std::make_shared<TextureData>();
    const bool ok = Decode(entry->file, fetch.Format(), gamma, *decoded, error);

    std::lock_guard lock(s.lock);
    if (entry->failed) return nullptr;
    if (!ok)
    {
        Fail(*entry, error ? error : "it does not decode");
        return nullptr;
    }
    LOGI("textures: replacement %s read, %ux%u", Utf8(entry->file).c_str(), decoded->width,
         decoded->height);
    if (entry->decoded) s.held -= entry->decoded->bytes.size();
    entry->decoded = decoded;
    entry->gamma = gamma;
    entry->used = ++s.clock;
    s.held += decoded->bytes.size();
    Trim(key);
    return decoded;
}

void gpu::replace::Refuse(uint64_t key, const char* why)
{
    std::lock_guard lock(s.lock);
    const auto found = s.index.find(key);
    if (found != s.index.end() && !found->second.failed) Fail(found->second, why);
}

void gpu::replace::Applied()
{
    std::lock_guard lock(s.lock);
    s.applied++;
}

void gpu::replace::Dump(uint64_t key, const TextureData& data, const TextureFetch& fetch)
{
    if (!s.dumping || !key) return;
    const uint32_t keyed = KeyedLevel(data);
    const TextureData::Level& level = data.levels[keyed];
    const uint8_t* bytes = data.bytes.data() + level.offset;
    if (std::all_of(bytes, bytes + level.size, [](uint8_t byte) { return !byte; })) return;
    {
        std::lock_guard lock(s.lock);
        if (!s.dumped.insert(key).second) return;
    }

    char name[32];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)key);
    std::vector<uint8_t> rgba;
    const bool picture =
        DecodeToRgba(data, fetch.Format(), rgba, keyed) &&
        png::Write(s.dumps / (std::string(name) + ".png"), rgba.data(), level.width,
                   level.height * level.layers);

    const char* format = TextureFormatName(fetch.Format());
    char line[256];
    std::snprintf(line, sizeof line, "%s %s %ux%u, %zu levels%s%s%s\n", name,
                  format ? format : "unknown", level.width, level.height,
                  data.levels.size() - keyed, fetch.Linearises() ? ", gamma" : "",
                  Flat(data, fetch) ? "" : ", a cube, volume or array: not replaced",
                  picture ? "" : ", no picture: not a format the dump decodes");
    std::lock_guard lock(s.lock);
    std::ofstream list(s.dumps / "textures.txt", std::ios::app);
    list << line;
}

void gpu::replace::Report()
{
    std::lock_guard lock(s.lock);
    if (s.index.empty()) return;
    std::error_code ec;
    LOGI("textures: %zu replacements indexed in %s; images made from them %llu times, %llu"
         " failed", s.index.size(), Utf8(fs::absolute(s.folder, ec)).c_str(),
         (unsigned long long)s.applied, (unsigned long long)s.failed);
}

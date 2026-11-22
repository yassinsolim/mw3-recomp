// Texture replacement and the keyed texture dump (texture_replace.h).
#include "texture_replace.h"

#include "../env.h"
#include "../install/install.h"
#include "../log.h"
#include "../png.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
        // The picture's file; an animation's first frame.
        fs::path file;
        // An animation's frames in the order they are shown; empty for a
        // single picture. Settled in Start.
        std::vector<fs::path> frames;
        double fps = 12.0;
        // Would not decode, or is for a texture that is not replaced: said
        // once, and not tried again.
        bool failed = false;
        // Decoded: one picture, or an animation's frames, all of one size.
        // Empty until first wanted, and again once let go (Trim).
        std::vector<std::shared_ptr<const gpu::TextureData>> pictures;
        size_t bytes = 0;     // of `pictures`
        bool gamma = false;   // how the pictures' levels were averaged
        uint64_t used = 0;    // State::clock when it was last asked for

        // A switchable texture (Switchable): the group it answers to; what
        // Select last chose for it, empty for none; and how many times it has
        // chosen, which with the frame is what an image says it shows.
        uint32_t group = 0;
        bool switchable = false;
        std::vector<fs::path> chosen;
        double chosenFps = 12.0;
        gpu::replace::Effect chosenEffect = gpu::replace::Effect::None;
        gpu::replace::Alpha chosenAlpha = gpu::replace::Alpha::Picture;
        // `request` counts what was asked for (Select) and `choice` what the
        // pictures in hand are of: the second moves only as `pictures` does,
        // so an image never marks a choice shown while it still holds the
        // last one's pictures and then keeps them.
        uint32_t request = 0;
        uint32_t choice = 0;
        // The original's format and the size of its keyed level, known from
        // its first Load on; every picture of a switchable texture is made
        // that size, so that one image can take any of them.
        bool shaped = false;
        uint32_t format = 0, width = 0, height = 0;
        // The title's own picture in a replacement's form, to go back to.
        std::shared_ptr<const gpu::TextureData> own;
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
        size_t animated = 0;   // entries of more than one frame
        uint64_t advanced = 0; // frames handed out to be staged over another (Due)
        // What an animation's frame is counted from.
        std::chrono::steady_clock::time_point started;
        // Whether any texture is switchable, which also turns replacement on.
        std::atomic<bool> switchable{ false };
    };
    State s;

    bool AllOf(const std::string& text, int (*is)(int))
    {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(), [is](char c) { return is(uint8_t(c)) != 0; });
    }

    // A key as the dump writes it; zero for anything else.
    uint64_t KeyNamed(const std::string& text)
    {
        if (text.size() > 16 || !AllOf(text, std::isxdigit)) return 0;
        return std::strtoull(text.c_str(), nullptr, 16);
    }

    // `fps=<n>` from an animation's `<key>.txt`; zero when it has none.
    double RateIn(const fs::path& file)
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            line.erase(std::remove_if(line.begin(), line.end(),
                                      [](char c) { return std::isspace(uint8_t(c)) != 0; }),
                       line.end());
            for (char& c : line) c = char(std::tolower(uint8_t(c)));
            if (line.rfind("fps=", 0) != 0) continue;
            const double rate = std::strtod(line.c_str() + 4, nullptr);
            if (rate > 0.0) return std::clamp(rate, 0.1, 120.0);
        }
        return 0.0;
    }

    // The folder's files by the key in their names. PNG is preferred where a
    // key has both, as the one that can hold alpha. `<key>.<n>.png`, and
    // `<n>.png` in a folder named by the sixteen digits of a key, are frame n
    // of that key's animation; `<key>.txt` holds its rate.
    bool Start()
    {
        std::error_code ec;
        s.started = std::chrono::steady_clock::now();
        if (const char* named = env::Text("MW2_TEXTURE_REPLACE")) s.folder = FromUtf8(named);
        else if (const fs::path beside = install::GameFolder() / "replace"; fs::is_directory(beside, ec))
            s.folder = beside;
        if (!s.folder.empty())
        {
            std::unordered_map<uint64_t, std::map<uint32_t, fs::path>> frames;
            std::unordered_map<uint64_t, double> rates;
            const fs::recursive_directory_iterator end;
            for (fs::recursive_directory_iterator at(
                     s.folder, fs::directory_options::skip_permission_denied, ec);
                 !ec && at != end; at.increment(ec))
            {
                if (!at->is_regular_file(ec)) continue;
                const fs::path file = at->path();
                std::string extension = Utf8(file.extension());
                for (char& c : extension) c = char(std::tolower(uint8_t(c)));
                const std::string stem = Utf8(file.stem());
                if (extension == ".txt")
                {
                    if (const uint64_t key = KeyNamed(stem))
                        if (const double rate = RateIn(file); rate > 0.0) rates[key] = rate;
                    continue;
                }
                const bool png = extension == ".png";
                if (!png && extension != ".pnm") continue;

                // A frame: by a number after the key, or by a number alone in
                // the key's folder. Nine digits keep it clear of a key.
                uint64_t of = 0;
                std::string number;
                if (const size_t dot = stem.rfind('.'); dot != std::string::npos)
                {
                    of = KeyNamed(stem.substr(0, dot));
                    number = stem.substr(dot + 1);
                }
                else if (const std::string parent = Utf8(file.parent_path().filename());
                         parent.size() == 16 && at.depth() > 0)
                {
                    of = KeyNamed(parent);
                    number = stem;
                }
                if (of && number.size() <= 9 && AllOf(number, std::isdigit))
                {
                    fs::path& frame = frames[of][uint32_t(std::strtoul(number.c_str(), nullptr, 10))];
                    if (frame.empty() || png) frame = file;
                    continue;
                }

                const uint64_t key = KeyNamed(stem);
                if (!key) continue;
                Entry& entry = s.index[key];
                if (entry.file.empty() || png) entry.file = file;
            }
            // Frames win over a single picture of the same key; one frame is
            // a single picture.
            for (auto& [key, numbered] : frames)
            {
                Entry& entry = s.index[key];
                entry.file = numbered.begin()->second;
                if (numbered.size() < 2) continue;
                for (auto& [number, file] : numbered) entry.frames.push_back(std::move(file));
                if (const auto rate = rates.find(key); rate != rates.end()) entry.fps = rate->second;
                s.animated++;
            }
            LOGI("textures: %zu replacements in %s, %zu of them animated", s.index.size(),
                 Utf8(fs::absolute(s.folder, ec)).c_str(), s.animated);
        }
        if (const char* dumps = env::Text("MW2_DUMP_TEXTURES"))
        {
            s.dumps = FromUtf8(dumps);
            fs::create_directories(s.dumps, ec);
            s.dumping = true;
        }
        return s.dumping || !s.index.empty();
    }

    // Called with the lock held. `file` is the one at fault, where that is
    // not the entry's first.
    void Fail(Entry& entry, const char* why, const fs::path* file = nullptr)
    {
        entry.failed = true;
        s.held -= entry.bytes;
        entry.bytes = 0;
        entry.pictures.clear();
        s.failed++;
        LOGW("textures: replacement %s is not used: %s", Utf8(file ? *file : entry.file).c_str(),
             why);
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

    // A picture's file as RGBA.
    bool ReadPicture(const fs::path& path, std::vector<uint8_t>& rgba, uint32_t& width,
                     uint32_t& height, const char*& error)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { error = "it cannot be opened"; return false; }
        std::vector<uint8_t> file(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(file.data()), std::streamsize(file.size()));
        if (!in) { error = "it cannot be read"; return false; }

        const bool isPng = file.size() >= 4 && std::memcmp(file.data(), "\x89PNG", 4) == 0;
        return isPng ? png::Read(file, rgba, width, height, error)
                     : ReadPnm(file, rgba, width, height, error);
    }

    // RGBA at another size: each texel the average of the area it covers
    // when the picture shrinks, the nearest when it grows.
    void Resize(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t toWidth,
                uint32_t toHeight)
    {
        if (width == toWidth && height == toHeight) return;
        std::vector<uint8_t> out(size_t(toWidth) * toHeight * 4);
        for (uint32_t y = 0; y < toHeight; y++)
        {
            const uint32_t y0 = uint32_t(uint64_t(y) * height / toHeight);
            const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * height / toHeight));
            for (uint32_t x = 0; x < toWidth; x++)
            {
                const uint32_t x0 = uint32_t(uint64_t(x) * width / toWidth);
                const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * width / toWidth));
                uint32_t sum[4] = {};
                for (uint32_t v = y0; v < y1; v++)
                    for (uint32_t u = x0; u < x1; u++)
                        for (int channel = 0; channel < 4; channel++)
                            sum[channel] += rgba[(size_t(v) * width + u) * 4 + channel];
                const uint32_t count = (y1 - y0) * (x1 - x0);
                for (int channel = 0; channel < 4; channel++)
                    out[(size_t(y) * toWidth + x) * 4 + channel] = uint8_t((sum[channel] + count / 2) / count);
            }
        }
        rgba = std::move(out);
        width = toWidth;
        height = toHeight;
    }

    // RGBA in `out.bytes` as the texture the cache uploads in the original's
    // place. `format` is the original's.
    void Shape(uint32_t width, uint32_t height, uint32_t format, bool gamma, gpu::TextureData& out)
    {
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
    }

    // The alpha of `title`, a picture of the same size in a replacement's
    // form, over RGBA that is opaque throughout.
    void TakeAlpha(std::vector<uint8_t>& rgba, const gpu::TextureData& title)
    {
        const size_t size = rgba.size();
        if (title.levels.empty() || title.levels[0].size != size) return;
        for (size_t at = 3; at < size; at += 4)
            if (rgba[at] != 255) return;
        const uint8_t* from = title.bytes.data() + title.levels[0].offset;
        for (size_t at = 3; at < size; at += 4) rgba[at] = from[at];
    }

    // A replacement's file as the texture the cache uploads in the original's
    // place, at the size given when there is one, and with the alpha of
    // `alphaOf` when that is given and the file has none.
    bool Decode(const fs::path& path, uint32_t format, bool gamma, gpu::TextureData& out,
                const char*& error, uint32_t toWidth = 0, uint32_t toHeight = 0,
                const gpu::TextureData* alphaOf = nullptr)
    {
        uint32_t width = 0, height = 0;
        if (!ReadPicture(path, out.bytes, width, height, error)) return false;
        if (toWidth && toHeight) Resize(out.bytes, width, height, toWidth, toHeight);
        if (alphaOf) TakeAlpha(out.bytes, *alphaOf);
        Shape(width, height, format, gamma, out);
        return true;
    }

    // Frame `frame` of kEffectFrames of the still picture `rgba` under
    // `effect`. Every effect is periodic over the picture, which is drawn
    // tiled, and over the frames.
    std::vector<uint8_t> Moved(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                               gpu::replace::Effect effect, uint32_t frame)
    {
        using gpu::replace::Effect;
        constexpr float kTurn = 6.2831853f;
        const float phase = float(frame) / float(gpu::replace::kEffectFrames);
        std::vector<uint8_t> out(rgba.size());
        const auto texel = [&](uint32_t x, uint32_t y) { return rgba.data() + (size_t(y % height) * width + x % width) * 4; };
        // Hue: a rotation about the grey axis, which keeps a colour's lightness.
        const float c = std::cos(phase * kTurn), s = std::sin(phase * kTurn);
        const float diagonal = (1.0f + 2.0f * c) / 3.0f, plus = (1.0f - c) / 3.0f + s * 0.57735f,
                    minus = (1.0f - c) / 3.0f - s * 0.57735f;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                uint8_t* to = out.data() + (size_t(y) * width + x) * 4;
                const uint8_t* from = texel(x, y);
                if (effect == Effect::Scroll)
                {
                    // A whole width and height a loop, between texels by weight.
                    const float u = float(x) + phase * float(width), v = float(y) + phase * float(height);
                    const uint32_t x0 = uint32_t(u), y0 = uint32_t(v);
                    const float fx = u - float(x0), fy = v - float(y0);
                    const uint8_t *a = texel(x0, y0), *b = texel(x0 + 1, y0), *d = texel(x0, y0 + 1),
                                  *e = texel(x0 + 1, y0 + 1);
                    for (int channel = 0; channel < 4; channel++)
                        to[channel] = uint8_t((a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
                                              (d[channel] * (1 - fx) + e[channel] * fx) * fy + 0.5f);
                }
                else if (effect == Effect::Shimmer)
                {
                    const float wave = 0.5f + 0.5f * std::cos((float(x) / float(width) + float(y) / float(height) - phase) * kTurn);
                    const float light = 1.0f + 0.9f * std::pow(wave, 8.0f);
                    for (int channel = 0; channel < 3; channel++)
                        to[channel] = uint8_t(std::min(255.0f, from[channel] * light));
                    to[3] = from[3];
                }
                else
                {
                    const float r = from[0], g = from[1], b = from[2];
                    to[0] = uint8_t(std::clamp(r * diagonal + g * minus + b * plus, 0.0f, 255.0f));
                    to[1] = uint8_t(std::clamp(r * plus + g * diagonal + b * minus, 0.0f, 255.0f));
                    to[2] = uint8_t(std::clamp(r * minus + g * plus + b * diagonal, 0.0f, 255.0f));
                    to[3] = from[3];
                }
            }
        return out;
    }

    // The pictures of a switchable texture as things stand: what was chosen,
    // else the folder's replacement, else the title's own. Called without the
    // lock, with what it reads of the entry copied out under it.
    std::vector<std::shared_ptr<const gpu::TextureData>> Chosen(
        const std::vector<fs::path>& files, const std::shared_ptr<const gpu::TextureData>& own,
        uint32_t format, bool gamma, uint32_t width, uint32_t height, gpu::replace::Effect effect,
        gpu::replace::Alpha alpha)
    {
        const gpu::TextureData* alphaOf = alpha == gpu::replace::Alpha::Title ? own.get() : nullptr;
        std::vector<std::shared_ptr<const gpu::TextureData>> pictures;
        if (files.size() == 1 && effect != gpu::replace::Effect::None)
        {
            std::vector<uint8_t> still;
            uint32_t w = 0, h = 0;
            const char* error = nullptr;
            if (ReadPicture(files[0], still, w, h, error))
            {
                Resize(still, w, h, width, height);
                if (alphaOf) TakeAlpha(still, *alphaOf);
                for (uint32_t frame = 0; frame < gpu::replace::kEffectFrames; frame++)
                {
                    const auto made = std::make_shared<gpu::TextureData>();
                    made->bytes = Moved(still, w, h, effect, frame);
                    Shape(w, h, format, gamma, *made);
                    pictures.push_back(made);
                }
                return pictures;
            }
        }
        // An image's frame is a byte of what it says it shows (Showing).
        for (size_t i = 0; i < files.size() && i < 255; i++)
        {
            const auto decoded = std::make_shared<gpu::TextureData>();
            const char* error = nullptr;
            if (!Decode(files[i], format, gamma, *decoded, error, width, height, alphaOf))
            {
                LOGW("textures: %s is not used: %s", Utf8(files[i]).c_str(),
                     error ? error : "it does not decode");
                continue;
            }
            pictures.push_back(decoded);
        }
        if (pictures.empty()) pictures.push_back(own);
        return pictures;
    }

    // The decoded pictures back under the bound, the least recently asked for
    // going first; never `keep`, which is the one just decoded. Called with
    // the lock held. An image being made from one that goes still holds it.
    // An animation is asked for at every frame it advances (Due), so one on
    // screen is the last to go; one that does go stays on the frame its
    // image shows until that image is made again.
    void Trim(uint64_t keep)
    {
        while (s.held > kHeldMost)
        {
            Entry* oldest = nullptr;
            for (auto& [key, entry] : s.index)
                if (!entry.pictures.empty() && key != keep && !entry.switchable &&
                    (!oldest || entry.used < oldest->used))
                    oldest = &entry;
            if (!oldest) return;
            if (oldest->pictures.size() > 1)
                LOGW("textures: animation %s let go to keep decoded replacements under %zu MB; it"
                     " stands still until its texture is made again",
                     Utf8(oldest->file).c_str(), kHeldMost >> 20);
            s.held -= oldest->bytes;
            oldest->bytes = 0;
            oldest->pictures.clear();
        }
    }

    // The frame of an animation the clock is at. Called with the lock held.
    uint32_t FrameNow(const Entry& entry)
    {
        if (entry.pictures.size() < 2) return 0;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started).count();
        const double fps = entry.switchable && !entry.chosen.empty() ? entry.chosenFps : entry.fps;
        return uint32_t(uint64_t(seconds * fps) % entry.pictures.size());
    }

    // What an image of a switchable texture shows: the choice and its frame,
    // so that a new choice is due as a new frame is. Called with the lock held.
    uint32_t Showing(const Entry& entry)
    {
        return (entry.choice << 8 | FrameNow(entry)) & 0x7FFFFFFFu;
    }

    // The files a switchable texture shows. Called with the lock held.
    std::vector<fs::path> FilesOf(const Entry& entry)
    {
        if (!entry.chosen.empty()) return entry.chosen;
        if (!entry.frames.empty()) return entry.frames;
        if (!entry.file.empty()) return { entry.file };
        return {};
    }
}

bool gpu::replace::On()
{
    static const bool on = Start();
    return on || s.switchable.load(std::memory_order_relaxed);
}

void gpu::replace::Switchable(uint32_t group, uint64_t key)
{
    std::lock_guard lock(s.lock);
    Entry& entry = s.index[key];
    entry.group = group;
    entry.switchable = true;
    s.switchable.store(true, std::memory_order_relaxed);
}

void gpu::replace::Select(uint32_t group, const std::vector<std::filesystem::path>& frames,
                          double fps, Effect effect, Alpha alpha)
{
    // Decoded here, on the caller's thread, for the textures whose shape is
    // known: the renderer then only copies (Due). The rest decode when they
    // are first read (Load).
    struct Shaped
    {
        Entry* entry;
        uint32_t request, format, width, height;
        bool gamma;
        Effect effect;
        Alpha alpha;
        std::vector<fs::path> files;
        std::shared_ptr<const gpu::TextureData> own;
    };
    std::vector<Shaped> shaped;
    {
        std::lock_guard lock(s.lock);
        for (auto& [key, entry] : s.index)
        {
            if (!entry.switchable || entry.group != group) continue;
            if (entry.chosen == frames && entry.chosenFps == fps && entry.chosenEffect == effect &&
                entry.chosenAlpha == alpha)
                continue;
            entry.chosen = frames;
            entry.chosenFps = fps;
            entry.chosenEffect = effect;
            entry.chosenAlpha = alpha;
            entry.request++;
            if (entry.shaped)
                shaped.push_back({ &entry, entry.request, entry.format, entry.width, entry.height,
                                   entry.gamma, frames.empty() ? Effect::None : effect,
                                   frames.empty() ? Alpha::Picture : alpha, FilesOf(entry), entry.own });
        }
    }
    for (Shaped& one : shaped)
    {
        auto pictures = Chosen(one.files, one.own, one.format, one.gamma, one.width, one.height, one.effect,
                               one.alpha);
        std::lock_guard lock(s.lock);
        // Chosen again meanwhile: that call's pictures are the ones to keep.
        if (one.entry->request != one.request) continue;
        one.entry->pictures = std::move(pictures);
        one.entry->choice++;
    }
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
                                                           const TextureFetch& fetch, bool gamma,
                                                           uint32_t* frame)
{
    if (frame) *frame = kStill;
    if (!key) return nullptr;
    // An entry is never removed, so it stays where it is while the file is
    // decoded outside the lock: two textures decode side by side.
    Entry* entry = nullptr;
    bool switchable = false;
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
        switchable = entry->switchable;
        if (!entry->pictures.empty() && entry->gamma == gamma)
        {
            entry->used = ++s.clock;
            const uint32_t now = FrameNow(*entry);
            if (frame && switchable) *frame = Showing(*entry);
            else if (frame && entry->pictures.size() > 1) *frame = now;
            return entry->pictures[now];
        }
    }

    if (switchable)
    {
        // The title's own picture first, in a replacement's form: the image
        // is made of that when nothing is chosen, and takes what is chosen
        // later as an animation takes its next frame.
        const uint32_t keyed = KeyedLevel(original);
        const auto own = std::make_shared<TextureData>();
        if (!DecodeToRgba(original, fetch.Format(), own->bytes, keyed))
        {
            Refuse(key, "its format is not one that decodes, so it cannot be switched");
            return nullptr;
        }
        const uint32_t width = original.levels[keyed].width, height = original.levels[keyed].height;
        Shape(width, height, fetch.Format(), gamma, *own);

        std::vector<fs::path> files;
        uint32_t request = 0;
        Effect effect = Effect::None;
        Alpha alpha = Alpha::Picture;
        {
            std::lock_guard lock(s.lock);
            entry->shaped = true;
            entry->format = fetch.Format();
            entry->width = width;
            entry->height = height;
            entry->gamma = gamma;
            entry->own = own;
            files = FilesOf(*entry);
            request = entry->request;
            // The effect and the alpha belong to what was chosen, not to the folder's or the title's picture.
            if (!entry->chosen.empty())
            {
                effect = entry->chosenEffect;
                alpha = entry->chosenAlpha;
            }
        }
        auto pictures = Chosen(files, own, fetch.Format(), gamma, width, height, effect, alpha);
        std::lock_guard lock(s.lock);
        // Chosen again meanwhile, and decoded there now that the shape is known.
        if (entry->request == request || entry->pictures.empty())
        {
            entry->pictures = std::move(pictures);
            entry->choice++;
        }
        if (frame) *frame = Showing(*entry);
        return entry->pictures[FrameNow(*entry)];
    }

    // Every frame now, so that showing the next one later is a copy and no
    // more; but no more frames than the bound has room for, which the first
    // says, as they are all its size.
    const char* error = nullptr;
    const fs::path* at = &entry->file;
    const size_t files = entry->frames.empty() ? 1 : entry->frames.size();
    size_t count = files;
    std::vector<std::shared_ptr<const TextureData>> pictures;
    size_t bytes = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++)
    {
        if (!entry->frames.empty()) at = &entry->frames[i];
        const auto decoded = std::make_shared<TextureData>();
        ok = Decode(*at, fetch.Format(), gamma, *decoded, error);
        if (!ok) break;
        if (i == 0)
            count = std::clamp<size_t>(kHeldMost / decoded->bytes.size(), 1, files);
        else if (decoded->width != pictures[0]->width || decoded->height != pictures[0]->height)
        {
            error = "it is not the size of the animation's first frame";
            ok = false;
            break;
        }
        bytes += decoded->bytes.size();
        pictures.push_back(decoded);
    }

    std::lock_guard lock(s.lock);
    if (entry->failed) return nullptr;
    if (!ok)
    {
        Fail(*entry, error ? error : "it does not decode", at);
        return nullptr;
    }
    if (pictures.size() > 1)
    {
        LOGI("textures: replacement %s read, %ux%u, %zu frames at %g a second",
             Utf8(entry->file).c_str(), pictures[0]->width, pictures[0]->height,
             pictures.size(), entry->fps);
    }
    else
    {
        LOGI("textures: replacement %s read, %ux%u", Utf8(entry->file).c_str(),
             pictures[0]->width, pictures[0]->height);
    }
    if (count < files)
        LOGW("textures: animation %s shows the first %zu of its %zu frames: more would pass the"
             " %zu MB kept of decoded replacements", Utf8(entry->file).c_str(), count, files,
             kHeldMost >> 20);
    s.held -= entry->bytes;
    entry->pictures = std::move(pictures);
    entry->bytes = bytes;
    entry->gamma = gamma;
    entry->used = ++s.clock;
    s.held += bytes;
    Trim(key);
    const uint32_t now = FrameNow(*entry);
    if (frame && entry->pictures.size() > 1) *frame = now;
    return entry->pictures[now];
}

std::shared_ptr<const gpu::TextureData> gpu::replace::Due(uint64_t key, uint32_t& shown)
{
    std::lock_guard lock(s.lock);
    const auto found = s.index.find(key);
    if (found == s.index.end()) return nullptr;
    Entry& entry = found->second;
    if (entry.switchable)
    {
        if (entry.pictures.empty()) return nullptr;
        const uint32_t now = Showing(entry);
        if (now == shown) return nullptr;
        shown = now;
        s.advanced++;
        return entry.pictures[now & 0xFF];
    }
    // Let go, or decoded again as fewer frames: never decoded here.
    if (entry.pictures.size() < 2) return nullptr;
    const uint32_t now = FrameNow(entry);
    if (now == shown) return nullptr;
    shown = now;
    entry.used = ++s.clock;
    s.advanced++;
    return entry.pictures[now];
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
    LOGI("textures: %zu replacements indexed in %s, %zu of them animated; images made from them"
         " %llu times, %llu failed; an animated image was given its next frame %llu times",
         s.index.size(), Utf8(fs::absolute(s.folder, ec)).c_str(), s.animated,
         (unsigned long long)s.applied, (unsigned long long)s.failed,
         (unsigned long long)s.advanced);
}

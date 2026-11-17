// PNG reading and writing (png.h).
#include "png.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
    uint32_t Be32(const uint8_t* p)
    {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
    }

    void PutBe32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
    }

    // ---- inflate (RFC 1951) ------------------------------------------------

    // The stream, least significant bit first. Past its end it reads as
    // zeroes, and `left` going below zero says that happened: checked once a
    // symbol rather than at every read.
    struct Bits
    {
        Bits(const uint8_t* data, size_t size)
            : at(data), end(data + size), left(int64_t(size) * 8) {}

        // At most sixteen.
        uint32_t Peek(uint32_t bits)
        {
            while (count <= 56)
            {
                hold |= uint64_t(at < end ? *at++ : uint8_t(0)) << count;
                count += 8;
            }
            return uint32_t(hold) & ((1u << bits) - 1);
        }
        void Drop(uint32_t bits)
        {
            hold >>= bits;
            count -= bits;
            left -= bits;
        }
        uint32_t Take(uint32_t bits)
        {
            const uint32_t value = Peek(bits);
            Drop(bits);
            return value;
        }

        const uint8_t* at;
        const uint8_t* end;
        uint64_t hold = 0;
        uint32_t count = 0;
        int64_t left;
    };

    // A canonical Huffman code as one table indexed by the next `bits` bits of
    // the stream: the symbol in the high twelve bits of an entry, its code's
    // length in the low four, zero where no code leads. A code shorter than
    // `bits` fills every entry it is a prefix of, so decoding is one lookup
    // whatever the length -- a bit at a time was several times slower, and a
    // replacement is inflated while a frame waits for it.
    struct Huffman
    {
        std::vector<uint16_t> table;
        uint32_t bits = 1;
    };

    bool Build(Huffman& code, const uint8_t* lengths, uint32_t count)
    {
        uint32_t perLength[16] = {};
        for (uint32_t i = 0; i < count; i++) perLength[lengths[i]]++;
        perLength[0] = 0;
        code.bits = 15;
        while (code.bits > 1 && !perLength[code.bits]) code.bits--;

        uint32_t next[16] = {};
        uint32_t first = 0;
        for (uint32_t length = 1; length <= 15; length++)
        {
            first = (first + perLength[length - 1]) << 1;
            next[length] = first;
            // More codes of this length than there are left to give.
            if (first + perLength[length] > (1u << length)) return false;
        }

        code.table.assign(size_t(1) << code.bits, 0);
        for (uint32_t symbol = 0; symbol < count; symbol++)
        {
            const uint32_t length = lengths[symbol];
            if (!length) continue;
            // The stream holds a code most significant bit first.
            const uint32_t forward = next[length]++;
            uint32_t reversed = 0;
            for (uint32_t i = 0; i < length; i++)
                reversed |= ((forward >> i) & 1) << (length - 1 - i);
            for (size_t at = reversed; at < code.table.size(); at += size_t(1) << length)
                code.table[at] = uint16_t(symbol << 4 | length);
        }
        return true;
    }

    // The next symbol, or -1 where the bits are no code.
    int Decode(Bits& in, const Huffman& code)
    {
        const uint16_t entry = code.table[in.Peek(code.bits)];
        if (!(entry & 15)) return -1;
        in.Drop(entry & 15);
        return entry >> 4;
    }

    // A zlib stream (RFC 1950) into `out`, which is already the size the
    // stream must come to: a PNG says how many bytes its rows are. False when
    // the stream is damaged or comes to anything else.
    bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
    {
        static constexpr uint16_t kLengthBase[29] = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258 };
        static constexpr uint8_t kLengthExtra[29] = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static constexpr uint16_t kDistanceBase[30] = {
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
            1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static constexpr uint8_t kDistanceExtra[30] = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
            12, 13, 13 };
        // The order a dynamic block lists its code-length code's lengths in.
        static constexpr uint8_t kOrder[19] = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

        // Deflate with a window of at most 32 KB, and no preset dictionary.
        if (size < 2 || (data[0] & 0x0F) != 8 || (data[1] & 0x20)) return false;
        Bits in(data + 2, size - 2);
        size_t written = 0;
        Huffman literals, distances;
        for (bool last = false; !last;)
        {
            last = in.Take(1) != 0;
            const uint32_t type = in.Take(2);
            if (type == 0)
            {
                // Stored: the rest of this byte is skipped, then a length, its
                // complement and that many bytes as they are.
                in.Drop(in.count & 7);
                size_t length = in.Take(16);
                if ((length ^ 0xFFFF) != in.Take(16)) return false;
                if (length > out.size() - written) return false;
                // What is buffered ahead goes first, straight from the buffer:
                // a read would buffer more, and the rest is copied whole.
                while (length && in.count)
                {
                    out[written++] = uint8_t(in.hold);
                    in.Drop(8);
                    length--;
                }
                if (size_t(in.end - in.at) < length) return false;
                std::memcpy(out.data() + written, in.at, length);
                in.at += length;
                in.left -= int64_t(length) * 8;
                written += length;
                if (in.left < 0) return false;
                continue;
            }
            if (type == 3) return false;

            uint8_t lengths[320] = {};
            uint32_t literalCount = 288, distanceCount = 30;
            if (type == 1)
            {
                for (uint32_t i = 0; i < 288; i++)
                    lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                for (uint32_t i = 0; i < 30; i++) lengths[288 + i] = 5;
            }
            else
            {
                literalCount = in.Take(5) + 257;
                distanceCount = in.Take(5) + 1;
                const uint32_t orderCount = in.Take(4) + 4;
                if (literalCount > 286 || distanceCount > 30) return false;
                uint8_t orderLengths[19] = {};
                for (uint32_t i = 0; i < orderCount; i++) orderLengths[kOrder[i]] = uint8_t(in.Take(3));
                Huffman order;
                if (!Build(order, orderLengths, 19)) return false;
                for (uint32_t at = 0; at < literalCount + distanceCount;)
                {
                    const int symbol = Decode(in, order);
                    if (symbol < 0 || in.left < 0) return false;
                    if (symbol < 16) { lengths[at++] = uint8_t(symbol); continue; }
                    uint8_t repeated = 0;
                    uint32_t times = 0;
                    if (symbol == 16)
                    {
                        if (!at) return false;
                        repeated = lengths[at - 1];
                        times = 3 + in.Take(2);
                    }
                    else if (symbol == 17) times = 3 + in.Take(3);
                    else                   times = 11 + in.Take(7);
                    if (at + times > literalCount + distanceCount) return false;
                    while (times--) lengths[at++] = repeated;
                }
            }
            if (!Build(literals, lengths, literalCount) ||
                !Build(distances, lengths + literalCount, distanceCount))
                return false;

            for (;;)
            {
                if (in.left < 0) return false;
                int symbol = Decode(in, literals);
                if (symbol < 0) return false;
                if (symbol < 256)
                {
                    if (written == out.size()) return false;
                    out[written++] = uint8_t(symbol);
                    continue;
                }
                if (symbol == 256) break;
                symbol -= 257;
                if (symbol >= 29) return false;
                const size_t length = kLengthBase[symbol] + in.Take(kLengthExtra[symbol]);
                const int reach = Decode(in, distances);
                if (reach < 0 || reach >= 30) return false;
                const size_t distance = kDistanceBase[reach] + in.Take(kDistanceExtra[reach]);
                if (distance > written || length > out.size() - written) return false;
                // A byte at a time: the two ranges overlap whenever the
                // distance is shorter than the length, which is how a run is
                // written.
                for (size_t i = 0; i < length; i++, written++)
                    out[written] = out[written - distance];
            }
        }
        return in.left >= 0 && written == out.size();
    }

    // ---- the checks a file carries -----------------------------------------

    uint32_t Crc(const uint8_t* bytes, size_t size, uint32_t crc)
    {
        struct Table
        {
            uint32_t entry[256];
            Table()
            {
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t value = i;
                    for (int bit = 0; bit < 8; bit++)
                        value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
                    entry[i] = value;
                }
            }
        };
        static const Table table;
        crc = ~crc;
        for (size_t i = 0; i < size; i++) crc = table.entry[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    uint32_t Adler(const uint8_t* bytes, size_t size)
    {
        uint32_t a = 1, b = 0;
        // As many bytes as the sums take before they can overflow.
        for (size_t at = 0; at < size;)
        {
            const size_t until = at + (size - at < 5552 ? size - at : 5552);
            for (; at < until; at++) { a += bytes[at]; b += a; }
            a %= 65521;
            b %= 65521;
        }
        return b << 16 | a;
    }

    void PutChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* payload, size_t size)
    {
        PutBe32(out, uint32_t(size));
        const size_t from = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload, payload + size);
        PutBe32(out, Crc(out.data() + from, out.size() - from, 0));
    }

    constexpr uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
}

bool png::Read(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba, uint32_t& width,
               uint32_t& height, const char*& error)
{
    if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0)
    {
        error = "not a PNG file";
        return false;
    }
    uint32_t depth = 0, colour = 0, interlace = 0;
    bool header = false;
    std::vector<uint8_t> packed, palette, transparency;
    for (size_t at = 8; at + 12 <= file.size();)
    {
        const uint32_t size = Be32(file.data() + at);
        const uint8_t* type = file.data() + at + 4;
        const uint8_t* payload = type + 4;
        if (size > file.size() - at - 12) { error = "a truncated PNG file"; return false; }
        if (!std::memcmp(type, "IHDR", 4) && size >= 13)
        {
            width = Be32(payload);
            height = Be32(payload + 4);
            depth = payload[8];
            colour = payload[9];
            interlace = payload[12];
            header = true;
        }
        else if (!std::memcmp(type, "PLTE", 4)) palette.assign(payload, payload + size);
        else if (!std::memcmp(type, "tRNS", 4)) transparency.assign(payload, payload + size);
        else if (!std::memcmp(type, "IDAT", 4)) packed.insert(packed.end(), payload, payload + size);
        else if (!std::memcmp(type, "IEND", 4)) break;
        at += size_t(size) + 12;
    }
    if (!header || packed.empty()) { error = "a PNG file with no picture in it"; return false; }
    if (interlace) { error = "an interlaced PNG; save it without interlacing"; return false; }

    // Channels a pixel, by colour type; zero for one the format does not have.
    const uint32_t channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1
                            : colour == 4 ? 2 : colour == 6 ? 4 : 0;
    const bool whole = depth == 8 || depth == 16;
    const bool part = depth == 1 || depth == 2 || depth == 4;
    if (!channels || !(colour == 3 ? part || depth == 8 : whole || (colour == 0 && part)))
    {
        error = "a PNG of a colour type or bit depth the format does not have";
        return false;
    }
    // Sixty-four million pixels is an 8192x8192 picture, past any device's
    // largest image; beyond it the sizes below are not worth computing.
    if (!width || !height || uint64_t(width) * height > (64ull << 20))
    {
        error = "a PNG of no size, or too large";
        return false;
    }

    const size_t stride = (size_t(width) * channels * depth + 7) / 8;
    // What a filter calls the pixel to the left: so many bytes back.
    const size_t step = channels * depth >= 8 ? channels * depth / 8 : 1;
    std::vector<uint8_t> rows((stride + 1) * height);
    if (!Inflate(packed.data(), packed.size(), rows))
    {
        error = "a PNG whose picture does not decompress";
        return false;
    }

    rgba.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t* row = rows.data() + (stride + 1) * y + 1;
        const uint8_t* above = y ? row - (stride + 1) : nullptr;
        switch (row[-1])
        {
        case 0: break;
        case 1:
            for (size_t i = step; i < stride; i++) row[i] = uint8_t(row[i] + row[i - step]);
            break;
        case 2:
            if (above) for (size_t i = 0; i < stride; i++) row[i] = uint8_t(row[i] + above[i]);
            break;
        case 3:
            for (size_t i = 0; i < stride; i++)
            {
                const uint32_t left = i >= step ? row[i - step] : 0, up = above ? above[i] : 0;
                row[i] = uint8_t(row[i] + ((left + up) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < stride; i++)
            {
                const int left = i >= step ? row[i - step] : 0, up = above ? above[i] : 0;
                const int corner = above && i >= step ? above[i - step] : 0;
                const int guess = left + up - corner;
                const int byLeft = std::abs(guess - left), byUp = std::abs(guess - up);
                const int byCorner = std::abs(guess - corner);
                const int nearest = byLeft <= byUp && byLeft <= byCorner ? left
                                  : byUp <= byCorner                     ? up
                                                                         : corner;
                row[i] = uint8_t(row[i] + nearest);
            }
            break;
        default:
            error = "a PNG with a row filter the format does not have";
            return false;
        }

        // One channel of one pixel: the high byte of sixteen bits, and below
        // eight the packed value as it is, which only one-channel pictures have.
        const auto sample = [&](uint32_t x, uint32_t channel) -> uint32_t {
            if (depth == 8) return row[size_t(x) * channels + channel];
            if (depth == 16) return row[(size_t(x) * channels + channel) * 2];
            const size_t bit = size_t(x) * depth;
            return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
        };
        uint8_t* out = rgba.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; x++, out += 4)
        {
            switch (colour)
            {
            case 0:
            {
                const uint32_t grey = part ? sample(x, 0) * 255 / ((1u << depth) - 1) : sample(x, 0);
                out[0] = out[1] = out[2] = uint8_t(grey);
                out[3] = 255;
                break;
            }
            case 2:
                out[0] = uint8_t(sample(x, 0));
                out[1] = uint8_t(sample(x, 1));
                out[2] = uint8_t(sample(x, 2));
                out[3] = 255;
                break;
            case 3:
            {
                // An index past the palette is a damaged file; it stays black.
                const size_t index = sample(x, 0);
                if (index * 3 + 2 < palette.size())
                    std::memcpy(out, palette.data() + index * 3, 3);
                else
                    out[0] = out[1] = out[2] = 0;
                out[3] = index < transparency.size() ? transparency[index] : 255;
                break;
            }
            case 4:
                out[0] = out[1] = out[2] = uint8_t(sample(x, 0));
                out[3] = uint8_t(sample(x, 1));
                break;
            default:
                out[0] = uint8_t(sample(x, 0));
                out[1] = uint8_t(sample(x, 1));
                out[2] = uint8_t(sample(x, 2));
                out[3] = uint8_t(sample(x, 3));
                break;
            }
        }
    }
    return true;
}

bool png::Write(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
                uint32_t height)
{
    if (!width || !height) return false;
    // Each row behind the byte that names its filter: none.
    const size_t rowBytes = size_t(width) * 4;
    std::vector<uint8_t> rows((rowBytes + 1) * height);
    for (uint32_t y = 0; y < height; y++)
        std::memcpy(rows.data() + (rowBytes + 1) * y + 1, rgba + rowBytes * y, rowBytes);

    // A zlib stream of stored blocks, 65535 bytes at most each.
    constexpr size_t kBlock = 65535;
    std::vector<uint8_t> packed;
    packed.reserve(rows.size() + (rows.size() / kBlock + 1) * 5 + 6);
    packed.push_back(0x78);
    packed.push_back(0x01);
    for (size_t at = 0; at < rows.size();)
    {
        const size_t length = rows.size() - at < kBlock ? rows.size() - at : kBlock;
        packed.push_back(at + length == rows.size() ? 1 : 0);
        packed.push_back(uint8_t(length));
        packed.push_back(uint8_t(length >> 8));
        packed.push_back(uint8_t(~length));
        packed.push_back(uint8_t(~length >> 8));
        packed.insert(packed.end(), rows.begin() + at, rows.begin() + at + length);
        at += length;
    }
    PutBe32(packed, Adler(rows.data(), rows.size()));
    // A chunk's length is 31 bits.
    if (packed.size() > 0x7FFFFFFFu) return false;

    std::vector<uint8_t> file(kSignature, kSignature + 8);
    file.reserve(packed.size() + 64);
    std::vector<uint8_t> header;
    PutBe32(header, width);
    PutBe32(header, height);
    // Eight bits a channel, RGBA, and the only compression, filtering and
    // interlacing there are: deflate, per row, none.
    for (const uint8_t byte : { 8, 6, 0, 0, 0 }) header.push_back(byte);
    PutChunk(file, "IHDR", header.data(), header.size());
    PutChunk(file, "IDAT", packed.data(), packed.size());
    PutChunk(file, "IEND", nullptr, 0);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    return bool(out);
}

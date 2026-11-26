// The hash functions of sha.h, from their definitions (FIPS 180-4).
#include "sha.h"

#include <cstring>

namespace
{
    uint32_t Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    uint64_t Rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

    uint32_t Big32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    uint64_t Big64(const uint8_t* p) { return uint64_t(Big32(p)) << 32 | Big32(p + 4); }
    void PutBig32(uint8_t* p, uint32_t x) { p[0] = uint8_t(x >> 24); p[1] = uint8_t(x >> 16); p[2] = uint8_t(x >> 8); p[3] = uint8_t(x); }
    void PutBig64(uint8_t* p, uint64_t x) { PutBig32(p, uint32_t(x >> 32)); PutBig32(p + 4, uint32_t(x)); }

    void Block(kernel::sha::Sha1& s, const uint8_t* block)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = Big32(block + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = s.state[0], b = s.state[1], c = s.state[2], d = s.state[3], e = s.state[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            const uint32_t t = Rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rotl(b, 30); b = a; a = t;
        }
        s.state[0] += a; s.state[1] += b; s.state[2] += c; s.state[3] += d; s.state[4] += e;
    }

    constexpr uint32_t kRound256[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    void Block(kernel::sha::Sha256& s, const uint8_t* block)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = Big32(block + 4 * i);
        for (int i = 16; i < 64; i++)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        std::memcpy(v, s.state, sizeof v);
        for (int i = 0; i < 64; i++)
        {
            const uint32_t s1 = Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25);
            const uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + kRound256[i] + w[i];
            const uint32_t s0 = Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22);
            const uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) s.state[i] += v[i];
    }

    constexpr uint64_t kRound512[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
        0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
        0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
        0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
        0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
        0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
        0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
        0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
        0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
        0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
        0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
        0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
        0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
    };

    void Block(kernel::sha::Sha512& s, const uint8_t* block)
    {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) w[i] = Big64(block + 8 * i);
        for (int i = 16; i < 80; i++)
        {
            const uint64_t s0 = Rotr(w[i - 15], 1) ^ Rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = Rotr(w[i - 2], 19) ^ Rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t v[8];
        std::memcpy(v, s.state, sizeof v);
        for (int i = 0; i < 80; i++)
        {
            const uint64_t s1 = Rotr(v[4], 14) ^ Rotr(v[4], 18) ^ Rotr(v[4], 41);
            const uint64_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + kRound512[i] + w[i];
            const uint64_t s0 = Rotr(v[0], 28) ^ Rotr(v[0], 34) ^ Rotr(v[0], 39);
            const uint64_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) s.state[i] += v[i];
    }

    // The bytes go through the state's buffer a block at a time.
    template <typename State>
    void Feed(State& s, const uint8_t* data, size_t size)
    {
        const size_t blockSize = sizeof s.buffer;
        size_t held = s.count % blockSize;
        s.count += uint32_t(size);
        while (size)
        {
            const size_t take = size < blockSize - held ? size : blockSize - held;
            std::memcpy(s.buffer + held, data, take);
            held += take; data += take; size -= take;
            if (held == blockSize) { Block(s, s.buffer); held = 0; }
        }
    }

    // The closing block or two: a one bit, zeroes, and the length in bits at
    // the very end (eight bytes of it, in sixteen for the 128-byte block).
    template <typename State>
    void Close(State& s)
    {
        const size_t blockSize = sizeof s.buffer, lengthSize = blockSize / 8;
        const uint64_t bits = uint64_t(s.count) * 8;
        size_t held = s.count % blockSize;
        s.buffer[held++] = 0x80;
        if (held > blockSize - lengthSize)
        {
            std::memset(s.buffer + held, 0, blockSize - held);
            Block(s, s.buffer);
            held = 0;
        }
        std::memset(s.buffer + held, 0, blockSize - held);
        PutBig64(s.buffer + blockSize - 8, bits);
        Block(s, s.buffer);
    }
}

namespace kernel::sha
{
    void Init(Sha1& s)
    {
        static constexpr uint32_t start[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        s.count = 0;
        std::memcpy(s.state, start, sizeof start);
    }
    void Init(Sha256& s)
    {
        static constexpr uint32_t start[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        s.count = 0;
        std::memcpy(s.state, start, sizeof start);
    }
    void Init384(Sha512& s)
    {
        static constexpr uint64_t start[8] = { 0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
                                               0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };
        s.count = 0;
        std::memcpy(s.state, start, sizeof start);
    }
    void Init512(Sha512& s)
    {
        static constexpr uint64_t start[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                               0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
        s.count = 0;
        std::memcpy(s.state, start, sizeof start);
    }

    void Update(Sha1& s, const uint8_t* data, size_t size)   { Feed(s, data, size); }
    void Update(Sha256& s, const uint8_t* data, size_t size) { Feed(s, data, size); }
    void Update(Sha512& s, const uint8_t* data, size_t size) { Feed(s, data, size); }

    void Final(Sha1& s, uint8_t out[kSha1Size])
    {
        Close(s);
        for (int i = 0; i < 5; i++) PutBig32(out + 4 * i, s.state[i]);
    }
    void Final(Sha256& s, uint8_t out[kSha256Size])
    {
        Close(s);
        for (int i = 0; i < 8; i++) PutBig32(out + 4 * i, s.state[i]);
    }
    void Final(Sha512& s, uint8_t out[kSha512Size])
    {
        Close(s);
        for (int i = 0; i < 8; i++) PutBig64(out + 8 * i, s.state[i]);
    }
}

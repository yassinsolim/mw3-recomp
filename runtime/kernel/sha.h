#pragma once
// SHA-1, SHA-256, SHA-384 and SHA-512, as the console's kernel offers them to
// a title (XeCryptSha*, xecrypt.cpp). Black Ops III is the first title here
// to import them. A state is what the kernel's own holds between calls: how
// many bytes went in, the running hash, and the bytes of an unfinished block.
#include <cstddef>
#include <cstdint>

namespace kernel::sha
{
    struct Sha1   { uint32_t count; uint32_t state[5]; uint8_t buffer[64]; };
    struct Sha256 { uint32_t count; uint32_t state[8]; uint8_t buffer[64]; };
    // SHA-384 is SHA-512 with other starting values and 48 bytes of its result.
    struct Sha512 { uint32_t count; uint64_t state[8]; uint8_t buffer[128]; };

    constexpr size_t kSha1Size = 20, kSha256Size = 32, kSha384Size = 48, kSha512Size = 64;

    void Init(Sha1& s);
    void Init(Sha256& s);
    void Init384(Sha512& s);
    void Init512(Sha512& s);
    void Update(Sha1& s, const uint8_t* data, size_t size);
    void Update(Sha256& s, const uint8_t* data, size_t size);
    void Update(Sha512& s, const uint8_t* data, size_t size);
    // The whole digest; a caller wanting fewer bytes takes the first of them.
    void Final(Sha1& s, uint8_t out[kSha1Size]);
    void Final(Sha256& s, uint8_t out[kSha256Size]);
    void Final(Sha512& s, uint8_t out[kSha512Size]);
}

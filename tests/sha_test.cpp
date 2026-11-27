// The kernel's hash functions (runtime/kernel/sha.cpp) against digests worked
// out with Python's hashlib, for messages whose lengths sit on and around the
// places where the padding changes shape (55, 56, 64 for the 64-byte block;
// 111, 112, 128 for the 128-byte one), fed whole and in uneven pieces.
// Message n is the bytes (7i + 3) mod 256 for i below n. "abc" is checked
// against the standard's own examples besides.
#include "runtime/kernel/sha.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kernel::sha;

namespace
{
    struct Case { size_t size; const char* sha1; const char* sha256; const char* sha384; const char* sha512; };
    const Case kCases[] = {
    { 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
      "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
    { 3, "4201de9f98cb0a9b8cf52398be7802b55d45266a", "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35",
      "de5060203c58b419f136c17666d339a05705575827e3600f0475f6fb3089870aede26c5a0903dea5938bcded4ee9e619",
      "8853167ab14c043919981a6409307ef68b5b726f4b80dcab259daa24b7cabcb8b4bbfd3e2e36ed98465059661f672e6529cdcc673e7eabfa78dd3186745b112a" },
    { 55, "ddf57317ef34bfee3b6df83d359098930eb278bc", "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b",
      "fc2728f452750d08b5dbad1aea53202e7fc19c06894d13e36c433b0444a899277c49f16e0577c03db0f0605578836ef6",
      "14fd424b1fcadee624da946ab03f7e1def7c0d6e00f689594319881a26ff30b875ba4c622ac13100c8cc784c9c2eb23159aecbb4a02e3999062f551193e2b256" },
    { 56, "a0d492bb0fc889d0eca3bc137066ab6f4f74f369", "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27",
      "6c969a865f38d2eaac60b7963a84c97ba27bcb76e589cbe1e2e89ead6aee4ad0337a4268f054be49c604ddd99692036d",
      "480fa85be41ef55a41208ca28ffc8743c91cf7d24758defe6f95bfb16de614fc86b701034896b047dd571de4318853d80e0809df162f1752cb26da6ddb94a0dd" },
    { 63, "c55856749bef509bdfe6bfebfc7bf4e793e82132", "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055",
      "be67a9a1162c1d97f183980dc811f4f4f835b8b850eb32ecbf87397b26369bc6646a8f2234940dcff142b627c8e21ebb",
      "ecd42a703a4e93e163d60d55e3785b1a763838b0351bc2e6f7c94b4bfb24f9aa15da5d744ebcebe11f0fc4315d45ba3a047b6e60e07448357f2795bf34b73502" },
    { 64, "bede92be29c3874e1b54ddc77988d606fc857a8e", "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241",
      "99d26430f561864c2f7d90b68650b960c86c85d6bf7f54ceeee404bdd59663a7f05588b5c750b862a2a24f6a9ad4c050",
      "8f3cc30b3fb5bf963688a46488249248ac2c67f0f85a145233c6c1e3c16dcd1df634c07d1d31da02576f65b9cf64e1c3fdb318b689b8a14e2e9552bcf30fb133" },
    { 65, "b05a80522b053d6dc7e0a517d0e70212c7dad11f", "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e",
      "cada16d798ec25243470f7827eb89a2402d52680b092dffc0f59f5a919e5058aebf7ad79dba85c61b179d23534d7c855",
      "22050d1b2c5bd016c4b04c3e84f513ffbbbc057f83dc9ab196fee6684e5ee19d98b84c6538b3c5e1fc67dbc484e9d0a198254e41d0fa0cf5106991e106632419" },
    { 111, "4a01b1272627e5be00f57bc3c3029c23a452e629", "67d9492e628fd376e0b2efec8ca2b99b123e202cf620deb270728df979b2f73e",
      "341388a9dc2275074e90cf394323761919761c805fd9e370977c9966a0e8c81a52135f02577670b0071638a4a26dbc31",
      "68cffa6d0d76f309c9ce0d35280939f8e25990c43b7b086ccdf709be35b07d4ddba599541ff2b1c19d34ea49aeafb9659adb7ac3c0b078bb30a22d57fc6687ef" },
    { 112, "54c08c2a0ed21de25e3d8606c3929454eb1d3f8b", "96b928cff8528dbb99602c709a65b846cb6467acb8b722f0d758e4dc27bfc508",
      "619cc5d06138526d70659eccf602d197e63e1050e22039a7feb40a30a5b2b08fb03729e291df12f8c576e6f1cd8af22a",
      "d0865c524d1dddf7c23b799c413f5adcd7caefd3f66a9b49750ec81066012c25a8bcf94ddea6dc525691673097ca40e0101e897fc97218cfdb0704084e2bef4b" },
    { 119, "504e27376a6e0f0dba8295b85cb25dc4dfa17d23", "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e",
      "9c284d266fc91c33a7f6ecbb0326646b3ed15cc33fd8bf968267c21991ca1a434dd65c64ee22c7dd6f5423950f605560",
      "236bdd7f38a611b5014b239245c381ae5d20a96f1e5b3178227c00056b7fa8c44ef54880085d82b7e20cd65f2bfda1326696c3f94a6a5bad0cb5ce289aa46167" },
    { 120, "82134b02fb3f702491be9bed581eeab59334acb2", "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5",
      "96faef865acb7c55ae34422444b4380e520de69f373ddb14c7ee2420c3f93cc220fca9952c0a4fe9dbfddf350ac40127",
      "4bd16fefaa35cc2df9fbb8ff379ec04a4070ffd5d4992574af239fa175534df87cbedceeb96be08e15090a78b83a328a2900c5411683ab47bab914591048548f" },
    { 127, "34d5e582029e9b9b85b2febe31da3db7cdabaaea", "a8d23e75d936f303d248888d9b165ee543f4cbafcad3c9dd2a79bd84faa11d07",
      "f8234ab81f5f9641c656680825a9bbb3c2616bcc80b65d5e479a4d96742e1b8742330c9e8256ef5f03caa932487044ca",
      "e0b6a20f1c0c88970a9340152cd5a1c1ecf3d3b8de55102741879438079473540133b812706e5dbec322c8c9523b6fc8c6d16ee626e87ad5fe3d2916afedc369" },
    { 128, "a09133e6730ffe899efb70204cb5646cd5dc24ee", "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6",
      "e8480e9c4dd90f88104a79cbaccec48edbd798a142b4f241d726dc252f1502350e824c7d18dadd59d7d716919fb8f9bf",
      "99b16f17aa0b969a5b8f08f367719d516e330ccd2660b6f0688ec031dbc783de50a1cd185a2568dba75070a2403d17d4741d163578515dfd2ff756ddfe4d47b1" },
    { 129, "808aea332ce367541d37adae7f94e59c5c1a934e", "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4",
      "430173387764874661acba1e914e50f6d0fbac534809e859b1da4396fad3d5f24c25c6a2d02486d8fbda3994713d981a",
      "a1556e29185778aa5991e34b8884c840d589f0fbb4b8ed590e51e9ac4eb03a008125000db2671f8fe7f485b59a77b518670078ecb41a54b4cd02a7f1d2ca4c6d" },
    { 1000, "4231a8a50a10fa9758db8ec71fdef855b751048a", "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371",
      "94c38db521ca733b8904c2d14b6e82d33dcfcc26e1318c579dbee1fc2f472019034e792263b9d46e90e700de2f6e7e91",
      "00e36fccf193e59697a92b5ab24666ce6326d7fa16bf10832d0991ddc591112e9dfa6a636950ed9c4d67344a760654c2ff7785e1d60094d651038735b5dccabd" },
    };

    std::string Hex(const uint8_t* bytes, size_t size)
    {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (size_t i = 0; i < size; i++) { out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15]; }
        return out;
    }

    // The message in pieces of `step` bytes (0: all at once).
    template <typename State, typename InitFn>
    std::string Digest(InitFn init, const std::vector<uint8_t>& message, size_t step, size_t digestSize)
    {
        State s;
        std::memset(&s, 0xCC, sizeof s);        // Init must not depend on what was there
        init(s);
        if (step == 0) Update(s, message.data(), message.size());
        else for (size_t at = 0; at < message.size(); at += step)
            Update(s, message.data() + at, message.size() - at < step ? message.size() - at : step);
        uint8_t out[64];
        Final(s, out);
        return Hex(out, digestSize);
    }
}

int main()
{
    for (const Case& c : kCases)
    {
        std::vector<uint8_t> message(c.size);
        for (size_t i = 0; i < c.size; i++) message[i] = uint8_t(i * 7 + 3);
        for (size_t step : { size_t(0), size_t(1), size_t(7), size_t(64), size_t(129) })
        {
            assert(Digest<Sha1>([](Sha1& s) { Init(s); }, message, step, kSha1Size) == c.sha1);
            assert(Digest<Sha256>([](Sha256& s) { Init(s); }, message, step, kSha256Size) == c.sha256);
            assert(Digest<Sha512>([](Sha512& s) { Init384(s); }, message, step, kSha384Size) == c.sha384);
            assert(Digest<Sha512>([](Sha512& s) { Init512(s); }, message, step, kSha512Size) == c.sha512);
        }
    }
    // FIPS 180's examples for "abc".
    const std::vector<uint8_t> abc = { 'a', 'b', 'c' };
    assert(Digest<Sha1>([](Sha1& s) { Init(s); }, abc, 0, kSha1Size) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    assert(Digest<Sha256>([](Sha256& s) { Init(s); }, abc, 0, kSha256Size)
           == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::puts("sha_test: SHA-1, SHA-256, SHA-384 and SHA-512 as defined");
    return 0;
}

// Whether the driver's pipeline cache is kept between runs and whether a blob
// read back may reach the driver (runtime/gpu/vulkan/pipeline_cache_rules.h),
// with no device: no Mali phone is here, so the rule is held to the driver
// versions the reporting phones show (a Galaxy A35: Mali-G68, r38p1, 38.1.0).
//
//   * Every GPU but a Mali keeps it, whatever MW2_PERSIST_PIPELINE_CACHE says.
//   * A Mali keeps it from r38 on and not before; 1 keeps it under any
//     release, 0 under none, and anything else set counts as 1 (env::Flag).
//   * A blob whose driver header (VkPipelineCacheHeaderVersionOne) is short,
//     gives a size it cannot have, is not version one, or names another
//     vendor, device or pipelineCacheUUID is set aside before the driver sees
//     it; one that names this device is not.
#include "gpu/vulkan/pipeline_cache_rules.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace rules = vk::pipeline::cache_rules;

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    // VK_MAKE_VERSION, as a Mali's driverVersion is made.
    constexpr uint32_t Version(uint32_t release, uint32_t patch) { return (release << 22) | (patch << 12); }

    constexpr uint32_t kVendor = 0x13B5, kDevice = 0x92041010;   // the Galaxy A35's Mali-G68
    const uint8_t kUuid[16] = { 0x16, 0x4F, 0xE9, 0xDB, 0x6E, 0xFE, 0x7B, 0x8B,
                                0x7B, 0x93, 0x95, 0x04, 0x5D, 0x54, 0xEE, 0x21 };

    void Put(std::vector<uint8_t>& blob, size_t at, uint32_t value) { std::memcpy(blob.data() + at, &value, 4); }

    // A blob as the driver writes it: its header, then `payload` bytes.
    std::vector<uint8_t> Blob(size_t payload = 64)
    {
        std::vector<uint8_t> blob(rules::kDriverHeaderBytes + payload, 0xAB);
        Put(blob, 0, uint32_t(rules::kDriverHeaderBytes));
        Put(blob, 4, rules::kHeaderVersionOne);
        Put(blob, 8, kVendor);
        Put(blob, 12, kDevice);
        std::memcpy(blob.data() + 16, kUuid, sizeof kUuid);
        return blob;
    }

    bool Accepted(const std::vector<uint8_t>& blob)
    {
        return rules::DriverHeaderProblem(blob.data(), blob.size(), kVendor, kDevice, kUuid) == nullptr;
    }

    void KeepRule()
    {
        // Not a Mali: always, and the switch is not read.
        for (const char* asked : { (const char*)nullptr, "0", "1" })
            Check(rules::KeepAcrossRuns(false, Version(1, 0), asked).keep,
                  std::string("a GPU that is not a Mali keeps its cache with the switch ") + (asked ? asked : "unset"));

        // A Mali, unasked, by its release.
        Check(rules::MaliRelease(Version(38, 1)) == 38, "38.1.0 is r38");
        Check(rules::MaliRelease(Version(54, 1)) == 54, "54.1.0 is r54");
        const struct { uint32_t release, patch; bool kept; } releases[] = {
            { 26, 0, false }, { 26, 2, false }, { 32, 1, false }, { 37, 9, false },
            { 38, 0, true }, { 38, 1, true }, { 49, 1, true }, { 54, 1, true }, { 56, 0, true },
        };
        for (const auto& r : releases)
        {
            const rules::Keep keep = rules::KeepAcrossRuns(true, Version(r.release, r.patch), nullptr);
            Check(keep.keep == r.kept, "a Mali under r" + std::to_string(r.release) + "p" + std::to_string(r.patch) +
                                           (r.kept ? " keeps" : " does not keep") + " its cache unasked");
            Check(keep.why && keep.why[0], "the decision says why");
        }

        // The switch, either way.
        Check(rules::KeepAcrossRuns(true, Version(32, 1), "1").keep, "MW2_PERSIST_PIPELINE_CACHE=1 keeps it under r32");
        Check(rules::KeepAcrossRuns(true, Version(26, 0), "yes").keep, "any value but 0 counts as 1, as env::Flag reads it");
        Check(!rules::KeepAcrossRuns(true, Version(38, 1), "0").keep, "MW2_PERSIST_PIPELINE_CACHE=0 refuses it under r38");
        Check(!rules::KeepAcrossRuns(true, Version(54, 1), "0").keep, "MW2_PERSIST_PIPELINE_CACHE=0 refuses it under r54");
        Check(rules::KeepAcrossRuns(true, Version(38, 1), "00").keep, "only a lone 0 refuses it");
    }

    void DriverHeader()
    {
        Check(Accepted(Blob()), "this device's blob reaches the driver");
        Check(Accepted(Blob(0)), "a blob of its header alone reaches the driver");

        Check(rules::DriverHeaderProblem(nullptr, 0, kVendor, kDevice, kUuid) != nullptr, "no blob is set aside");
        std::vector<uint8_t> blob = Blob();
        Check(rules::DriverHeaderProblem(blob.data(), rules::kDriverHeaderBytes - 1, kVendor, kDevice, kUuid) != nullptr,
              "a blob shorter than the driver's header is set aside");

        blob = Blob();
        Put(blob, 0, 16);
        Check(!Accepted(blob), "a header size under 32 is set aside");
        blob = Blob(8);
        Put(blob, 0, uint32_t(blob.size() + 1));
        Check(!Accepted(blob), "a header size past the blob's end is set aside");
        blob = Blob(8);
        Put(blob, 0, uint32_t(blob.size()));
        Check(Accepted(blob), "a longer header that fits is taken");
        blob = Blob();
        Put(blob, 0, 0);
        Check(!Accepted(blob), "a zero-filled header is set aside");
        Check(!Accepted(std::vector<uint8_t>(4096, 0)), "a zero-filled file is set aside");

        blob = Blob();
        Put(blob, 4, 2);
        Check(!Accepted(blob), "a header that is not version one is set aside");
        blob = Blob();
        Put(blob, 8, 0x5143);
        Check(!Accepted(blob), "another vendor's blob is set aside");
        blob = Blob();
        Put(blob, 12, 0x92020010);
        Check(!Accepted(blob), "another device's blob is set aside");
        for (size_t at = 16; at < 32; at++)
        {
            blob = Blob();
            blob[at] ^= 1;
            Check(!Accepted(blob), "a blob whose UUID differs at byte " + std::to_string(at) + " is set aside");
        }
    }
}

int main()
{
    KeepRule();
    DriverHeader();
    std::printf("pipeline cache: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

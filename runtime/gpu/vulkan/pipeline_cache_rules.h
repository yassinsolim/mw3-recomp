#pragma once
// Whether the driver's pipeline cache is kept between runs, and whether a
// blob read back from disk may be handed to the driver at all. The rules
// alone, with no device and no driver in them (tests/pipeline_cache_test.cpp);
// pipeline.cpp applies them.
//
// A Mali's cache was not kept from October 2026 on, on a word: "known driver
// deadlocks" (6089570, 371b5ef, 635e298). What 6089570 fixed was a hang at
// 97% of the boot compile on one Mali-G68 MC4 (a Dimensity 920 kind of chip),
// and the one change took away both the cache and the lock every pipeline
// creation then waited on, with no log saying which of the two hung and no
// driver version recorded. The cache within a run came back the same day
// (635e298) and has worked on Malis since -- a Galaxy A35's log has pipelines
// found "out of the pipeline cache" -- while reading it back from disk stayed
// off without a log of it ever failing. None has turned up since: no report,
// in the repository or in an issue, shows a reloaded cache crashing a Mali or
// being refused by one. Arm's own guidance is to save the cache and load it
// on the next run. A Mali's pipelineCacheUUID changes with every driver
// release (vulkan.gpuinfo.org: Mali-G57 MC2 under r26, r32, r38, r48, r49,
// r54 and r56 has seven), so the driver itself can tell its own blobs.
//
// So a Mali's cache is kept from r38 on, the release the phones reporting
// today run (a Galaxy A35's Mali-G68, r38p1), and not under older ones: the
// Mali-G68 MC4s on vulkan.gpuinfo.org run r26, r32 or r49, which of them hung
// is not known, and the releases before r38 are the ones nothing has been
// tried on since. MW2_PERSIST_PIPELINE_CACHE says otherwise either way: 1
// keeps it under any release, 0 under none.
//
// What is read back is checked before the driver sees it, since a driver need
// not survive a blob it did not write (Roblox's account of shipping Vulkan on
// Android: some read it with memcpy and pointer casts): the runtime's own
// header (pipeline.cpp, ReadCacheFile) names the device, the driver's version
// and its pipelineCacheUUID, and the driver's header at the start of the
// blob, VkPipelineCacheHeaderVersionOne, has to agree with them as well.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vk::pipeline::cache_rules
{
    constexpr uint32_t kArm = 0x13B5;
    // The first Mali driver release whose cache is kept unasked.
    constexpr uint32_t kFirstKeptMaliRelease = 38;

    // A Mali's driverVersion is VK_MAKE_VERSION(release, patch, 0): r38p1 is
    // 38.1.0.
    constexpr uint32_t MaliRelease(uint32_t driverVersion) { return driverVersion >> 22; }

    struct Keep
    {
        bool keep = true;
        // For the log: why, in a few words.
        const char* why = "";
    };

    // `mali`: an Arm GPU under the runtime that reads the switch (Android).
    // `asked`: MW2_PERSIST_PIPELINE_CACHE as set, null where it is not.
    inline Keep KeepAcrossRuns(bool mali, uint32_t driverVersion, const char* asked)
    {
        if (!mali) return { true, "not a Mali" };
        if (asked && asked[0] == '0' && asked[1] == 0) return { false, "MW2_PERSIST_PIPELINE_CACHE=0" };
        if (asked) return { true, "MW2_PERSIST_PIPELINE_CACHE=1" };
        if (MaliRelease(driverVersion) >= kFirstKeptMaliRelease) return { true, "a Mali driver from r38 on" };
        return { false, "a Mali driver older than r38" };
    }

    // VkPipelineCacheHeaderVersionOne, as the specification lays it out:
    // headerSize, headerVersion, vendorID, deviceID, pipelineCacheUUID.
    constexpr size_t kDriverHeaderBytes = 32;
    constexpr uint32_t kHeaderVersionOne = 1;   // VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    constexpr size_t kUuidBytes = 16;           // VK_UUID_SIZE

    // Null where the blob starts with this device's header; otherwise what
    // is wrong with it, for the log.
    inline const char* DriverHeaderProblem(const uint8_t* data, size_t size, uint32_t vendorID, uint32_t deviceID,
                                           const uint8_t* uuid)
    {
        if (!data || size < kDriverHeaderBytes) return "shorter than the driver's header";
        uint32_t words[4];
        std::memcpy(words, data, sizeof words);
        const uint32_t headerSize = words[0], headerVersion = words[1];
        if (headerSize < kDriverHeaderBytes || headerSize > size) return "the driver's header gives a size it cannot have";
        if (headerVersion != kHeaderVersionOne) return "the driver's header is not version one";
        if (words[2] != vendorID || words[3] != deviceID) return "the driver's header names another device";
        if (std::memcmp(data + 16, uuid, kUuidBytes) != 0) return "the driver's header names another pipelineCacheUUID";
        return nullptr;
    }
}

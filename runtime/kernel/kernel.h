#pragma once
#if __has_include(<ppc_recomp_shared.h>)
#include <ppc_recomp_shared.h>
#else
#include "../../ppc/ppc_recomp_shared.h"
#endif
#include "physical.h"
#include "durable.h"
#include "../guest.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace kernel
{
    // Guest address of the executable's XEX header as the loader would have
    // placed it (the one XexExecutableModuleHandle's module entry points to), or
    // zero for a run with no XEX metadata, such as a flat-PE development run.
    uint32_t ExecutableXexHeader();

    // The threads blocked in a kernel wait, and on what: for the watchdog.
    void ReportWaits();

    void Initialise(const std::filesystem::path& gameRoot,
                    const std::vector<uint8_t>& xexHeader = {});
    const std::filesystem::path& GameRoot();

    uint32_t AllocateGuest(uint32_t size, uint32_t alignment = 16);
    uint32_t AllocatePhysical(uint32_t size, uint32_t alignment = 4096);
    // False when `address` is not the start of a live allocation.
    bool     FreeGuest(uint32_t address);
    bool     FreePhysical(uint32_t address);
    void     ReportMemory();

    // Returns the value r13 must hold. The recompiled code reads the thread id
    // straight out of these structures (KPCR.CurrentThread -> KTHREAD.ThreadId),
    // so they have to be populated or every thread reports id 0.
    uint32_t CreateThreadBlock(uint32_t threadId);
    // A fresh id from the same sequence guest threads draw on, for a host
    // thread that runs guest code (the audio worker).
    uint32_t NewThreadId();

    // The kernel publishes a small block of clock values the title reads directly
    // rather than through a call. It arrives as a data import, which nothing
    // resolves, so it has to be created and kept ticking here. 100ns ticks since
    // 1601-01-01, the epoch the console reports.
    uint64_t SystemTime100ns();

    void InstallTimeStampBundle();
    void InstallDebugMonitorData();
    void UpdateTimeStampBundle();

    // The recompiled function at a guest address, or null when the address is
    // outside the image's code or has no function starting there.
    PPCFunc* GuestFunction(uint32_t address);

    // The nth argument (from 0) of a guest call, for the ones past the eight that
    // arrive in r3-r10: the caller's parameter save area starts at r1+0x14 with
    // eight-byte slots, so the ninth is at r1+0x54.
    inline uint32_t StackArgument(const PPCContext& ctx, uint32_t n)
    {
        return *reinterpret_cast<const be32*>(guest::Base() + ctx.r1.u32 + 0x14 + n * 8);
    }

    // Borrows the caller's stack below its frame.
    void CallGuest(const PPCContext& caller, uint32_t address,
                   uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0);

    // User APCs. The title issues asynchronous reads and does its accounting in a
    // completion routine, which NT delivers only when the issuing thread enters an
    // alertable wait -- never inside the call that queued it.
    //
    // `work` runs just before the routine, at delivery time. Asynchronous reads
    // put the transfer itself there: the title double-buffers on the assumption
    // that a read it has issued is still in flight while it decompresses the other
    // buffer, so filling the buffer at issue time corrupts what it is reading.
    void QueueUserApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock,
                      std::function<void()> work = {});
    bool DeliverUserApcs(const PPCContext& caller);

    // XOVERLAPPED. Asynchronous XAM calls take one and the caller polls it, so a
    // call that completes inline still has to mark it finished and signal the
    // event the caller may be waiting on.
    void CompleteOverlapped(uint32_t address, uint32_t result);
    // The same, for a call that failed: the caller reads `result` from the
    // block, and why from its extended error.
    void CompleteOverlappedEx(uint32_t address, uint32_t result, uint32_t extendedError,
                              uint32_t length);

    // XNotify: queues a notification for every listener that asked for its area.
    void PostNotification(uint32_t id, uint32_t param);
    // An accepted Xbox LIVE invitation: XInviteGetAcceptedInfo reports it from
    // here on, and the title is told it arrived.
    void AcceptInvite(uint64_t inviterAccount, const uint8_t* session60, bool fromInvite);

    // The level the title last asked for ("mp_dome"), by the name it asks
    // for the level's loading screen by; empty before the first. For a
    // performance report (docs/perf-reports.md), which names the map and no file.
    std::string LevelAsked();

    // Content packages -- saved games. The title mounts one as a lettered drive
    // ("save0:") and then opens "save0:\savegame.svg" through the ordinary file
    // API, so XAM and the file layer have to share one table of device roots.
    // A package here is a directory under SaveRoot(); nothing writes the console's
    // STFS container format.
    void MountDevice(const std::string& name, const std::filesystem::path& root);
    // A read-only volume as a drive: a downloadable content package, read
    // where it lies (volume.h).
    // `alsoAtRoot` names a folder of it whose files are found at the drive's
    // root as well: the title asks for a fastfile of a language that is not
    // the one it runs in at the root (content.cpp, the title update).
    // `english` are other volumes whose english\ files answer where this one
    // has none: an update of another pressing has no English, and the
    // player's packs have some of what it would have brought.
    void MountVolume(const std::string& name, std::shared_ptr<class ReadVolume> volume,
                     const std::string& alsoAtRoot = {}, std::vector<std::shared_ptr<class ReadVolume>> english = {});
    // A package of downloadable content as a drive, for a title whose table
    // holds fewer packages than the player has (content.cpp): the files of
    // `joined`, other packages of the player's, are found on the drive as
    // well, and the root file `marker`, which says to the title which pack
    // this is, is answered with the volume's own `markerIs`.
    void MountContent(const std::string& name, std::shared_ptr<class ReadVolume> volume,
                      std::vector<std::shared_ptr<class ReadVolume>> joined, const std::string& marker = {},
                      const std::string& markerIs = {});
    void UnmountDevice(const std::string& name);
    const std::filesystem::path& SaveRoot();
    // Downloadable content: the player's own packages for this title, found in
    // the `dlc` folders (content.cpp, docs/dlc.md). Writes XCONTENT_DATA for
    // those from `next` on that fit in the buffer, moves `next` past them and
    // returns how many.
    uint32_t ListDownloadedContent(uint32_t buffer, uint32_t bufferSize, size_t& next);
    // A map or mission file of the player's packages, the package's name as
    // the console shows it, and which of its packs the title takes the
    // file's drive for (0 where that is not known).
    struct ContentFile { std::string name, package; uint32_t pack = 0; };
    // The files at the root of every package the title was given, for what
    // lists the maps in them (mw3_dlc_maps.cpp).
    std::vector<ContentFile> DownloadedContentFiles();
    // The packages themselves, for a title that reads none of their files
    // through a drive of their own (Call of Duty 4: files.cpp).
    std::vector<std::shared_ptr<class ReadVolume>> DownloadedContentVolumes();
    // The title update's package among them, as the drive `update:`, for a
    // build of the executable the update makes (content.cpp).
    void MountTitleUpdate();
    // One of its files, whole, for the game folder `gameRoot` (before
    // Initialise): false when there is no update's package, or no such file.
    bool ReadTitleUpdateFile(const std::filesystem::path& gameRoot, const std::string& name, std::vector<uint8_t>& bytes);
    // The update in use did not make this build's executable: the next one
    // found is in use instead. False when there is no other.
    bool NextTitleUpdate(const std::filesystem::path& gameRoot);
    // The three letters its localised fastfiles begin with ("sna") when the
    // update is of a pressing in another language than the game runs in and
    // so has none of English's; empty otherwise. Whether it has a fastfile.
    const std::string& UpdateLanguage();
    bool UpdateHasZone(const std::string& zone);
    // What is kept under a player's number -- his rank, offline and Live, and
    // the settings of a profile at the second to fourth controllers -- renamed
    // to another number, where that one has none yet (kernel/xam.cpp).
    void MovePlayerData(uint64_t from, uint64_t to);
    // The XUID Live knows the player at the first controller by.
    uint64_t LocalLiveXuid();

    // The one storage device this runtime offers. The title takes the id from the
    // device selector, hands it back in every content call and stores it in the
    // package it creates, so the selector and the content layer have to agree.
    constexpr uint32_t kSaveDeviceId = 1;

    // The console's own network address, as XNetGetTitleXnAddr reports it: 36
    // bytes of XNADDR. XSessionCreate has to put the same one in the session it
    // hands out, so the two answers come from one place.
    void TitleXnAddr(void* out36);
    // The peer an address names; false for one that did not come from here.
    bool XnAddrPeer(const void* addr36, uint64_t& peer);
    // The 8-byte id of the session this machine hosts or joined, which
    // XNetInAddrToXnAddr names for a player the title did not look up itself.
    void NoteSessionId(const void* id8);

    // A search's answer, for XNetQosLookup: the round trip to the player
    // hosting a session, by its id, until the next search.
    void ForgetSessionLags();
    void NoteSessionLag(const void* id8, uint32_t milliseconds);
    // Puts a service's id for a peer into a 36-byte XNADDR.
    void SetXnAddrPeer(void* addr36, uint64_t peer);

    // Once a frame: hands the title the searches that finished.
    void FinishSearches();

    // Called by generated stubs; logs the first hit and counts the rest.
    void Unimplemented(const char* name);

    // End-of-run reports.
    void ReportUnimplemented();
    void ReportMissingFiles();
}

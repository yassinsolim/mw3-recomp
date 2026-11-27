// Small XAM / kernel imports the title reaches that need an honest answer
// rather than a logging stub.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../log.h"

#include <algorithm>
#include <cstdlib>

using namespace kernel;

// XamGetOverlappedResult(overlapped, DWORD* transferred, BOOL wait). Every
// XAM call here completes inline, so the block is already final: report its
// result, and the length through the out pointer. 997 (pending) stays pending.
PPC_FUNC(__imp__XamGetOverlappedResult)
{
    const uint32_t overlapped = ctx.r3.u32;
    if (!overlapped) { ctx.r3.u64 = 87; return; }   // ERROR_INVALID_PARAMETER
    const uint32_t result = *GuestPtr<be32>(overlapped);
    if (auto* out = GuestPtr<be32>(ctx.r4.u32)) *out = uint32_t(*GuestPtr<be32>(overlapped + 4));
    ctx.r3.u64 = result;
}

// XamContentGetDeviceName(deviceId, wchar_t* name, chars).
PPC_FUNC(__imp__XamContentGetDeviceName)
{
    if (ctx.r3.u32 != kSaveDeviceId) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    const char* friendly = "Hard Drive";
    auto* name = GuestPtr<be16>(ctx.r4.u32);
    const uint32_t room = ctx.r5.u32;
    if (!name || room < 11) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    for (int i = 0; ; i++) { name[i] = uint16_t(friendly[i]); if (!friendly[i]) break; }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamShowMessageBoxUI: no dashboard to show it. Answer with the first button,
// which the title lays out as the default, and say so in the log.
PPC_FUNC(__imp__XamShowMessageBoxUI)
{
    const uint32_t result = ctx.r10.u32;
    const uint32_t overlapped = kernel::StackArgument(ctx, 8);
    LOGW("xam: message box (%u buttons) -- answering with the first", ctx.r6.u32);
    if (auto* r = GuestPtr<be32>(result)) *r = 0;
    CompleteOverlappedEx(overlapped, X_ERROR_SUCCESS, X_ERROR_SUCCESS, 0);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
}

PPC_FUNC(__imp__KeBugCheck)
{
    LOGE("KeBugCheck(0x%X) -- the title stopped itself", ctx.r3.u32);
    kernel::ReportUnimplemented();
    std::exit(4);
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    LOGE("KeBugCheckEx(0x%X, %08X, %08X, %08X, %08X) -- the title stopped itself",
         ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    kernel::ReportUnimplemented();
    std::exit(4);
}

// XamTaskSchedule(callback, context, params, HANDLE* task): XAM's own worker
// pool. The callback is DWORD(*)(void*). Each task gets a host thread with its
// own guest stack and thread block, like the DPC worker; the handle is a plain
// object so XamTaskCloseHandle has something to close. XamTaskShouldExit is
// always "no": nothing here cancels a task.
#include "../crash.h"
#include <thread>

namespace
{
    struct XamTask : Object { const char* TypeName() const override { return "xam-task"; } };
    constexpr uint32_t kTaskStack = 256u * 1024u;

    void RunTask(uint32_t routine, uint32_t context)
    {
        crash::RegisterThread("xam task");
        const uint32_t stack = kernel::AllocateGuest(kTaskStack, 0x1000);
        const uint32_t pcr = kernel::CreateThreadBlock(kernel::NewThreadId());
        PPCFunc* fn = kernel::GuestFunction(routine);
        if (!stack || !pcr || !fn)
        {
            LOGW("xam: task %08X could not start", routine);
            return;
        }
        PPCContext ctx{};
        ctx.r1.u32 = (stack + kTaskStack - 0x100) & ~0xFu;
        ctx.r13.u32 = pcr;
        ctx.r3.u32 = context;
        ctx.fpscr.loadFromHost();
        fn(ctx, guest::Base());
        LOGI("xam: task %08X returned %u", routine, ctx.r3.u32);
    }
}

PPC_FUNC(__imp__XamTaskSchedule)
{
    const uint32_t routine = ctx.r3.u32, context = ctx.r4.u32, out = ctx.r6.u32;
    if (!routine) { ctx.r3.u64 = 0x80070057u; return; }   // E_INVALIDARG
    if (auto* h = GuestPtr<be32>(out)) *h = InsertHandle(std::make_shared<XamTask>());
    LOGI("xam: scheduling task %08X(%08X)", routine, context);
    std::thread(RunTask, routine, context).detach();
    ctx.r3.u64 = 0;   // S_OK
}

PPC_FUNC(__imp__XamTaskShouldExit)  { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamTaskCloseHandle) { CloseHandle(ctx.r3.u32); ctx.r3.u64 = 0; }

// XamShowFriendRequestUI: the friend-request dialog is a dashboard screen and
// there is no dashboard. Fail the call outright, with no overlapped left
// pending for the title to wait on.
PPC_FUNC(__imp__XamShowFriendRequestUI)
{
    LOGW("xam: friend request UI -- no dashboard, failing the call");
    ctx.r3.u64 = 1627;   // ERROR_FUNCTION_FAILED
}

// XamVoiceIsActiveProcess: whether this title, and not the system's party
// chat, has the headsets. Voice library 2 asks every frame; "no" has it leave
// the headsets alone, which is right where the runtime's own voice path
// (voice.cpp) is not this title's.
PPC_FUNC(__imp__XamVoiceIsActiveProcess) { ctx.r3.u64 = 0; }

// XamGetLocaleEx(highest country, highest locale): the console's locale, one
// of XC_LOCALE_* (kernel::ConsoleLocale), or the United States (36) where
// the title knows no locale that high, as the console falls back.
PPC_FUNC(__imp__XamGetLocaleEx)
{
    constexpr uint32_t kUnitedStates = 36;
    const uint32_t locale = ConsoleLocale();
    ctx.r3.u64 = locale <= ctx.r4.u32 ? locale : std::min(kUnitedStates, ctx.r4.u32);
}

// XamBackgroundDownloadSetMode: how eagerly the dashboard downloads behind the
// title. There is no dashboard and nothing to download.
PPC_FUNC(__imp__XamBackgroundDownloadSetMode) { ctx.r3.u64 = 0; }

// The system lowering the game's sound under voice chat ("ducking"), which
// XAudio2 sets up as it starts: nothing chats here, so it is off and its
// figures are zero.
PPC_FUNC(__imp__XAudioEnableDucker)           { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerLevel)         { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerThreshold)     { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerAttackTime)    { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerHoldTime)      { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerReleaseTime)   { ctx.r3.u64 = 0; }

// What Advanced Warfare's multiplayer asks before it logs on to its
// publisher's servers (online/demonware.h, kernel/xhttp.cpp).

// XamQueryLiveHiveA(name, buffer, size, overlapped): a setting of the Live
// environment the console is in. The title asks for "activeservices" and
// looks in it for the names of Live's two test environments; an answer naming
// neither is the retail one, so the answer is empty.
PPC_FUNC(__imp__XamQueryLiveHiveA)
{
    if (auto* value = GuestPtr<char>(ctx.r4.u32); value && ctx.r5.u32) value[0] = 0;
    if (ctx.r6.u32) CompleteOverlapped(ctx.r6.u32, X_ERROR_SUCCESS);
    ctx.r3.u64 = 0;
}

// XAuthStartup: the library that fetches Live's tokens for web services.
PPC_FUNC(__imp__XampXAuthStartup) { ctx.r3.u64 = 0; }

// XamGetToken(user, url, its length, token out, overlapped): Live's token for
// a web service, which the title would send with its HTTPS request. No
// request leaves this process, so the answer is Live's own for an address
// that wants no token: the title reads the extended error 0x80070428 as
// "XSTS token not required" (821BA674) and sends its request without one.
PPC_FUNC(__imp__XamGetToken)
{
    constexpr uint32_t kFunctionFailed = 1627, kNoTokenNeeded = 0x80070428, kPending = 997;
    if (!ctx.r7.u32) { ctx.r3.u64 = kFunctionFailed; return; }
    CompleteOverlappedEx(ctx.r7.u32, kFunctionFailed, kNoTokenNeeded, 0);
    ctx.r3.u64 = kPending;
}

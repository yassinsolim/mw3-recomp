// Vd* -- the kernel's video/GPU surface. These are the only calls between the
// title's statically linked D3D9 and the hardware, so they are where the
// command processor (gpu/) attaches.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "../guest.h"
#include "../log.h"
#include "../render_size.h"
#include "../gpu/gpu.h"

#include <cstring>

namespace
{
    struct X_VIDEO_MODE
    {
        be32 displayWidth;
        be32 displayHeight;
        be32 isInterlaced;
        be32 isWidescreen;
        be32 isHiDef;
        be32 refreshRate;      // float bits
        be32 videoStandard;
        be32 unknown0x8A;
        be32 unknown0x01;
        be32 reserved[3];
    };

    constexpr uint32_t kDisplayWidth  = 1280;
    constexpr uint32_t kDisplayHeight = 720;

    uint32_t FloatBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

    void FillVideoMode(X_VIDEO_MODE* m)
    {
        if (!m) return;
        m->displayWidth   = kDisplayWidth;
        m->displayHeight  = kDisplayHeight;
        m->isInterlaced   = 0;
        m->isWidescreen   = render::Widescreen(render::Frame());
        m->isHiDef        = 1;
        m->refreshRate    = FloatBits(60.0f);
        m->videoStandard  = 1;          // NTSC
        m->unknown0x8A    = 0x8A;
        m->unknown0x01    = 0x01;
        m->reserved[0] = m->reserved[1] = m->reserved[2] = 0;
    }
}

// VdInitializeRingBuffer(physicalBase, log2(sizeBytes) - 3)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    gpu::SetRingBuffer(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    gpu::SetReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdInitializeEngines)
{
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__VdShutdownEngines) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__VdQueryVideoMode)
{
    FillVideoMode(GuestPtr<X_VIDEO_MODE>(ctx.r3.u32));
    ctx.r3.u64 = 0;
}

// XGetVideoMode is XAM's view of the same structure.
PPC_FUNC(__imp__XGetVideoMode)
{
    FillVideoMode(GuestPtr<X_VIDEO_MODE>(ctx.r3.u32));
    ctx.r3.u64 = 0;
}

// XGetVideoCapabilities(): no argument and a word of flags for an answer.
// Black Ops III asks as it fills in its record of the display (sub_8248B338)
// and reads bit 4 alone, which with XGetDisplaySize's answer is what it knows
// of a stereoscopic television. None: no flag, and a size of nothing.
PPC_FUNC(__imp__XGetVideoCapabilities) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XGetDisplaySize)       { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__VdQueryVideoFlags)
{
    // Widescreen | HiDef; the title only tests individual bits.
    ctx.r3.u64 = render::Widescreen(render::Frame()) ? 0x00000003 : 0x00000002;
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    // The first fields overlap XVIDEO_MODE; the rest is front-buffer geometry the
    // title does not read before a mode is set.
    if (auto* p = GuestPtr<be32>(ctx.r3.u32))
    {
        std::memset(p, 0, 0x60);
        FillVideoMode(reinterpret_cast<X_VIDEO_MODE*>(p));
    }
    ctx.r3.u64 = 0;
}

// VdSetDisplayModeOverride(width, height, refreshRate (f1), flags, mode, 0): the
// title asks the console for a display mode and then calls VdSetDisplayMode and
// reads the result back through VdGetCurrentDisplayInformation. The host window
// owns the real mode and that query already reports it, so the request is
// recorded and accepted. MW3 calls it from sub_820A6690 with 1280x720 or
// 1920x1080.
PPC_FUNC(__imp__VdSetDisplayModeOverride)
{
    LOGI("video: display mode override %ux%u @ %.2f Hz (host mode unchanged)",
         ctx.r3.u32, ctx.r4.u32, ctx.f1.f64);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    if (auto* type = GuestPtr<be32>(ctx.r3.u32)) *type = 2;
    if (auto* gamma = GuestPtr<be32>(ctx.r4.u32)) *gamma = FloatBits(2.22222233f);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    ctx.r3.u64 = 0;
}

// VdSwap -- the title asks the kernel to build a present command list.
//
// The first argument is 64 dwords D3D9 reserved in its own command stream for
// the kernel's commands, between the frame's last resolve and the flip
// handshake. Left unwritten they hold whatever the arena held on its previous
// lap: zeros at first, then stale packets once it wraps, which the command
// processor walks straight into -- it swallowed the handshake and the title
// stopped a second into a level. Filled as Xenia fills them: a swap packet
// naming the front buffer, then type-2 no-ops. Xenia also writes the front
// buffer's fetch constant to slot 0 first; not here, where the renderer would
// take it for a texture binding.
PPC_FUNC(__imp__VdSwap)
{
    constexpr uint32_t kReservedDwords = 64;
    constexpr uint32_t kSwapSignature  = 0x53574150;               // 'SWAP'
    constexpr uint32_t kXeSwapPacket   = 0xC0000000u | (3u << 16) | (0x64u << 8);
    constexpr uint32_t kType2Nop       = 0x80000000u;
    if (auto* dwords = GuestPtr<be32>(ctx.r3.u32))
    {
        // The fetch constant D3D9 keeps in the front buffer's texture header:
        // base address in dword 1, size in dword 2.
        const auto* fetch = GuestPtr<be32>(ctx.r4.u32);
        const uint32_t frontBuffer = ctx.r8.u32 ? uint32_t(*GuestPtr<be32>(ctx.r8.u32)) : 0;
        const uint32_t size = fetch ? uint32_t(fetch[2]) : 0;
        uint32_t i = 0;
        dwords[i++] = kXeSwapPacket;
        dwords[i++] = kSwapSignature;
        dwords[i++] = frontBuffer ? kernel::ToPhysical(frontBuffer) : 0;
        dwords[i++] = fetch ? (size & 0x1FFF) + 1 : kDisplayWidth;
        dwords[i++] = fetch ? ((size >> 13) & 0x1FFF) + 1 : kDisplayHeight;
        while (i < kReservedDwords) dwords[i++] = kType2Nop;
    }
    gpu::Swap();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    // A scratch buffer the title may append to; it is never submitted.
    static uint32_t buffer = 0;
    if (!buffer) buffer = kernel::AllocatePhysical(64 * 1024, 0x1000);
    // What it fills is 148 bytes on D3D9's stack (every title's present:
    // `addi r3,r1,208`, and `li r5,148` where D3D9 clears them itself), and
    // D3D9 reads more of them than the buffer. While the dword at +8 is not
    // zero and the front buffer is the title's own, the frame is presented
    // with no gamma ramp, which D3D9 writes to the display's colour table as
    // a straight one, and the title's ramp is written again with the next
    // frame that finds the dword zero. Left as the stack had them, those
    // bytes were not zero for most of Advanced Warfare's frames and zero for
    // a few: the picture was shown through the straight ramp, and through
    // the title's for one frame a few times a second -- a flash of the whole
    // frame, the HUD with it. Modern Warfare 3's were never zero, and its
    // Brightness never reached the screen. So the block is zeroed, as Xenia
    // zeroes it: every frame is the title's own, with the title's ramp.
    constexpr uint32_t kSystemCommandBufferBytes = 148;
    if (auto* block = GuestPtr<uint8_t>(ctx.r3.u32))
    {
        std::memset(block, 0, kSystemCommandBufferBytes);
        *GuestPtr<be32>(ctx.r3.u32) = buffer;
    }
    if (auto* id = GuestPtr<be32>(ctx.r4.u32)) *id = 0;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)           { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)                    { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdPersistDisplay)                             { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdEnableDisableClockGating)                   { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAM)                               { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker)                         { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)              { ctx.r3.u64 = 0; }

// This has to match the masking sequence the title also inlines, or the two
// disagree.
PPC_FUNC(__imp__MmGetPhysicalAddress) { ctx.r3.u64 = kernel::ToPhysical(ctx.r3.u32); }

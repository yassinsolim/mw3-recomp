// MW2_FOV: the field of view, as the angle the title's own cg_fov counts --
// the width of a 4:3 picture, 65 on the console. The 16:9 picture is as tall
// as the 4:3 one and wider, so 65 is 80.7 degrees across it.
//
// The title has two settings for it and both belong to its scripts: cg_fov is
// put back to 65 at every multiplayer spawn and walked through other angles
// by the campaign's climbs and cinematics, and cg_fovScale is written at every
// level load and multiplayer connect (0.75 in split screen, 1 otherwise). A
// value given to either is gone at the next script that writes it. So neither
// is touched: the title is made to see cg_fovScale as what the scripts last
// wrote times MW2_FOV / 65, in the two functions that read it.
//
//   T_CG_ViewFov   the angle a client's view is drawn with: cg_fov, or the
//                  weapon's while aiming, or a turret's or a kill camera's,
//                  times cg_fovScale, held between cg_fovMin and 170.
//   T_CG_CullZoom  the zoom the distance culls take from that view, divided by
//                  cg_fovScale so the scale does not bring them nearer.
//
// Each hook scales the function's result, which is the result it would have
// had with the scale in the setting, except for a view already held at one of
// the two bounds before the scale: that one is scaled from the bound.
//
// Modern Warfare 3's multiplayer and Advanced Warfare turn by the view's
// angle in two places, and both compare it with a hip view that is no longer
// the player's. The pad's turn rate is multiplied by tan(half the view) over
// tan(half of 65 degrees times cg_fovScale), kept by T_AimAssist_FovScale: 1
// from the hip and smaller down a sight, which is how a sight turns slower.
// The input's sensitivity scale is the same ratio against a fixed 65, kept
// by T_CG_SetViewFov. Left alone, a view of 100 would turn 1.87 times faster
// from the hip. Each hook divides its factor by what the hip's became, so
// the hip turns as on the console and a sight slows by its zoom against the
// view the player chose.
//
// Advanced Warfare's campaign cannot have T_CG_ViewFov's answer scaled: its
// T_CG_SetViewFov writes the angle it is handed back into cg_fov and into
// the view a script's change starts from, so a scaled angle was scaled again
// at the next frame and the drop into Seoul was drawn at the title's bound
// of 170 degrees. There the angle stays the title's and T_CG_SetViewFov's
// hook scales the two tangents the view is drawn with.
//
// Advanced Warfare blurs the picture's edges in a slide (and has the same
// sprite for other moves) with a sprite ten units ahead of the eye, sized to
// cover a view of 65 degrees and no wider: at 110 it was a blurred rectangle
// some 530 by 300 in the middle of 1280 by 720, whether the view was the
// runtime's or the title's own cg_fov. T_FX_PlayOnTag's hook makes such a
// sprite as much larger as the view is wider, each time its effect is played.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"
#include "live_options.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    constexpr double kConsole = 65.0;       // cg_fov's default
    constexpr double kWidest = 120.0;
    constexpr double kTitleMost = 170.0;    // T_CG_ViewFov's own upper bound

#ifdef T_CG_CullZoom
    // The view over the console's; 0 until the switch has been read.
    std::atomic<double> g_scale{ 0.0 };

    double Given(double asked)
    {
        const double fov = std::clamp(asked, kConsole, kWidest);
        if (fov != asked) LOGW("fov: MW2_FOV is %g to %g, not %g", kConsole, kWidest, asked);
        if (fov != kConsole) LOGI("fov: the view is %g degrees where the console's is %g", fov, kConsole);
        return fov / kConsole;
    }

    double Scale()
    {
        double scale = g_scale.load(std::memory_order_relaxed);
        if (scale != 0.0) return scale;
        const double given = Given(env::Real("MW2_FOV", kConsole));
        // A view set while this read the switch is the newer of the two.
        return g_scale.compare_exchange_strong(scale, given, std::memory_order_relaxed) ? given : scale;
    }
#endif
}

bool live::SetFieldOfView([[maybe_unused]] double degrees)
{
#ifdef T_CG_CullZoom
    g_scale.store(Given(degrees), std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
}

#ifdef T_CG_ViewFov
GUEST_HOOK(T_CG_ViewFov)
{
    GUEST_ORIG(T_CG_ViewFov)(ctx, base);
    ctx.f1.f64 = double(float(std::min(ctx.f1.f64 * Scale(), kTitleMost)));
}
#endif

#ifdef T_CG_CullZoom    // not located in Modern Warfare 3's campaign
GUEST_HOOK(T_CG_CullZoom)
{
    GUEST_ORIG(T_CG_CullZoom)(ctx, base);
    ctx.f1.f64 = double(float(ctx.f1.f64 / Scale()));
}
#endif

#ifdef T_CG_SetViewFov
namespace
{
    constexpr double kRadians = 3.14159265358979323846 / 180.0;

    // tan(half the hip's view) before the scale over the same after it, for
    // that half in radians.
    float HipRatio(double half)
    {
        return float(std::tan(half) / std::tan(std::min(half * Scale(), 1.5)));
    }

    float Real(uint8_t* base, uint32_t address)
    {
        const uint32_t bits = PPC_LOAD_U32(address);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // A client's cg: Advanced Warfare's campaign has the one.
    uint32_t Cg([[maybe_unused]] uint8_t* base, [[maybe_unused]] uint32_t client)
    {
#ifdef T_DATA_Cg
        return T_DATA_Cg;
#else
        return PPC_LOAD_U32(T_DATA_CgArray) + client * T_CG_STRIDE;
#endif
    }

    void Multiply(uint8_t* base, uint32_t address, float by)
    {
        uint32_t bits = PPC_LOAD_U32(address);
        float value;
        std::memcpy(&value, &bits, 4);
        value *= by;
        std::memcpy(&bits, &value, 4);
        PPC_STORE_U32(address, bits);
    }
}

// (local client, the angle T_CG_ViewFov answered)
GUEST_HOOK(T_CG_SetViewFov)
{
    const uint32_t client = ctx.r3.u32;
    GUEST_ORIG(T_CG_SetViewFov)(ctx, base);
    if (Scale() == 1.0) return;
#ifdef T_CG_VIEW_HALF_HEIGHT
    // Advanced Warfare's campaign: the angle is the title's own here, and the
    // two tangents the view is drawn with are scaled where it has just kept
    // them. The sensitivity scale is then the console's as it stands.
    const uint32_t cg = Cg(base, client);
    const double half = std::atan(double(Real(base, cg + T_CG_VIEW_HALF_HEIGHT)) / 0.75);
    if (!(half > 0.0)) return;
    const float wider = float(std::tan(std::min(half * Scale(), kTitleMost * 0.5 * kRadians)) / std::tan(half));
    Multiply(base, cg + T_CG_VIEW_HALF_HEIGHT, wider);
    Multiply(base, cg + T_CG_VIEW_HALF_WIDTH, wider);
#else
    Multiply(base, Cg(base, client) + T_CG_ZOOM_SENSITIVITY, HipRatio(kConsole * 0.5 * kRadians));
#endif
}

// (the aim assist's state, tan(half the view's height)); Advanced Warfare's
// has the local client before the state.
GUEST_HOOK(T_AimAssist_FovScale)
{
    const uint32_t state = ctx.T_AIM_STATE_REGISTER.u32;
    const double tangent = ctx.f1.f64;
    GUEST_ORIG(T_AimAssist_FovScale)(ctx, base);
    if (Scale() == 1.0) return;
    // The hip's half angle is read back from what the title has just kept,
    // the tangent over tan(that half) * 0.75: it is 65 degrees times
    // cg_fovScale and, in Advanced Warfare, times a scale of the scripts'
    // for each client, which only the title knows where to find.
    const double kept = Real(base, state + T_AIM_TURN_RATE_SCALE);
    if (!(kept > 0.0) || !(tangent > 0.0)) return;
    Multiply(base, state + T_AIM_TURN_RATE_SCALE, HipRatio(std::atan(tangent / (0.75 * kept))));
}

#ifdef T_FX_PlayOnTag
namespace
{
    // A sprite's half size the hook has written: where, the console's value
    // and the one written, by which a value the title has loaded afresh at
    // the same place is told from this hook's own.
    struct Fitted
    {
        uint32_t address;
        float console;
        float written;
    };
    std::mutex g_fittedLock;
    std::vector<Fitted> g_fitted;

    void Store(uint8_t* base, uint32_t address, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        PPC_STORE_U32(address, bits);
    }

    // The console's value times `wider` at a half size's address.
    void Fit(uint8_t* base, uint32_t address, float wider)
    {
        const float now = Real(base, address);
        const auto kept = std::find_if(g_fitted.begin(), g_fitted.end(),
                                       [address](const Fitted& fitted) { return fitted.address == address; });
        const bool ours = kept != g_fitted.end() && kept->written == now;
        const float console = ours ? kept->console : now;
        const float value = console * wider;
        if (value != now) Store(base, address, value);
        if (wider == 1.0f)
        {
            // The console's own again: nothing of this hook's is left there.
            if (kept != g_fitted.end()) g_fitted.erase(kept);
        }
        else if (kept != g_fitted.end()) *kept = { address, console, value };
        else g_fitted.push_back({ address, console, value });
    }

    // The view over the console's as a sprite at a fixed distance sees it:
    // the ratio of the tangents of half of each. cg_fov is what the hip's
    // view is made from in the multiplayer and what the campaign's view was
    // last set to, both before the runtime's scale. Never under 1: a sprite
    // that covers the console's view covers a narrower one.
    float Wider(uint8_t* base)
    {
        const uint32_t setting = PPC_LOAD_U32(T_DATA_CgFovDvar);
        if (setting == 0) return 1.0f;
        const double angle = std::min(double(Real(base, setting + T_DVAR_REAL)) * Scale(), kTitleMost);
        return float(std::max(1.0, std::tan(angle * 0.5 * kRadians) / std::tan(kConsole * 0.5 * kRadians)));
    }

    bool BlursTheEdges(uint8_t* base, uint32_t material)
    {
        static const char kName[] = "radialblur_sprite_";
        const uint32_t name = material ? PPC_LOAD_U32(material) : 0;
        return name != 0 && std::strncmp(reinterpret_cast<const char*>(base + name), kName, sizeof kName - 1) == 0;
    }
}

// (.., r4 = the effect, ..)
GUEST_HOOK(T_FX_PlayOnTag)
{
    const uint32_t effect = ctx.r4.u32;
    const uint32_t elements = effect ? PPC_LOAD_U32(effect + 0x30) : 0;
    if (elements != 0)
    {
        const uint32_t count = PPC_LOAD_U32(effect + 0x10) + PPC_LOAD_U32(effect + 0x14) + PPC_LOAD_U32(effect + 0x18);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t element = elements + i * 0x120;
            // A sprite of one material: the other kinds keep something else
            // where a sprite keeps its material.
            if (PPC_LOAD_U8(element + 0xB4) > 2 || PPC_LOAD_U8(element + 0xB6) != 1) continue;
            if (!BlursTheEdges(base, PPC_LOAD_U32(element + 0xC4))) continue;
            const uint32_t samples = PPC_LOAD_U32(element + 0xC0);
            if (samples == 0) continue;
            const float wider = Wider(base);
            const std::lock_guard<std::mutex> lock(g_fittedLock);
            // One sample more than the intervals; the base and the amplitude
            // of each, so that a size drawn at random keeps its share.
            for (uint32_t state = 0; state < (PPC_LOAD_U8(element + 0xB8) + 1u) * 2; ++state)
            {
                Fit(base, samples + state * 0x38 + 0x24, wider);
                Fit(base, samples + state * 0x38 + 0x28, wider);
            }
        }
    }
    GUEST_ORIG(T_FX_PlayOnTag)(ctx, base);
}
#endif
#endif

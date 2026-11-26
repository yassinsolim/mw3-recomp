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
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"
#include "live_options.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

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
#endif

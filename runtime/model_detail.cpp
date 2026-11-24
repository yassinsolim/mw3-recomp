// MW2_LOD_BIAS=<1 to 8>: how many times nearer than on the console a model
// changes to its coarser meshes. 1, and unset, is the console's.
//
// A model carries up to four meshes, each with the distance it is drawn
// within, and XModelGetLodForDist picks one: the first whose distance the
// model's own (scaled for the field of view by its callers) is under. The
// title registers the PC game's settings for this -- r_highLodDist,
// r_mediumLodDist, r_lowLodDist, r_lowestLodDist and r_forceLod, cheats all
// (R_RegisterDvars, sub_823A2F98 in the disc's campaign) -- but what they
// would set, XModelSetTestLods (sub_82300BC8), is a lone `blr` in the
// console's build: forcing the lowest mesh with them draws the same indices
// as before. The PC's r_lodScaleRigid and r_lodBiasRigid are not in the
// executables at all. So the choice is made here, where every caller passes:
// the first of the function's two distances, the one the meshes are compared
// with, is multiplied. The second, which only decides whether a model beyond
// its last mesh's distance is drawn at all, is left alone, so nothing
// disappears sooner than on the console and what a player can see of another
// is the same on every device in a match.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"
#include "live_options.h"
#include "log.h"

#include <atomic>

#ifdef T_XModelGetLodForDist

namespace
{
    constexpr double kConsole = 1.0, kMost = 8.0;
    // 0 until the switch has been read.
    std::atomic<double> g_bias{ 0.0 };

    double Given(double asked)
    {
        if (!(asked >= kConsole && asked <= kMost))
        {
            LOGW("detail: MW2_LOD_BIAS is from 1 to 8, not %g", asked);
            return kConsole;
        }
        if (asked != kConsole)
            LOGI("detail: a model changes to its coarser meshes %g times nearer than on the console (MW2_LOD_BIAS)", asked);
        return asked;
    }

    double Bias()
    {
        double bias = g_bias.load(std::memory_order_relaxed);
        if (bias != 0.0) return bias;
        const double given = Given(env::Real("MW2_LOD_BIAS", kConsole));
        // A bias set while this read the switch is the newer of the two.
        return g_bias.compare_exchange_strong(bias, given, std::memory_order_relaxed) ? given : bias;
    }
}

// (model, the distance its meshes are compared with, the distance its last
// mesh is culled by) -> the mesh, or -1 for none
GUEST_HOOK(T_XModelGetLodForDist)
{
    ctx.f1.f64 *= Bias();
    GUEST_ORIG(T_XModelGetLodForDist)(ctx, base);
}

bool live::SetDetailBias(double bias)
{
    g_bias.store(Given(bias), std::memory_order_relaxed);
    return true;
}

#else

bool live::SetDetailBias(double) { return false; }

#endif

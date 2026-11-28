// How the presenter's frame sits on the panel and what the log says of it
// (runtime/gpu/vulkan/surface_transform.h): identity wherever the surface
// takes it, and the warning that the picture may be rotated only where it does
// not. A phone's log (HONOR LGN-NX1, case C0015) warned at every swapchain
// remade after the first, on a surface that took identity.
#include "surface_transform.h"

#include <cstdio>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        if (ok) return;
        g_failures++;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

int main()
{
    using namespace vk::surface_transform;

    // A landscape window on a portrait panel that takes identity.
    const Choice turned = Choose(kIdentity | kRotate90 | kRotate180 | kRotate270, kRotate90);
    Check(turned.transform == kIdentity, "identity where the surface takes it");
    Check(turned.compositorTurns, "the compositor turns a frame drawn square on");
    Check(!turned.mayBeRotated, "an identity frame is upright");

    bool told = false;
    Check(Tell(turned, told) == Note::CompositorTurns, "the first swapchain says the compositor turns the frame");
    for (int remade = 0; remade < 8; remade++)
        Check(Tell(turned, told) == Note::None, "a remade swapchain on the same surface says nothing");

    // A surface already upright: nothing to say.
    const Choice upright = Choose(kIdentity | kRotate90, kIdentity);
    Check(upright.transform == kIdentity && !upright.compositorTurns && !upright.mayBeRotated, "an upright surface");
    bool fresh = false;
    Check(Tell(upright, fresh) == Note::None && !fresh, "an upright surface says nothing and leaves the once unspent");

    // A surface that will not take identity: its own transform, and the warning every time.
    const Choice refused = Choose(kRotate90, kRotate90);
    Check(refused.transform == kRotate90, "the surface's own transform where identity is refused");
    Check(!refused.compositorTurns, "presents are not expected to be suboptimal");
    Check(refused.mayBeRotated, "the frame may show on its side");
    Check(Tell(refused, told) == Note::MayBeRotated, "warned after the once was spent");
    bool unspent = false;
    Check(Tell(refused, unspent) == Note::MayBeRotated, "warned before any");

    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("surface transform: all checks passed\n");
    return 0;
}

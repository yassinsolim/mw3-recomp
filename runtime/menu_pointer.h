#pragma once
#include <cstdint>

struct PPCContext;

// The mouse as a pointer in the title's menus (menu_pointer.cpp): Modern
// Warfare 3 on Windows. The menus are the console's, made for a d-pad, and
// know nothing of a pointer; what is under it is given the focus the d-pad
// would have had to walk to, and a click is then the A button.
namespace menu_pointer
{
    // Whether this title's menus can be pointed in. Where they cannot, the
    // window keeps the mouse and its buttons and wheel stand in for the pad.
    bool There();

    enum class Under
    {
        Nothing,    // the menu has things to choose, and none is under the pointer
        Item,       // one is, and it has the focus now
        NoChoice,   // nothing on screen can be chosen: a notice, a "press A"
    };
    // The pointer at (x, y) on the frame, 0 being its left or top edge and 1
    // its right or bottom. On the title's main thread, from inside a kernel
    // import: `ctx` is the guest's, and is as it was on return.
    Under At(PPCContext& ctx, uint8_t* base, float x, float y);
}

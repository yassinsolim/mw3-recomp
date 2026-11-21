// The mouse as a pointer in the title's menus (menu_pointer.h).
//
// The console's menus are walked with the d-pad: one item of the menu on top
// has the focus, up and down move it, A acts on it. The PC game's menus hand
// the pointer's place to every item and focus the one that holds it; the
// console's executable has no such code left -- the context's cursor is put
// in the middle of the screen when a menu opens and never read. So the same
// is done from here with what the executable does have: the open menu's
// items and their rectangles, ScrPlace_ApplyRect to turn a rectangle into
// the frame's pixels as the drawing does (alignment, the safe area, a wide
// screen), the test the d-pad makes of whether an item can be reached, and
// Item_SetFocus, which takes the focus off the item that had it and runs
// both items' scripts. The click itself is the A button (kernel/input.cpp).
//
// A list drawn by one item (the lobby's players, a server browser) has no
// rows here; the wheel and the keys go through it as the d-pad does.

#include <ppc_recomp_shared.h>
#include "title.h"
#include "menu_pointer.h"
#include "diagnostics.h"
#include "log.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(_WIN32) && defined(T_UI_ItemSetFocus)

namespace
{
    constexpr uint32_t kVisible = 4, kFocused = 2;      // a window's flags
    constexpr uint32_t kRect = 4, kFlags = 72;          // in a window
    constexpr uint32_t kMenuWindow = 4, kMenuItemCount = 180, kMenuItems = 184;
    constexpr uint32_t kFullscreen = 4;                 // the alignment that fills the frame
    constexpr uint32_t kStackDepth = 16, kMostItems = 4096;

    // The menu the controller is talking to: the topmost open one that is
    // visible and focused, as the title's own Menu_GetFocused finds it.
    uint32_t FocusedMenu(uint8_t* base)
    {
        const uint32_t open = PPC_LOAD_U32(T_DATA_UiContext + T_UI_OPEN_MENUS);
        if (open > kStackDepth) return 0;
        for (uint32_t i = open; i-- > 0; )
        {
            const uint32_t menu = PPC_LOAD_U32(T_DATA_UiContext + T_UI_MENU_STACK + i * 4);
            if (menu && (PPC_LOAD_U32(menu + kMenuWindow + kFlags) & (kVisible | kFocused)) == (kVisible | kFocused))
                return menu;
        }
        return 0;
    }

    struct Rect { float x, y, width, height; };

    // Guest calls made from inside a kernel import, on stack borrowed below
    // the guest's own (console.cpp does the same for Cbuf_AddText).
    struct Calls
    {
        PPCContext& ctx;
        uint8_t* base;
        const PPCContext saved;
        const uint32_t scratch, stack;

        Calls(PPCContext& ctx, uint8_t* base)
            : ctx(ctx), base(base), saved(ctx),
              scratch((ctx.r1.u32 - 0x100) & ~0xFu), stack((ctx.r1.u32 - 0x200) & ~0xFu) {}
        ~Calls() { ctx = saved; }

        // A window's rectangle, in the frame's pixels.
        Rect Placed(uint32_t placement, const Rect& rect, uint32_t horizontal, uint32_t vertical)
        {
            const float in[4] = { rect.x, rect.y, rect.width, rect.height };
            for (uint32_t i = 0; i < 4; i++) PPC_STORE_U32(scratch + i * 4, std::bit_cast<uint32_t>(in[i]));
            ctx.r1.u64 = stack;
            ctx.r3.u64 = placement;
            ctx.r4.u64 = scratch;
            ctx.r5.u64 = scratch + 4;
            ctx.r6.u64 = scratch + 8;
            ctx.r7.u64 = scratch + 12;
            ctx.r8.u64 = horizontal;
            ctx.r9.u64 = vertical;
            GUEST_FUNC(T_ScrPlace_ApplyRect)(ctx, base);
            float out[4];
            for (uint32_t i = 0; i < 4; i++) out[i] = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(scratch + i * 4)));
            return { out[0], out[1], out[2], out[3] };
        }
        Rect Placed(uint32_t placement, uint32_t window)
        {
            Rect rect;
            rect.x = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(window + kRect)));
            rect.y = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(window + kRect + 4)));
            rect.width = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(window + kRect + 8)));
            rect.height = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(window + kRect + 12)));
            return Placed(placement, rect, PPC_LOAD_U8(window + kRect + 16), PPC_LOAD_U8(window + kRect + 17));
        }
        uint32_t Placement()
        {
            ctx.r1.u64 = stack;
            ctx.r3.u64 = 0;
            GUEST_FUNC(T_ScrPlace_Active)(ctx, base);
            return ctx.r3.u32;
        }
        bool CanFocus(uint32_t item)
        {
            ctx.r1.u64 = stack;
            ctx.r3.u64 = 0;
            ctx.r4.u64 = item;
            GUEST_FUNC(T_UI_ItemCanFocus)(ctx, base);
            return ctx.r3.u32 != 0;
        }
        void Focus(uint32_t item)
        {
            ctx.r1.u64 = stack;
            ctx.r3.u64 = T_DATA_UiContext;
            ctx.r4.u64 = item;
            GUEST_FUNC(T_UI_ItemSetFocus)(ctx, base);
        }
    };
}

bool menu_pointer::There() { return true; }

menu_pointer::Under menu_pointer::At(PPCContext& ctx, uint8_t* base, float x, float y)
{
    const uint32_t menu = FocusedMenu(base);
    if (!menu || ctx.r1.u32 < 0x1000) return Under::NoChoice;
    const uint32_t count = PPC_LOAD_U32(menu + kMenuItemCount);
    const uint32_t items = PPC_LOAD_U32(menu + kMenuItems);
    if (!items || count > kMostItems) return Under::NoChoice;

    Calls calls(ctx, base);
    const uint32_t placement = calls.Placement();
    // The frame as the title has it, which the pointer's place is a part of.
    const Rect frame = calls.Placed(placement, { 0.0f, 0.0f, 640.0f, 480.0f }, kFullscreen, kFullscreen);
    const float px = frame.x + x * frame.width, py = frame.y + y * frame.height;

    // Under MW2_TRACE_POINTER, what a menu offers, the first time it is pointed in.
    static uint32_t listed = 0;
    if (diag::Flag("MW2_TRACE_POINTER") && std::exchange(listed, menu) != menu)
    {
        const uint32_t name = PPC_LOAD_U32(menu + kMenuWindow);
        LOGI("pointer: menu \"%s\", %u items, frame %.0fx%.0f", name ? reinterpret_cast<const char*>(base + name) : "",
             count, frame.width, frame.height);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t item = PPC_LOAD_U32(items + i * 4);
            if (!item || !(PPC_LOAD_U32(item + kFlags) & kVisible) || !calls.CanFocus(item)) continue;
            const Rect at = calls.Placed(placement, item);
            const uint32_t itemName = PPC_LOAD_U32(item);
            LOGI("pointer:   item %u \"%s\" at %.0f,%.0f %.0fx%.0f", i,
                 itemName ? reinterpret_cast<const char*>(base + itemName) : "", at.x, at.y, at.width, at.height);
        }
    }

    // The first item that holds the pointer and can take the focus, in the
    // menu's own order, which is the PC game's rule.
    bool choice = false;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t item = PPC_LOAD_U32(items + i * 4);
        if (!item) continue;
        const uint32_t flags = PPC_LOAD_U32(item + kFlags);
        if (!(flags & kVisible)) continue;
        const Rect at = calls.Placed(placement, item);
        const bool inside = at.width > 0.0f && at.height > 0.0f &&
                            px >= at.x && px < at.x + at.width && py >= at.y && py < at.y + at.height;
        if (choice && !inside) continue;
        if (!calls.CanFocus(item)) continue;
        choice = true;
        if (!inside) continue;
        if (!(flags & kFocused))
        {
            calls.Focus(item);
            if (diag::Flag("MW2_TRACE_POINTER"))
            {
                const uint32_t name = PPC_LOAD_U32(item);
                LOGI("pointer: at %.0f,%.0f of %.0fx%.0f -- item %u \"%s\" (%.0f,%.0f %.0fx%.0f) has the focus",
                     px, py, frame.width, frame.height, i,
                     name ? reinterpret_cast<const char*>(base + name) : "", at.x, at.y, at.width, at.height);
            }
        }
        return Under::Item;
    }
    return choice ? Under::Nothing : Under::NoChoice;
}

bool menu_pointer::OverWorld(uint8_t* base)
{
    const uint32_t menu = FocusedMenu(base);
    const uint32_t name = menu ? PPC_LOAD_U32(menu + kMenuWindow) : 0;
    return name && std::strcmp(reinterpret_cast<const char*>(base + name), "scoreboard") == 0;
}

#else

bool menu_pointer::There() { return false; }
menu_pointer::Under menu_pointer::At(PPCContext&, uint8_t*, float, float) { return Under::NoChoice; }
bool menu_pointer::OverWorld(uint8_t*) { return false; }

#endif

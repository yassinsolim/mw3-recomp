// XamInput* -- gamepad state. Backed by SDL3 when available; otherwise reports
// "no controller connected" unless an explicit diagnostic input script is set.
// On Android the touch controls and any physical pad (android/) answer first.
#if __has_include(<ppc_recomp_shared.h>)
#include <ppc_recomp_shared.h>
#else
#include "../../ppc/ppc_recomp_shared.h"
#endif
#include "kernel.h"
#include "../guest.h"
#include "../log.h"
#include "../diagnostics.h"
#include "../console.h"
#include "../online/service.h"
#include "../player.h"
#include "../friends.h"
#include "../guide.h"
#include "../signin.h"
#include "../stutters.h"
#include "../gpu/vulkan/renderer.h"
#include "../gpu/vulkan/presenter.h"
#include "../menu_pointer.h"
#include "../mouse_look.h"
#include "../title.h"
#include "../env.h"

#ifdef MW2_ANDROID
#include "../android/android.h"
#include <algorithm>
#include <cstring>
#endif

#ifdef MW2_USE_SDL
#include <SDL3/SDL.h>
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    // XINPUT_GAMEPAD, big-endian in guest memory.
    struct XInputGamepad
    {
        be16 buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        be16 thumbLX, thumbLY, thumbRX, thumbRY;
    };
    struct XInputState
    {
        be32 packet;
        XInputGamepad gamepad;
    };
    struct XInputVibration { be16 left, right; };
    struct XInputCapabilities
    {
        uint8_t type, subType;
        be16 flags;
        XInputGamepad gamepad;
        XInputVibration vibration;
    };

#ifdef MW2_USE_SDL
    std::once_flag g_sdlOnce;
    // The title reads the pads on its main thread and sets the motors from
    // whichever thread runs its rumble; both open and close pads here.
    std::mutex g_padLock;
    SDL_Gamepad* g_pads[4] = {};

    void EnsureSdl()
    {
        std::call_once(g_sdlOnce, []{
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                LOGW("SDL gamepad init failed: %s", SDL_GetError());
        });
    }

    // What is plugged in, said whenever it changes: a controller the game does
    // not answer to is either not here at all, here as a joystick SDL has no
    // layout for, or one SDL was told to ignore.
    void SayDevices()
    {
        static std::string said = "?";
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        std::string now;
        for (int i = 0; ids && i < count; i++)
        {
            const char* name = SDL_GetJoystickNameForID(ids[i]);
            char text[160];
            std::snprintf(text, sizeof text, "%s%s (%04X:%04X, %s)", i ? ", " : "", name ? name : "unnamed",
                          SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]),
                          SDL_IsGamepad(ids[i]) ? "a gamepad" : "no gamepad layout known");
            now += text;
        }
        SDL_free(ids);
        if (now == said) return;
        said = now;
        if (now.empty()) LOGI("input: no controller is plugged in, as far as SDL sees");
        else LOGI("input: plugged in: %s", now.c_str());
        for (const char* hint : { SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT })
            if (const char* value = SDL_GetHint(hint); value && *value)
                LOGI("input: SDL is told %s=%s", hint, value);
    }

    SDL_Gamepad* PadFor(uint32_t user)
    {
        if (user >= 4) return nullptr;
        EnsureSdl();
        std::lock_guard lock(g_padLock);
        if (g_pads[user] && !SDL_GamepadConnected(g_pads[user]))
        {
            SDL_CloseGamepad(g_pads[user]);
            g_pads[user] = nullptr;
        }
        // Looked for once a second, not on every poll: the title polls all
        // four users every frame, and each look allocates the device list.
        static std::chrono::steady_clock::time_point lastLook[4];
        const auto now = std::chrono::steady_clock::now();
        if (!g_pads[user] && now - lastLook[user] >= std::chrono::seconds(1))
        {
            lastLook[user] = now;
            if (user == 0) SayDevices();
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            if (ids)
            {
                if (int(user) < count) g_pads[user] = SDL_OpenGamepad(ids[user]);
                SDL_free(ids);
            }
            if (g_pads[user])
            {
                const char* name = SDL_GetGamepadName(g_pads[user]);
                LOGI("input: controller %u is %s", user, name ? name : "unnamed");
            }
        }
        return g_pads[user];
    }

#endif
    constexpr uint16_t BTN_DPAD_UP=0x0001, BTN_DPAD_DOWN=0x0002, BTN_DPAD_LEFT=0x0004,
        BTN_DPAD_RIGHT=0x0008, BTN_START=0x0010, BTN_BACK=0x0020, BTN_LTHUMB=0x0040,
        BTN_RTHUMB=0x0080, BTN_LB=0x0100, BTN_RB=0x0200, BTN_A=0x1000, BTN_B=0x2000,
        BTN_X=0x4000, BTN_Y=0x8000;

    struct Named { const char* name; uint16_t bit; };
    constexpr Named kButtons[] = {
        { "up", BTN_DPAD_UP }, { "down", BTN_DPAD_DOWN }, { "left", BTN_DPAD_LEFT },
        { "right", BTN_DPAD_RIGHT }, { "start", BTN_START }, { "back", BTN_BACK },
        { "lthumb", BTN_LTHUMB }, { "rthumb", BTN_RTHUMB }, { "lb", BTN_LB },
        { "rb", BTN_RB }, { "a", BTN_A }, { "b", BTN_B }, { "x", BTN_X }, { "y", BTN_Y },
        { "guide", signin::kGuide },
    };

    // What MW2_INPUT_SCRIPT does with the mouse this poll (ScriptedButtons):
    // `at<x>/<y>` puts a menu's pointer at that part of the frame, `click`
    // holds the left button and `turn<dx>/<dy>` moves the mouse that many
    // counts, with no window and no hand.
    struct ScriptedMouse
    {
        bool pointing = false;
        float x = 0, y = 0;
        bool click = false;
        bool turning = false;   // a `turn` is under way, or only just over
        float dx = 0, dy = 0;   // its counts that MouseInputs has not taken yet
    } g_scriptMouse;

    // The keyboard, for when there is no controller. Only useful with the window
    // open, since that is what gives SDL a focused surface to read keys from.
#ifdef MW2_USE_SDL
    struct Key { SDL_Scancode code; uint16_t bit; };
    constexpr Key kKeys[] = {
        { SDL_SCANCODE_RETURN, BTN_START },
        { SDL_SCANCODE_TAB, BTN_BACK },       { SDL_SCANCODE_BACKSPACE, BTN_BACK },
        { SDL_SCANCODE_UP, BTN_DPAD_UP },     { SDL_SCANCODE_DOWN, BTN_DPAD_DOWN },
        { SDL_SCANCODE_LEFT, BTN_DPAD_LEFT }, { SDL_SCANCODE_RIGHT, BTN_DPAD_RIGHT },
        { SDL_SCANCODE_Z, BTN_A },            { SDL_SCANCODE_X, BTN_B },
        { SDL_SCANCODE_C, BTN_X },            { SDL_SCANCODE_V, BTN_Y },
        { SDL_SCANCODE_Q, BTN_LB },           { SDL_SCANCODE_E, BTN_RB },
        { SDL_SCANCODE_HOME, signin::kGuide },
        // Standard PC bindings beside them
        { SDL_SCANCODE_SPACE, BTN_A },        { SDL_SCANCODE_LCTRL, BTN_B },
        { SDL_SCANCODE_R, BTN_X },            { SDL_SCANCODE_F, BTN_X },
        { SDL_SCANCODE_1, BTN_Y },            { SDL_SCANCODE_2, BTN_Y },
        { SDL_SCANCODE_LSHIFT, BTN_LTHUMB },
        { SDL_SCANCODE_4, BTN_LB },           { SDL_SCANCODE_G, BTN_RB },
    };

    uint16_t KeyboardButtons()
    {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys) return 0;
        // Alt+Tab is the desktop's: its Tab, which the window still sees go
        // down, is not Back, or leaving the window would open the scoreboard.
        const bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
        uint16_t buttons = 0;
        for (const Key& key : kKeys)
            if (keys[key.code] && !(alt && key.code == SDL_SCANCODE_TAB)) buttons |= key.bit;
        return buttons;
    }

    // Whether the title has a menu up: the first local client's key catchers
    // have the menus' bit while one takes the controller (title.h). Never,
    // for an executable they have not been found in.
    bool MenuUp(uint8_t* base)
    {
#ifdef T_DATA_KeyCatchers
        return (PPC_LOAD_U32(T_DATA_KeyCatchers) & 0x10) != 0;
#else
        (void)base;
        return false;
#endif
    }

    // Escape is the PC's key for the pause menu and for leaving a menu: Start
    // in the world, B with a menu up. Which it is is settled as the key goes
    // down, and the button is let go as soon as the answer changes: the press
    // that opened the pause menu would otherwise be the B that closed it.
    uint16_t EscapeButton(bool menu)
    {
        static bool wasDown = false, inMenu = false;
        const bool* keys = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(nullptr) : nullptr;
        const bool down = keys && keys[SDL_SCANCODE_ESCAPE];
        if (down && !wasDown) inMenu = menu;
        wasDown = down;
        if (!down || inMenu != menu) return 0;
        return menu ? BTN_B : BTN_START;
    }

#ifdef _WIN32
    // WASD and the mouse on user 0's sticks and triggers: Windows only, and
    // only while the game's window has the keyboard (XamInputGetState).
    void KeyboardMoveAxes(int16_t& lx, int16_t& ly)
    {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys) return;
        int x = 0, y = 0;
        if (keys[SDL_SCANCODE_W]) y += 32767;
        if (keys[SDL_SCANCODE_S]) y -= 32767;
        if (keys[SDL_SCANCODE_D]) x += 32767;
        if (keys[SDL_SCANCODE_A]) x -= 32767;
        if (x != 0) lx = int16_t(x);
        if (y != 0) ly = int16_t(y);
    }

    // The wheel, a notch at a time: a tap of Y in the world, which is the
    // console's one weapon switch whichever way the wheel turns, and of the
    // d-pad's up or down in a menu. A tap is long enough for a title drawing
    // 30 frames a second to see it go down and come up.
    uint16_t WheelTaps(int notches, bool menu)
    {
        constexpr auto kHalf = std::chrono::milliseconds(70);
        static int waiting = 0;                     // notches not tapped yet, up positive
        static bool forMenu = false, down = false;
        static std::chrono::steady_clock::time_point turn{};
        if (menu != forMenu) { waiting = 0; forMenu = menu; }
        waiting = std::clamp(waiting + notches, -8, 8);
        const auto now = std::chrono::steady_clock::now();
        if (now >= turn && (down || waiting))
        {
            if (down) waiting -= waiting > 0 ? 1 : -1;
            down = !down;
            turn = now + kHalf;
        }
        if (!down || !waiting) return 0;
        return !menu ? BTN_Y : waiting > 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN;
    }

    // The mouse as user 0's (vk::TakeMouse). In the world the window holds it
    // and it turns the view, and is the triggers (left button the right one,
    // right button the sights), the right thumb click (middle) and Y (the
    // wheel). With a menu up it is what chooses: where the title's menus can
    // be pointed in (menu_pointer.h) the cursor is free, what is under it has
    // the focus and the left button is A on that; elsewhere the window still
    // holds it and the left button is A on what the keys chose. The right
    // button is B and the wheel the d-pad either way. After F2 has given the
    // cursor back, a click is the desktop's and nothing here.
    //
    // A button is the world's or the menu's by where it went down, and is let
    // go when that changes under it: the click that chose Resume is not a
    // jump, nor the shot held as the match ended a choice in the next menu.
    //
    // The view is turned by the angle the counts come to wherever the title
    // has been found to take one (mouse_look.h), which is the disc's
    // executables. The update's take the right stick, and what follows is
    // for them; it is not a mouse, as mouse_look.cpp's measurements say.
    //
    // A stick is a rate and a mouse a distance. The counts that came since the
    // last read are how far the view should turn over the frame this read is
    // for, which lasts about as long as the time since that read; so the stick
    // is set to the rate that turns that far in that time, as a part of what
    // the title turns at a full stick. Taken as a deflection themselves, as
    // they once were, the counts turned the view by the frame time twice over
    // -- twice as fast at half the frame rate -- and a slow hand, under the
    // title's dead zone, did not turn it at all.
    //
    // The numbers are the title's own. Modern Warfare 3's multiplayer takes
    // 0.2 off the stick as dead and the last 0.01 as full
    // (gpad_stick_deadzone_min, _max), and turns a full stick 260 degrees a
    // second across and 90 up and down from the hip, 90 and 55 down the sights
    // (constants in its sub_820D4088); they are Modern Warfare 2's
    // aim_turnrate dvars too. What it does to a stick besides is still done to
    // the mouse: its response curve (aim_assist/view_input_3.graph), the
    // 1200 degrees a second squared a turn speeds up by, its slowing near a
    // target, and its own sensitivity setting, which multiplies all of this.
    constexpr float kDegreesPerCount = 0.022f;              // the PC game's, at a `sensitivity` of 1
    constexpr float kDeadZone = 0.2f, kDeadZoneTop = 0.01f;
    constexpr float kTurnAcross[2] = { 260.0f, 90.0f };     // from the hip, down the sights
    constexpr float kTurnUpDown[2] = { 90.0f, 55.0f };
    // How long the sights take to come up. The title blends its two rates by
    // how far up they are, which depends on the weapon and is not known here.
    constexpr float kSightsSeconds = 0.25f;
    // A full stick turns only so fast, and what a quick hand asked for beyond
    // it waits for the next frames -- this many seconds of full stick at most,
    // or the view would go on turning long after the hand had stopped.
    constexpr float kCarrySeconds = 0.05f;

    struct MouseLook
    {
        float x = 0, y = 0;         // counts the view has not turned by yet
        float sights = 0;           // 0 from the hip to 1 down the sights
        std::chrono::steady_clock::time_point read{};
        bool reading = false;       // `read` is a time
        bool turning = false;       // the last read moved the stick,
        int16_t rx = 0, ry = 0;     // to here
    };

    struct MouseButton
    {
        bool down = false, menu = false, dead = false;
        // Whether the button is pressed, as far as the title is to know.
        bool Held(bool isDown, bool inMenu, bool nothingUnder)
        {
            if (isDown && !down) { menu = inMenu; dead = nothingUnder; }
            down = isDown;
            return down && !dead && menu == inMenu;
        }
    };

    uint16_t MouseInputs(PPCContext& ctx, uint8_t* base, bool menu, uint8_t triggers[2], int16_t& rx, int16_t& ry)
    {
        static MouseLook look;
        static MouseButton left, right, middle;
        static float pointerX = -1.0f, pointerY = -1.0f;    // where the pointer was last looked under

        vk::Mouse mouse;
        vk::TakeMouse(menu && menu_pointer::There(), mouse);
        // SDL's buttons, which leave out the click that brought the window to
        // the front: that one was aimed at the window, not at anybody.
        SDL_MouseButtonFlags buttons = mouse.held || mouse.pointing ? SDL_GetMouseState(nullptr, nullptr) : 0;
        if (g_scriptMouse.pointing)
        {
            mouse.held = false;
            mouse.pointing = menu && menu_pointer::There();
            mouse.x = g_scriptMouse.x;
            mouse.y = g_scriptMouse.y;
        }
        if (g_scriptMouse.click) buttons |= SDL_BUTTON_LMASK;
        if (g_scriptMouse.turning)
        {
            mouse.held = true;
            mouse.pointing = false;
            mouse.dx += std::exchange(g_scriptMouse.dx, 0.0f);
            mouse.dy += std::exchange(g_scriptMouse.dy, 0.0f);
        }

        // What is under the pointer takes the focus when the pointer moves,
        // not while it rests, so the keys still walk a menu it lies on; and
        // again as the left button goes down, which acts on that and on
        // nothing when the pointer is beside everything.
        const bool pressed = (buttons & SDL_BUTTON_LMASK) && !left.down;
        bool nothingUnder = false;
        if (mouse.pointing && (pressed || mouse.x != pointerX || mouse.y != pointerY))
        {
            pointerX = mouse.x;
            pointerY = mouse.y;
            nothingUnder = menu_pointer::At(ctx, base, mouse.x, mouse.y) == menu_pointer::Under::Nothing;
        }

        uint16_t b = WheelTaps(mouse.wheel, menu);
        if (left.Held(buttons & SDL_BUTTON_LMASK, menu, nothingUnder))
        {
            if (menu) b |= BTN_A;
            else triggers[1] = 255;
        }
        if (right.Held(buttons & SDL_BUTTON_RMASK, menu, false))
        {
            if (menu) b |= BTN_B;
            else triggers[0] = 255;
        }
        if (middle.Held(buttons & SDL_BUTTON_MMASK, menu, menu)) b |= BTN_RTHUMB;

        // The view turns only in the world, with the mouse held.
        if (menu || !mouse.held)
        {
            look = MouseLook{};
            return b;
        }
        // MW2_MOUSE_SENSITIVITY is the PC game's `sensitivity`, 5 as there.
        static const float degrees =
            kDegreesPerCount * float(std::clamp(env::Real("MW2_MOUSE_SENSITIVITY", 5.0), 0.1, 100.0));
        // Where the title's view takes an angle, the counts are that angle
        // and the stick is left alone (mouse_look.h).
        if (mouse_look::There())
        {
            mouse_look::Turn(mouse.dx * degrees, mouse.dy * degrees);
            return b;
        }
        look.x += mouse.dx;
        look.y += mouse.dy;
        const auto now = std::chrono::steady_clock::now();
        const float since = look.reading ? std::chrono::duration<float>(now - look.read).count() : 0.0f;
        // The title may read the pad more than once a frame. The later reads
        // answer as the first did, and their counts wait for the next frame:
        // spread over the moment between two reads they would be a full stick.
        if (look.reading && since < 0.002f)
        {
            if (look.turning) { rx = look.rx; ry = look.ry; }
            return b;
        }
        // The first read, or the first after a pause (the window was away, a
        // map loaded): the frame is not as long as the pause was.
        const float seconds = look.reading && since < 0.25f ? since : 1.0f / 60.0f;
        look.reading = true;
        look.read = now;
        look.turning = false;

        // A trigger past gpad_button_deadzone (0.13) is pulled, the left one
        // being the sights: the pad's or the right mouse button.
        const float step = seconds / kSightsSeconds;
        look.sights = std::clamp(triggers[0] > 33 ? 1.0f : 0.0f, look.sights - step, look.sights + step);
        if (look.x == 0.0f && look.y == 0.0f) return b;

        // The counts a full stick turns in a second, and with them the stick
        // these counts ask for: past 1 when the hand is quicker than that.
        const float across = std::lerp(kTurnAcross[0], kTurnAcross[1], look.sights) / degrees;
        const float upDown = std::lerp(kTurnUpDown[0], kTurnUpDown[1], look.sights) / degrees;
        float sx = look.x / (across * seconds), sy = look.y / (upDown * seconds);
        const float asked = std::hypot(sx, sy);
        const float given = std::min(asked, 1.0f);
        sx /= asked;
        sy /= asked;
        const float carried = std::min(asked - given, kCarrySeconds / seconds);
        look.x = sx * carried * across * seconds;
        look.y = sy * carried * upDown * seconds;

        // Onto the stick beyond the dead zone, whose direction the title
        // keeps. The guest's Y axis points up, the mouse's down.
        const float stick = (kDeadZone + (1.0f - kDeadZone - kDeadZoneTop) * given) * 32767.0f;
        look.rx = rx = int16_t(std::lround(sx * stick));
        look.ry = ry = int16_t(std::lround(-sy * stick));
        look.turning = true;
        return b;
    }
#endif

    // Whether the keyboard and mouse are user 0's controller: on Windows,
    // whenever the game has a window. The title asks XamInputGetCapabilities
    // about every user each frame and listens only to those it says are
    // there, so a controller that exists just while a key is held is never
    // heard: with no pad plugged in, the menus ignored every key. Like the
    // touch controls on Android, the keyboard therefore always answers; which
    // keys count, and when, is still XamInputGetState's business.
    bool KeyboardStandsIn()
    {
#ifdef _WIN32
        return SDL_WasInit(SDL_INIT_VIDEO) != 0;
#else
        return false;
#endif
    }
#endif

    // MW2_INPUT_SCRIPT="2.5:start,6:a" presses buttons at fixed times, so a title
    // can be driven to a screen without a hand on the controller, and a bring-up
    // run reproduces the same path twice.
    //
    // An entry may name `lt` or `rt` for a trigger rather than a button, or a
    // stick direction -- `lx+ lx- ly+ ly- rx+ rx- ry+ ry-` -- and may carry a
    // third field giving how long to hold it: "40:lt:25" aims down the sights for
    // twenty-five seconds. Some of what the renderer does only happens while a
    // trigger is held -- MW2 turns its depth of field on when the player aims --
    // and some screens only respond to a stick: the first-boot brightness screen
    // will not accept anything until the slider has been moved, so a run that
    // only presses buttons sits on it forever. A name ending in `@2`, `@3` or
    // `@4` is that player's controller, which the script then stands in for:
    // "5:a@2" is the second player's A. The first player's mouse can be
    // scripted too (Windows): `at<x>/<y>` rests the pointer at that part of
    // the frame while a menu is up, "30:at0.2/0.45:1" a fifth of the way
    // across and 0.45 of the way down for a second; `click` holds the left
    // button; and `turn<dx>/<dy>` moves the mouse that many counts, evenly
    // over the hold time, "80:turn1000/0:1" a thousand to the right in a second.
    // `bit` names a button; the trigger and stick pseudo-names set `trigger` or
    // `axis` instead, and `held` is how long the press lasts.
    struct Press
    {
        double at; uint16_t bit; int trigger = -1; double held = 0.2;
        int axis = -1; int16_t value = 0; uint32_t user = 0;
        enum class Mouse { None, Pointer, Click, Turn } mouse = Mouse::None;
        float x = 0, y = 0;     // the pointer's place, or a turn's counts
        float sent = 0;         // how much of a turn has been handed over, of 1
    };
    std::vector<Press> g_script;
    uint8_t g_scriptTriggers[4][2]{};
    int16_t g_scriptAxes[4][4]{};
    bool g_scriptAxisHeld[4][4]{};
    bool g_scripted[4]{};   // the script presses something on this controller

    // "lx+" is the left stick's X pushed fully one way, "ly-" the other.
    bool NamedAxis(const std::string& name, int& axis, int16_t& value)
    {
        if (name.size() != 3 || (name[2] != '+' && name[2] != '-')) return false;
        const int stick = name[0] == 'l' ? 0 : name[0] == 'r' ? 2 : -1;
        const int which = name[1] == 'x' ? 0 : name[1] == 'y' ? 1 : -1;
        if (stick < 0 || which < 0) return false;
        axis = stick + which;
        value = name[2] == '+' ? int16_t(32767) : int16_t(-32768);
        return true;
    }
    std::once_flag g_scriptOnce;
    std::chrono::steady_clock::time_point g_scriptStart;

    // One entry, "<seconds>:<name>[:<held>]", its time counted from `from`.
    void AddScriptEntry(const std::string& entry, double from)
    {
        const size_t colon = entry.find(':');
        if (colon == std::string::npos) return;
        const double when = from + std::atof(entry.substr(0, colon).c_str());
        std::string name = entry.substr(colon + 1);
        double held = 0.2;
        const size_t second = name.find(':');
        if (second != std::string::npos)
        {
            held = std::atof(name.substr(second + 1).c_str());
            if (held <= 0.0) held = 0.2;
            name = name.substr(0, second);
        }
        uint32_t user = 0;
        if (name.size() > 2 && name[name.size() - 2] == '@')
        {
            user = uint32_t(name.back() - '1') & 3;
            name.resize(name.size() - 2);
        }
        const size_t before = g_script.size();
        int axis = -1; int16_t value = 0;
        float x = 0, y = 0;
        if (name == "lt" || name == "rt")
            g_script.push_back({ when, 0, name == "lt" ? 0 : 1, held });
        else if (NamedAxis(name, axis, value))
            g_script.push_back({ when, 0, -1, held, axis, value });
        else if (name == "click")
            g_script.push_back({ when, 0, -1, held, -1, 0, 0, Press::Mouse::Click });
        else if (std::sscanf(name.c_str(), "at%f/%f", &x, &y) == 2)
            g_script.push_back({ when, 0, -1, held, -1, 0, 0, Press::Mouse::Pointer, x, y });
        else if (std::sscanf(name.c_str(), "turn%f/%f", &x, &y) == 2)
            g_script.push_back({ when, 0, -1, held, -1, 0, 0, Press::Mouse::Turn, x, y });
        else
            for (const Named& button : kButtons)
                if (name == button.name)
                    g_script.push_back({ when, button.bit, -1, held });
        for (size_t i = before; i < g_script.size(); i++) g_script[i].user = user;
        if (g_script.size() > before) g_scripted[user] = true;
    }

    void AddScriptEntries(const std::string& line, double from)
    {
        for (size_t at = 0; at <= line.size(); )
        {
            size_t comma = line.find(',', at);
            if (comma == std::string::npos) comma = line.size();
            AddScriptEntry(line.substr(at, comma - at), from);
            at = comma + 1;
        }
    }

    // MW2_INPUT_LIVE=<file>: a script written while the game runs. Timed
    // input cannot play a level: the prompt comes when it comes, and where
    // the player ends up after ten seconds of walking is not where the
    // script's author thought. So each line added to the file is a script
    // whose times count from the moment the line is read, "0:ly+:3,1:a", and
    // whoever writes it looks at the dumped frames in between. A line
    // starting with `!` is a console command instead: "!viewpos". The file
    // is read from where the last read ended, a line once its newline is
    // there, ten times a second.
    void ReadLiveScript(double now)
    {
        static const char* path = diag::Text("MW2_INPUT_LIVE");
        static double next = 0.0;
        static long offset = 0;
        static std::string partial;
        if (!path || now < next) return;
        next = now + 0.1;
        std::FILE* file = std::fopen(path, "rb");
        if (!file) return;
        char buffer[1024];
        size_t got = 0;
        if (std::fseek(file, offset, SEEK_SET) == 0)
            while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
            {
                partial.append(buffer, got);
                offset += long(got);
            }
        std::fclose(file);
        for (size_t end; (end = partial.find('\n')) != std::string::npos; partial.erase(0, end + 1))
        {
            std::string line = partial.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            LOGI("input: live at %.2f s: %s", now, line.c_str());
            if (line[0] == '!') console::RunNow(line.c_str() + 1);
            else AddScriptEntries(line, now);
        }
    }

    uint16_t ScriptedButtons(uint32_t user)
    {
        if constexpr (!diag::kOn) return 0;
        std::call_once(g_scriptOnce, [] {
            g_scriptStart = std::chrono::steady_clock::now();
            const char* text = diag::Text("MW2_INPUT_SCRIPT");
            if (!text) return;
            AddScriptEntries(text, 0.0);
            static const char* kAxisNames[4] = { "left stick X", "left stick Y",
                                                 "right stick X", "right stick Y" };
            for (const Press& press : g_script)
                LOGI("input: controller %u, scripted %s at %.2f s for %.2f s (%04X)", press.user,
                     press.axis >= 0 ? kAxisNames[press.axis]
                                     : press.trigger < 0 ? "press"
                                     : press.trigger ? "right trigger" : "left trigger",
                     press.at, press.held, press.bit);
        });
        if (user == 0)
            ReadLiveScript(std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_scriptStart).count());
        g_scriptTriggers[user][0] = g_scriptTriggers[user][1] = 0;
        for (int i = 0; i < 4; i++) { g_scriptAxes[user][i] = 0; g_scriptAxisHeld[user][i] = false; }
        if (user == 0) g_scriptMouse.pointing = g_scriptMouse.click = g_scriptMouse.turning = false;
        if (!g_scripted[user]) return 0;

        // A press lasts long enough for a title polling at 60 Hz to see it and
        // short enough not to repeat, unless the entry asked to hold it.
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_scriptStart).count();
        uint16_t buttons = 0;
        for (Press& press : g_script)
        {
            if (press.mouse == Press::Mouse::Turn)
            {
                // The counts due by now, whichever poll comes for them: the
                // last of them may fall due after the hold time is over, and
                // the view goes on turning for a moment after that.
                if (press.user != user || now < press.at || now >= press.at + press.held + 0.5) continue;
                const float done = float(std::min((now - press.at) / press.held, 1.0));
                g_scriptMouse.dx += press.x * (done - press.sent);
                g_scriptMouse.dy += press.y * (done - press.sent);
                press.sent = done;
                g_scriptMouse.turning = true;
                continue;
            }
            if (press.user != user || now < press.at || now >= press.at + press.held) continue;
            if (press.mouse == Press::Mouse::Pointer)
            {
                g_scriptMouse.pointing = true;
                g_scriptMouse.x = press.x;
                g_scriptMouse.y = press.y;
            }
            else if (press.mouse == Press::Mouse::Click) g_scriptMouse.click = true;
            else if (press.axis >= 0)
            {
                g_scriptAxes[user][press.axis] = press.value;
                g_scriptAxisHeld[user][press.axis] = true;
            }
            else if (press.trigger < 0) buttons |= press.bit;
            else                        g_scriptTriggers[user][press.trigger] = 255;
        }
        return buttons;
    }

    // A scripted stick entry moves nothing on its own, so a run with no pad
    // attached still has to take the path that applies it.
    bool ScriptedAxesHeld(uint32_t user)
    {
        for (bool held : g_scriptAxisHeld[user]) if (held) return true;
        return false;
    }

    // The packet number tells the title whether anything has changed since it
    // last looked. Incrementing it every poll says "changed" sixty times a second.
    struct Latch { uint32_t packet = 0; uint16_t buttons = 0; uint8_t triggers[2]{}; int16_t axes[4]{}; };
    Latch g_latch[4];
}

PPC_FUNC(__imp__XamInputGetState)
{
    // The title polls input once a frame from its main thread, which makes this
    // the cheapest place to hand the engine a scripted console command.
    online::Frame();
    console::Pump(ctx, base);

    uint32_t user  = ctx.r3.u32;
    uint32_t sAddr = ctx.r5.u32;
    auto* out = GuestPtr<XInputState>(sAddr);
    if (!out) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

#ifdef MW2_ANDROID
    // The touch controls and any physical pad, merged by the Android layer.
    // User 0 always answers: the on-screen pad stands in for a controller,
    // and a title told there is none on user 0 stops on its "please
    // reconnect the controller" screen and waits there.
    {
        // A controller the Android layer does not have falls through to the
        // diagnostic input script below, as on a desktop build without SDL.
        android::input::Pad pad;
        if (android::input::Poll(user, pad))
        {
            // The diagnostic input script presses on top of the pad, as on the
            // desktop: a measurement run is the same path twice with no hand
            // on the screen.
            pad.buttons |= ScriptedButtons(user & 3);
            // The sign-in screen's bookkeeping: which controller pressed last
            // is the one the title's "Do you wish to sign in?" is answered
            // for (signin::Ask), and a press that answered it is not the
            // title's too.
            if (signin::Input(user & 3, pad.buttons)) pad.buttons = 0;
            int16_t axes[4] = { pad.leftX, pad.leftY, pad.rightX, pad.rightY };
            uint8_t triggers[2] = { std::max(pad.leftTrigger, g_scriptTriggers[user & 3][0]),
                                    std::max(pad.rightTrigger, g_scriptTriggers[user & 3][1]) };
            if (user == 0)
            {
                // The same hand-off the desktop build makes: what the player did
                // is seen first, then the autopilot may add to it.
                player::SawPad(pad.buttons, triggers[1]);
                player::Press press = player::Press::None;
                if (player::Steer(axes, press))
                    switch (press)
                    {
                    case player::Press::A:    pad.buttons |= 0x1000; break;
                    case player::Press::Up:   pad.buttons |= 0x0001; break;
                    case player::Press::Left: pad.buttons |= 0x0004; break;
                    case player::Press::Fire: triggers[1] = 255; break;
                    case player::Press::None: break;
                    }
            }
            for (int i = 0; i < 4; i++)
                if (g_scriptAxisHeld[user & 3][i]) axes[i] = g_scriptAxes[user & 3][i];

            Latch& latch = g_latch[user & 3];
            const bool changed = latch.buttons != pad.buttons ||
                                 std::memcmp(latch.triggers, triggers, sizeof triggers) != 0 ||
                                 std::memcmp(latch.axes, axes, sizeof axes) != 0;
            if (changed)
            {
                latch.packet++;
                latch.buttons = pad.buttons;
                std::memcpy(latch.triggers, triggers, sizeof triggers);
                std::memcpy(latch.axes, axes, sizeof axes);
            }

            out->packet               = latch.packet;
            out->gamepad.buttons      = pad.buttons;
            out->gamepad.leftTrigger  = triggers[0];
            out->gamepad.rightTrigger = triggers[1];
            out->gamepad.thumbLX      = uint16_t(axes[0]);
            out->gamepad.thumbLY      = uint16_t(axes[1]);
            out->gamepad.thumbRX      = uint16_t(axes[2]);
            out->gamepad.thumbRY      = uint16_t(axes[3]);
            ctx.r3.u64 = X_ERROR_SUCCESS;
            return;
        }
    }
#endif

#ifdef MW2_USE_SDL
    if (user >= 4) { *out = XInputState{}; ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    SDL_Gamepad* pad = PadFor(user);
    const uint16_t elsewhere = uint16_t((user == 0 ? KeyboardButtons() : 0) | ScriptedButtons(user));
    if (pad || elsewhere ||
        (user ? g_scripted[user]
              : g_scripted[0] || ScriptedAxesHeld(0) || player::Wanted() || KeyboardStandsIn()))
    {

        if (pad) SDL_UpdateGamepads();
        uint16_t b = elsewhere;
        auto down = [&](SDL_GamepadButton x){ return pad && SDL_GetGamepadButton(pad, x); };
        if (down(SDL_GAMEPAD_BUTTON_DPAD_UP))        b |= BTN_DPAD_UP;
        if (down(SDL_GAMEPAD_BUTTON_DPAD_DOWN))      b |= BTN_DPAD_DOWN;
        if (down(SDL_GAMEPAD_BUTTON_DPAD_LEFT))      b |= BTN_DPAD_LEFT;
        if (down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     b |= BTN_DPAD_RIGHT;
        if (down(SDL_GAMEPAD_BUTTON_START))          b |= BTN_START;
        if (down(SDL_GAMEPAD_BUTTON_BACK))           b |= BTN_BACK;
        if (down(SDL_GAMEPAD_BUTTON_LEFT_STICK))     b |= BTN_LTHUMB;
        if (down(SDL_GAMEPAD_BUTTON_RIGHT_STICK))    b |= BTN_RTHUMB;
        if (down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= BTN_LB;
        if (down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= BTN_RB;
        if (down(SDL_GAMEPAD_BUTTON_SOUTH))          b |= BTN_A;
        if (down(SDL_GAMEPAD_BUTTON_EAST))           b |= BTN_B;
        if (down(SDL_GAMEPAD_BUTTON_WEST))           b |= BTN_X;
        if (down(SDL_GAMEPAD_BUTTON_NORTH))          b |= BTN_Y;

        // A flash hunt (MW2_FLASH_HUNT, tools/flash_hunt.sh) takes the pad's Y
        // as its mark, because reaching for F7 mid-fight is not going to
        // happen. The title does not see it: Y switches weapon, and the
        // switch would change the very frames the mark keeps. A stutter hunt
        // (MW2_STUTTERS) marks a hitch with it the same way.
        static const bool hunting = diag::Flag("MW2_FLASH_HUNT") || stutters::On();
        if (hunting && user == 0 && pad)
        {
            static bool wasDown = false;
            const bool isDown = down(SDL_GAMEPAD_BUTTON_NORTH);
            if (isDown && !wasDown)
            {
                if (stutters::On()) stutters::Mark("pad Y");
                else vk::renderer::FlashSeen("pad Y");
            }
            wasDown = isDown;
            b &= uint16_t(~BTN_Y);
        }

        auto axis = [&](SDL_GamepadAxis a){ return pad ? SDL_GetGamepadAxis(pad, a) : int16_t(0); };
        uint8_t triggers[2] = {
            uint8_t(std::max<int>(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7,
                                  g_scriptTriggers[user][0])),
            uint8_t(std::max<int>(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7,
                                  g_scriptTriggers[user][1])) };
        // The guest's Y axes point up; SDL's point down.
        int16_t axes[4] = { axis(SDL_GAMEPAD_AXIS_LEFTX),
                            int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY)),
                            axis(SDL_GAMEPAD_AXIS_RIGHTX),
                            int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY)) };
        if (user == 0)
        {
            const bool menu = MenuUp(base);
            b |= EscapeButton(menu);
#ifdef _WIN32
            // The mouse answers only while the window has it, a scripted one
            // whenever the script says. Under the scoreboard it is the
            // world's, as the controller is.
            b |= MouseInputs(ctx, base, menu && !menu_pointer::OverWorld(base), triggers, axes[2], axes[3]);
            // The keyboard stands in for a pad on Windows, and only
            // while the game's window has the keyboard: GetAsyncKeyState
            // answers for the whole desktop, and keys typed into another
            // window are not the player's.
            if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetKeyboardFocus())
            {
                KeyboardMoveAxes(axes[0], axes[1]);
                auto isDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
                if (isDown(VK_RETURN))                      b |= BTN_START;
                if ((isDown(VK_TAB) && !isDown(VK_MENU)) || isDown(VK_BACK)) b |= BTN_BACK;
                if (isDown(VK_UP))                          b |= BTN_DPAD_UP;
                if (isDown(VK_DOWN))                        b |= BTN_DPAD_DOWN;
                if (isDown(VK_LEFT))                        b |= BTN_DPAD_LEFT;
                if (isDown(VK_RIGHT))                       b |= BTN_DPAD_RIGHT;

                if (isDown(VK_SPACE) || isDown('Z') || isDown(VK_RETURN)) b |= BTN_A;
                if (isDown(VK_CONTROL) || isDown('X'))                    b |= BTN_B;
                if (isDown('C') || isDown('R') || isDown('F'))            b |= BTN_X;
                if (isDown('V') || isDown('1') || isDown('2') || isDown('Y')) b |= BTN_Y;

                if (isDown('Q') || isDown('4'))                           b |= BTN_LB;
                if (isDown('G') || isDown('E'))                           b |= BTN_RB;

                if (isDown(VK_SHIFT))                                     b |= BTN_LTHUMB;

                // The stick only: the D-pad is a control of its own, and the
                // title binds other things to it.
                if (isDown('W')) axes[1] = 32767;
                if (isDown('S')) axes[1] = -32768;
                if (isDown('D')) axes[0] = 32767;
                if (isDown('A')) axes[0] = -32768;
            }
#endif

            // What a person did, before the autopilot adds to it: a recorded
            // route keeps the presses (MW2_RECORD_PATH), and every shot is logged.
            player::SawPad(b, triggers[1]);
            player::Press press = player::Press::None;
            if (player::Steer(axes, press))
                switch (press)
                {
                case player::Press::A:    b |= BTN_A; break;
                case player::Press::Up:   b |= BTN_DPAD_UP; break;
                case player::Press::Left: b |= BTN_DPAD_LEFT; break;
                case player::Press::Fire: triggers[1] = 255; break;
                case player::Press::None: break;
                }
        }
        // A scripted stick entry overrides the pad and the autopilot while it is
        // held: a script that says where to look means it.
        for (int i = 0; i < 4; i++) if (g_scriptAxisHeld[user][i]) axes[i] = g_scriptAxes[user][i];

        // The system's screens take the controllers while one is open, the
        // left stick moving through it as the d-pad does: the friends screen,
        // the guide, then the sign-in screen, each reached only while none
        // before it is open. The Guide button -- or Back and Start together,
        // where the system keeps that button for itself, or Home -- opens the
        // guide at the first controller and the sign-in screen at the others.
        const uint16_t stick = axes[1] > 16000 ? BTN_DPAD_UP : axes[1] < -16000 ? BTN_DPAD_DOWN : uint16_t(0);
        const bool guide = down(SDL_GAMEPAD_BUTTON_GUIDE) || (b & (BTN_BACK | BTN_START)) == (BTN_BACK | BTN_START);
        const uint16_t held = uint16_t(b | stick | (guide ? signin::kGuide : 0));
        const bool picking = friends::Input(user, held);
        const bool guided = guide::Input(user, picking ? uint16_t(0) : held) || picking;
        const bool taken = signin::Input(user, guided ? uint16_t(0) : held) || guided;
        b &= uint16_t(~signin::kGuide);
        if (taken)
        {
            b = 0;
            triggers[0] = triggers[1] = 0;
            axes[0] = axes[1] = axes[2] = axes[3] = 0;
        }

        Latch& latch = g_latch[user];
        const bool changed = latch.buttons != b ||
                             std::memcmp(latch.triggers, triggers, sizeof triggers) != 0 ||
                             std::memcmp(latch.axes, axes, sizeof axes) != 0;
        if (changed)
        {
            latch.packet++;
            latch.buttons = b;
            std::memcpy(latch.triggers, triggers, sizeof triggers);
            std::memcpy(latch.axes, axes, sizeof axes);
        }

        out->packet                = latch.packet;
        out->gamepad.buttons       = b;
        out->gamepad.leftTrigger   = triggers[0];
        out->gamepad.rightTrigger  = triggers[1];
        out->gamepad.thumbLX       = uint16_t(axes[0]);
        out->gamepad.thumbLY       = uint16_t(axes[1]);
        out->gamepad.thumbRX       = uint16_t(axes[2]);
        out->gamepad.thumbRY       = uint16_t(axes[3]);
        ctx.r3.u64 = X_ERROR_SUCCESS;
        return;
    }
    // Nothing is held at a controller that is not there -- the keyboard's
    // with every key up: the screens hear it, so that their next press of a
    // key counts as one.
    if (user < 4)
    {
        friends::Input(user, 0);
        guide::Input(user, 0);
        signin::Input(user, 0);
    }
#else
    if (user < 4)
    {
        uint16_t buttons = ScriptedButtons(user);
        // As above: a scripted controller answers the sign-in question too.
        if (signin::Input(user, buttons)) buttons = 0;
        if (g_scripted[user])
        {
            Latch& latch = g_latch[user];
            const bool changed = latch.buttons != buttons ||
                std::memcmp(latch.triggers, g_scriptTriggers[user], sizeof latch.triggers) != 0 ||
                std::memcmp(latch.axes, g_scriptAxes[user], sizeof latch.axes) != 0;
            if (changed)
            {
                ++latch.packet;
                if (diag::Flag("MW2_INPUT_TRACE"))
                {
                    const double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - g_scriptStart).count();
                    LOGI("input: delivered controller %u at %.3f s packet %u buttons %04X triggers %u/%u axes %d/%d/%d/%d",
                         user, elapsed, latch.packet, unsigned(buttons),
                         unsigned(g_scriptTriggers[user][0]), unsigned(g_scriptTriggers[user][1]),
                         int(g_scriptAxes[user][0]), int(g_scriptAxes[user][1]),
                         int(g_scriptAxes[user][2]), int(g_scriptAxes[user][3]));
                }
            }
            latch.buttons = buttons;
            std::memcpy(latch.triggers, g_scriptTriggers[user], sizeof latch.triggers);
            std::memcpy(latch.axes, g_scriptAxes[user], sizeof latch.axes);
            *out = XInputState{};
            out->packet = latch.packet;
            out->gamepad.buttons = buttons;
            out->gamepad.leftTrigger = latch.triggers[0];
            out->gamepad.rightTrigger = latch.triggers[1];
            out->gamepad.thumbLX = uint16_t(latch.axes[0]);
            out->gamepad.thumbLY = uint16_t(latch.axes[1]);
            out->gamepad.thumbRX = uint16_t(latch.axes[2]);
            out->gamepad.thumbRY = uint16_t(latch.axes[3]);
            ctx.r3.u64 = X_ERROR_SUCCESS;
            return;
        }
    }
#endif
    *out = XInputState{};
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
}

PPC_FUNC(__imp__XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32;
    auto* caps = GuestPtr<XInputCapabilities>(ctx.r5.u32);
    if (!caps) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (user < 4) ScriptedButtons(user); // Capabilities may be queried before state.
    bool connected = user < 4 && g_scripted[user];
#ifdef MW2_ANDROID
    connected = connected || android::input::Connected(user);
#endif
#ifdef MW2_USE_SDL
    connected = connected || PadFor(user) || (user == 0 && (player::Wanted() || KeyboardStandsIn()));
#endif
    if (connected)
    {
        // What a wired 360 pad reports: every control it has at full range,
        // and both motors.
        *caps = XInputCapabilities{};
        caps->type = 1;      // XINPUT_DEVTYPE_GAMEPAD
        caps->subType = 1;   // XINPUT_DEVSUBTYPE_GAMEPAD
        caps->gamepad.buttons = 0xF3FF;
        caps->gamepad.leftTrigger = caps->gamepad.rightTrigger = 0xFF;
        caps->gamepad.thumbLX = caps->gamepad.thumbLY = uint16_t(0xFFFF);
        caps->gamepad.thumbRX = caps->gamepad.thumbRY = uint16_t(0xFFFF);
        caps->vibration.left = caps->vibration.right = uint16_t(0xFFFF);
        ctx.r3.u64 = X_ERROR_SUCCESS;
        return;
    }
    *caps = XInputCapabilities{};
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
}

// XamInputSetState(user, flags, vibration): the left motor is the heavy, low
// frequency one, as SDL's first argument is. A 360 motor keeps its speed until
// the next call, so the rumble is given SDL's longest duration rather than
// none: with none, SDL also stops resending it to pads that let a rumble lapse.
PPC_FUNC(__imp__XamInputSetState)
{
    const uint32_t user = ctx.r3.u32;
    if (user >= 4) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    auto* v = GuestPtr<XInputVibration>(ctx.r5.u32);
    if (!v) { ctx.r3.u64 = X_ERROR_BAD_ARGUMENTS; return; }
#ifdef MW2_ANDROID
    // The phone's vibrator, or a pad that has motors of its own: the app
    // decides which, and whether the player wanted either.
    android::input::Rumble(user, v->left, v->right);
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
#endif
#ifdef MW2_USE_SDL
    if (SDL_Gamepad* pad = PadFor(user))
    {
        std::lock_guard lock(g_padLock);
        if (g_pads[user] == pad)
        {
            const uint16_t left = v->left, right = v->right;
            const bool ok = SDL_RumbleGamepad(pad, left, right, 0xFFFF);
            // Said once per pad: the first rumble, or that the pad has none.
            static bool told[4];
            if ((left || right || !ok) && !told[user])
            {
                told[user] = true;
                if (ok) LOGI("input: controller %u rumbles (%04X, %04X)", user, left, right);
                else    LOGW("input: controller %u cannot rumble: %s", user, SDL_GetError());
            }
            ctx.r3.u64 = X_ERROR_SUCCESS;
            return;
        }
    }
#endif
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
}

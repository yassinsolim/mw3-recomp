// MW2_TRACE_AIM=1, Advanced Warfare's multiplayer: the local player's aim
// down the sight, frame by frame, and the times the held weapon's asset gives
// for it.
//
// A tester saw the weapon come up to the sight in a few frames after a
// sprint, where from a standstill it takes a quarter of a second, and asked
// whether an animation was missing. It is not: the title's own movement code
// starts the aim the moment the sprint ends and runs it under the weapon's
// sprint-out state, at the weapon's slower "from a sprint" rate, so that when
// the sprint-out ends (the ASM1: 200 ms) the aim is already two thirds done
// and what is left of it takes 100 ms. This trace is what shows it, on any
// device and with any weapon: every frame of an aim with the player state's
// clock, the weapon state and the fraction, then one line with the aim's
// length beside the asset's.
//
// The player state is the client's predicted one (title.h,
// T_DATA_PredictedPlayerState), read once a present: its command time is the
// game's clock, so a frame's step is what the movement code was given, and a
// long frame shows as a long step and not as a wrong rate.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "weapon_timing.h"
#include "guest.h"
#include "log.h"

#include <bit>
#include <cstdint>

#if MW2_DIAGNOSTICS && defined(T_DATA_PredictedPlayerState)

namespace
{
    uint32_t Word(uint32_t address) { return *GuestPtr<be32>(address); }
    float Real(uint32_t address) { return std::bit_cast<float>(Word(address)); }

    struct Aim
    {
        uint32_t weapon = ~0u;      // the weapon whose times were last logged
        uint32_t time = 0;          // the command time of the frame before
        float fraction = 0.0f;      // and its aim
        // The aim in progress: when it began, in which weapon state, when
        // that state ended and how far the aim was then.
        bool rising = false;
        uint32_t began = 0, state = 0, stateEnded = 0, frames = 0;
        float atStateEnd = 0.0f;
    };
    Aim g;

    uint32_t Definition(uint32_t weapon)
    {
        return Word(T_DATA_WeaponTable + (weapon & T_PS_HELD_WEAPON_INDEX) * 4);
    }
}

void weapon_timing::Frame()
{
    static const bool on = diag::Flag("MW2_TRACE_AIM");
    if (!on) return;
    const uint32_t state = T_DATA_PredictedPlayerState;
    const uint32_t time = Word(state + T_PS_COMMAND_TIME);
    if (time == g.time) return;         // no movement frame since the last present
    const uint32_t weapon = Word(state + T_PS_HELD_WEAPON);
    const uint32_t weaponState = Word(state + T_PS_WEAPON_STATE) & T_PS_WEAPON_STATE_KIND;
    const uint32_t weaponTime = Word(state + T_PS_WEAPON_TIME);
    const float fraction = Real(state + T_PS_AIM_FRACTION);
    const uint32_t definition = Definition(weapon);
    if (!definition) { g.time = time; return; }   // not in a match
    if (weapon != g.weapon)
    {
        g.weapon = weapon;
        LOGI("aim: holding %s: sprint in %u ms, sprint out %u ms, aim in %u ms, aim in from a sprint %u ms, aim out %u ms",
             GuestPtr<char>(Word(definition + T_WEAPON_NAME)), Word(definition + T_WEAPON_SPRINT_IN),
             Word(definition + T_WEAPON_SPRINT_OUT), Word(definition + T_WEAPON_AIM_IN),
             Word(definition + T_WEAPON_AIM_IN_FROM_SPRINT), Word(definition + T_WEAPON_AIM_OUT));
    }
    if (fraction > g.fraction && !g.rising)
    {
        // The first step is part of the aim: it began with the frame before.
        g.rising = true;
        g.began = g.time;
        g.state = weaponState;
        g.stateEnded = 0;
        g.frames = 0;
    }
    if (g.rising)
    {
        g.frames++;
        if (!g.stateEnded && weaponState != g.state)
        {
            g.stateEnded = time;
            g.atStateEnd = fraction;
        }
        LOGI("aim: %u (+%u ms): weapon state %u with %u ms left, aim %.3f", time, time - g.time,
             weaponState, weaponTime, fraction);
        if (fraction >= 1.0f)
        {
            if (g.stateEnded)
                LOGI("aim: at the sight %u ms and %u frames after it began in weapon state %u, which ended after %u ms with the aim at %.2f",
                     time - g.began, g.frames, g.state, g.stateEnded - g.began, g.atStateEnd);
            else
                LOGI("aim: at the sight %u ms and %u frames after it began, in weapon state %u throughout",
                     time - g.began, g.frames, g.state);
            g.rising = false;
        }
        else if (fraction < g.fraction) g.rising = false;   // let go before the sight
    }
    g.time = time;
    g.fraction = fraction;
}

#elif MW2_DIAGNOSTICS

void weapon_timing::Frame() {}

#endif

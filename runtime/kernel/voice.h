#pragma once
#include <cstdint>

// Voice chat: the headset XAM gives the title's voice library (XHV), made of
// the host's microphone and the game's own sound output. voice.cpp has the
// whole of it; this is the one call the mixer makes.
namespace kernel::voice
{
    // Whether the player turned voice chat on (MW2_VOICE=1), in a title that
    // has it.
    bool On();

    // Adds what the other players are saying to one mixer frame: `frames`
    // samples of `channels` interleaved floats at 48 kHz, front left and right
    // first. Does nothing while voice chat is off or nobody is speaking.
    void Mix(float* interleaved, uint32_t frames, uint32_t channels);
}

#pragma once
// MW2_TRACE_MOVIES=1: when a Bink movie's pictures are decoded and when the
// sound beside them is handed to the output, on one clock, to tell how far
// apart a cutscene's picture and sound are and whether that grows.
//
// The log gets a line for every movie opened (its size, length and rate), one
// for every picture (its number, when the title stepped past it and how long
// its decode took) and one for every sixteenth mixer frame (its number, which
// is its place in MW2_AUDIO_DUMP's file, when it was submitted and how long
// the output will hold it), and one for every read of a movie's or a streamed
// sound's file, or any that was slow (how much, from where and how long it
// took). A movie's own sound track, decoded
// from the file and found in the dump, then says which mixer frame belongs to
// which picture. Off, a call here is one load of a flag; without diagnostics,
// nothing.
#include "diagnostics.h"

namespace movies
{
#if MW2_DIAGNOSTICS
    bool On();
    // Milliseconds on the clock the lines are stamped with.
    double Clock();
    // A read of `name`, traced if that is a movie's or a streamed sound's
    // file, or the read was slow.
    void Read(const char* name, uint32_t bytes, long long offset, double tookMs);
    // The mixer submitted its next frame. `queuedMs` is what the output's
    // queue holds ahead of it and `deviceMs` what the device does after
    // that, negative where the output does not say; `underruns` counts the
    // times the device found the queue empty.
    void Sound(double queuedMs, double deviceMs, uint64_t underruns);
#else
    inline bool On() { return false; }
    inline double Clock() { return 0.0; }
    inline void Read(const char*, uint32_t, long long, double) {}
    inline void Sound(double, double, uint64_t) {}
#endif
}

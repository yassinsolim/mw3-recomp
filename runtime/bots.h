#pragma once

// Bots for Modern Warfare 3's multiplayer (bots.cpp): players the host's own
// server adds and plays, so a match can be played alone or filled up.
namespace bots
{
    // A console line the runtime answers itself instead of the title: `bots <n>`,
    // `bots +`, `bots -`, `bots skill <0-3>`. True when the line was one of them;
    // the server acts on it on its next frame. Any thread.
    bool ConsoleLine(const char* text);
}

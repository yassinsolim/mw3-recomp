#pragma once
#include <cstdint>
struct PPCContext;

// The end-of-match map vote and map rotation of Modern Warfare 3's multiplayer
// (map_vote.cpp, MW2_MAP_VOTE and MW2_MAP_ROTATION).
namespace mapvote
{
    // Once a server frame, on the server's thread: runs the vote that is open
    // and loads the map it ends on. Nothing happens between votes.
    void ServerFrame(PPCContext& ctx, uint8_t* base);
}

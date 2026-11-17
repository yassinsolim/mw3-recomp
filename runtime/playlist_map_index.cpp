// A party member takes playlist map indexes from its host with nothing bounding
// them, and uses them to read and to write the playlist table.
//
// The playlist table holds 24 rows of 10816 bytes. A row's map entries, 40
// bytes each, start at +2384, and the row holds at most 210 of them (the
// playlist parser stops there, and 2384 + 210 * 40 is where the row's entry
// count begins). Each entry starts with the map's name and gametype, and the
// word at +36 counts how often the entry has been played.
//
// The party keeps the entries it last played as 8 bytes at party+8096, with
// 255 for none (ring index at +8104). On the host they are its own picks. On a
// member they are whatever the host sends: the party-state message
// (sub_82198D80, the public-match branch) reads the 8 bytes into the ring
// verbatim, and the "go" message (sub_8219B1E8) carries one more index for a
// public match, read with atoi. Two functions use them:
//
//   sub_822E6870(localClient, index) reads that entry of the current row and
//   sets ui_mapname, ui_gametype and the row's rules from it. Its callers: the
//   "go" handler, the party-state handler and becoming host (sub_8219B4D8),
//   each with a host-sent index; the host's own pick; and one caller passing 0.
//
//   sub_822E67A0(localClient, history, count) takes one off the play count of
//   every entry the history names, if it is nonzero. Its callers: becoming
//   host (which a member does when a public lobby migrates to it), and
//   Party_OnDisconnect (sub_82198698, outside a private match), which the
//   client's disconnect and server-timeout handlers call.
//
// Neither compares the index with the row. An index outside 0..209 reads, or
// decrements a word, up to 254 * 40 bytes past the entries: into the next
// row's name and rules text, which the title later runs as commands, or past
// the table for the last row. On a console a large index read can also stop
// the title; here every guest read succeeds.
//
// Both are closed here, in the two functions, so every caller is covered:
// sub_822E6870 refuses an index outside the row and does nothing (the member
// keeps the map it had, and the row's rules are not applied for that call;
// the original read past the row for 255 as well, which a stock public host
// sends before anything has been played),
// and sub_822E67A0 treats a history byte outside the row as the empty 255 the
// function already skips. Every index inside the row, and 255, goes to the
// originals untouched.
//
// Multiplayer only: the campaign has no playlists.
//
// Modern Warfare 3's multiplayer keeps the same indexes (the history's 8 bytes
// at party+0x3D90 and a second pick at +0x3D98, both read from the host's
// partystate by sub_82186FC8; the "go" message's index, read with atoi by
// sub_821896C0) in another table: every row's entries, 4 bytes each {map,
// rules, ?, play count}, lie in one array (0x836FCFD0, 2048 of them), and a row
// (0x178 bytes at 0x836FEFD0) names its first entry (+0x170) and how many it
// has (+0x172). The functions add the index to the row's first entry and
// compare it with nothing:
//   sub_822F3B88(localClient, index) sets ui_mapname and ui_gametype from the
//     entry and runs its rules' command text;
//   sub_822F3AC0(localClient, index, which) sets a map vote's two dvars from it;
//   sub_822F3A18(localClient, history, count) takes one off each named
//     entry's play count;
//   sub_822F3D90 / sub_822F3E08(localClient, index) give the menus two texts
//     of the entry's rules.
// An index past the row reads another row's entry, or past the array the rows
// themselves as entries, and "go"'s is a whole int. The bound here is the
// row's own count, read as the functions read the row (sub_822F3610 of the
// playlist dvar). With no entries in the row, index 0 is still let through:
// the title itself asks for it (sub_8216FF48).
#include <ppc_recomp_shared.h>
#include "title.h"
#include "log.h"

#include <cstdint>

#if defined(T_Playlist_RowOf)

namespace
{
    // How many entries the current playlist's row has. The hooks run at their
    // function's entry, so the stack below r1 is free for the call. The lookup
    // puts a playlist number no row has back to the default (sub_822F3548), so
    // it is asked twice: the second answer is for the number the original will
    // read.
    uint32_t RowEntries(PPCContext& ctx, uint8_t* base, uint32_t localClient)
    {
        const PPCContext saved = ctx;
        uint32_t row = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const uint32_t dvar = PPC_LOAD_U32(T_DATA_PlaylistDvar);
            ctx.r3.u64 = localClient;
            ctx.r4.u64 = dvar ? PPC_LOAD_U32(dvar + T_DVAR_CURRENT) : 0;
            GUEST_FUNC(T_Playlist_RowOf)(ctx, base);
            row = ctx.r3.u32;
        }
        ctx = saved;
        return PPC_LOAD_U16(T_DATA_PlaylistRows + row * T_PLAYLIST_ROW_STRIDE + T_PLAYLIST_ROW_ENTRIES);
    }

    bool InRow(int32_t index, uint32_t entries)
    {
        return (index >= 0 && uint32_t(index) < entries) || (entries == 0 && index == 0);
    }

    void Refused(const char* what, int32_t index, uint32_t entries)
    {
        // 255 is the history's "none", which a host sends before anything has
        // been played: refused all the same, but not worth a warning.
        static bool logged;
        if (logged || index == 255)
            return;
        LOGW("online: refusing playlist %s %d from the party host (the playlist has %u)", what, index, entries);
        logged = true;
    }
}

// sub_822F3B88(localClient = r3, index = r4)
GUEST_HOOK(T_Playlist_SetMapEntry)
{
    const uint32_t entries = RowEntries(ctx, base, ctx.r3.u32);
    if (!InRow(ctx.r4.s32, entries))
    {
        Refused("map entry", ctx.r4.s32, entries);
        return;
    }
    GUEST_ORIG(T_Playlist_SetMapEntry)(ctx, base);
}

// sub_822F3AC0(localClient = r3, index = r4, which = r5)
GUEST_HOOK(T_Playlist_SetVoteEntry)
{
    const uint32_t entries = RowEntries(ctx, base, ctx.r3.u32);
    if (!InRow(ctx.r4.s32, entries))
    {
        Refused("vote entry", ctx.r4.s32, entries);
        return;
    }
    GUEST_ORIG(T_Playlist_SetVoteEntry)(ctx, base);
}

// sub_822F3A18(localClient = r3, history = r4, count = r5)
GUEST_HOOK(T_Playlist_UnplayHistory)
{
    const uint32_t entries = RowEntries(ctx, base, ctx.r3.u32);
    const uint32_t history = ctx.r4.u32;
    const int32_t count = ctx.r5.s32;
    for (int32_t i = 0; i < count; ++i)
    {
        const uint8_t entry = PPC_LOAD_U8(history + i);
        if (entry == 255 || entry < entries)
            continue;
        Refused("history entry", entry, entries);
        PPC_STORE_U8(history + i, 255);   // the "none" the function skips
    }
    GUEST_ORIG(T_Playlist_UnplayHistory)(ctx, base);
}

// sub_822F3D90 / sub_822F3E08(localClient = r3, index = r4) -> a text
GUEST_HOOK(T_Playlist_EntryText)
{
    if (!InRow(ctx.r4.s32, RowEntries(ctx, base, ctx.r3.u32)))
    {
        ctx.r3.u64 = T_DATA_EmptyString;
        return;
    }
    GUEST_ORIG(T_Playlist_EntryText)(ctx, base);
}

GUEST_HOOK(T_Playlist_EntryText2)
{
    if (!InRow(ctx.r4.s32, RowEntries(ctx, base, ctx.r3.u32)))
    {
        ctx.r3.u64 = T_DATA_EmptyString;
        return;
    }
    GUEST_ORIG(T_Playlist_EntryText2)(ctx, base);
}

#else

#ifdef T_Playlist_SetMapEntry

// sub_822E6870(localClient = r3, index = r4)
GUEST_HOOK(T_Playlist_SetMapEntry)
{
    const int32_t index = ctx.r4.s32;
    if (index < 0 || index >= T_PLAYLIST_MAP_ENTRIES)
    {
        // 255 is the history's "none", which a host sends before anything has
        // been played: refused all the same, but not worth a warning.
        static bool logged;
        if (!logged && index != 255)
        {
            LOGW("online: refusing playlist map entry %d from the party host", index);
            logged = true;
        }
        return;
    }
    GUEST_ORIG(T_Playlist_SetMapEntry)(ctx, base);
}

#endif

#ifdef T_Playlist_UnplayHistory

// sub_822E67A0(localClient = r3, history = r4, count = r5)
GUEST_HOOK(T_Playlist_UnplayHistory)
{
    const uint32_t history = ctx.r4.u32;
    const int32_t count = ctx.r5.s32;
    for (int32_t i = 0; i < count; ++i)
    {
        const uint8_t entry = PPC_LOAD_U8(history + i);
        if (entry == 255 || entry < T_PLAYLIST_MAP_ENTRIES)
            continue;
        static bool logged;
        if (!logged)
            LOGW("online: dropping playlist history entry %u from the party host", entry);
        logged = true;
        PPC_STORE_U8(history + i, 255);
    }
    GUEST_ORIG(T_Playlist_UnplayHistory)(ctx, base);
}

#endif

#endif

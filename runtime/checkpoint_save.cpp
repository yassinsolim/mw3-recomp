// A checkpoint reaches the save file when the title commits it.
//
// Advanced Warfare's campaign keeps a checkpoint in memory. Its scripts ask
// for one with SaveGameNoCommit and then CommitSave (sub_8239FA28), which ends
// in T_SaveMemory_Commit: the pending save becomes the committed one and
// "Checkpoint Reached" is printed. Nothing more happens there. The write to
// the device is the console command `savegame_lastcommit` (sub_825232C8: the
// committed save, sub_823C1F50, the package and the file), and the only thing
// that gives it is the menus' WriteSave (sub_821AA230 sets a byte that the
// client's frame, sub_8246BD30, turns into the command), which one menu
// calls: `save_game_info_hud`, opened by the pause menu's Save and Quit. A
// level's start is the one other write (flag 4 of the request, "Start Level
// Save"). So on the console a checkpoint is on the disc only once the player
// has chosen Save and Quit, and the title's own warning says as much.
//
// A phone's player does not leave that way: the app is put away and the
// system takes the process when it wants the memory, with no notice
// (docs/saves.md, "Surviving an interrupted write"). Resume Game then started
// the level again. So the title's own command is queued as soon as a commit
// succeeds; it runs from the command buffer between frames, where Save and
// Quit's runs, and writes the same file the same way.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest.h"
#include "console.h"
#include "env.h"
#include "log.h"

#if defined(T_SaveMemory_Commit) && defined(T_G_FinishSaveRequest)

namespace
{
    // Set while a save request that writes the file itself is being finished:
    // a level's start is committed and written in one call, and a second
    // write of the megabyte behind it would be for nothing. Requests are
    // finished on the server's thread only.
    bool g_requestWrites = false;
}

GUEST_HOOK(T_G_FinishSaveRequest)
{
    const be32* flags = GuestPtr<be32>(ctx.r3.u32 + T_SAVE_REQUEST_FLAGS);
    g_requestWrites = flags && (uint32_t(*flags) & T_SAVE_REQUEST_WRITE);
    GUEST_ORIG(T_G_FinishSaveRequest)(ctx, base);
    g_requestWrites = false;
}

GUEST_HOOK(T_SaveMemory_Commit)
{
    GUEST_ORIG(T_SaveMemory_Commit)(ctx, base);
    // 1 is a save committed; 0 is a handle that was not waiting for it.
    if (ctx.r3.u32 != 1 || g_requestWrites) return;
    static const bool leaveToTitle = env::Flag("MW2_NO_CHECKPOINT_WRITE");
    if (leaveToTitle) return;
    LOGK("content: a checkpoint was committed; writing it to the save device");
    console::RunNow("savegame_lastcommit");
}

#endif

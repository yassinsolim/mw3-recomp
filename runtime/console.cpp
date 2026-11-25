// Feeds text into IW4's own console command buffer: the shortest path to a
// level, or to any setting, is to queue the command the menu would have
// queued.
//
//   MW2_CONSOLE="30:map af_caves"          one command, at 30 seconds
//   MW2_CONSOLE="30:map af_caves;45:god"   several, separated by ';'
//
// The time is wall-clock seconds since the runtime started, the same clock
// MW2_INPUT_SCRIPT uses, so the two can be interleaved.
//
// Cbuf_AddText copies the text, so the string only has to survive the call. It
// goes just below the guest stack pointer of whichever thread is pumping --
// memory below r1 belongs to nobody -- with the call made at a stack pointer
// pushed past it, so the callee's own frame cannot reach it.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest.h"
#include "log.h"
#include "console.h"
#include "console_queue.h"
#include "bots.h"
#include "diagnostics.h"
#include "match_options.h"

#include <cstring>
#include <string>

void console::Pump(PPCContext& ctx, uint8_t* base)
{
#ifdef T_ClientEndFrame
    match_options::Frame(base);
#endif
    std::string text;
    bool placing = false;
    if (!Take(text, placing)) return;

    // Cbuf_AddText wants a newline-terminated line, the way a .cfg supplies it.
    if (text.back() != '\n') text += '\n';
    if (text.size() > 192) { LOGW("console: \"%s\" is too long", text.c_str()); return; }

    const uint32_t sp = ctx.r1.u32;
    if (sp < 0x1000) { LOGW("console: no guest stack to borrow"); return; }

    const uint32_t scratch = (sp - 0x100) & ~0xFu;
    std::memcpy(base + scratch, text.c_str(), text.size() + 1);

#ifdef T_Cbuf_AddText
    const PPCContext saved = ctx;
    ctx.r1.u64 = (sp - 0x200) & ~0xFull;
    ctx.r3.u64 = 0;                         // localClientNum
    ctx.r4.u64 = scratch;
    GUEST_FUNC(T_Cbuf_AddText)(ctx, base);
    ctx = saved;
#else
    // The title's Cbuf_AddText is not enabled (title.h), so there is no queue to feed.
    LOGW("console: no command queue for the %s", MW2_TITLE_NAME);
    return;
#endif

    if (!placing) LOGI("console: queued \"%.*s\" at %.2f s", int(text.size()) - 1, text.c_str(), Elapsed());
}

// Cbuf_AddText(localClientNum, text): what the menus and the title's own code
// queue for the console; MW2_TRACE_CBUF shows the commands a menu runs. The
// `bots` and `match` lines are the runtime's own (bots.cpp, aw_bots.cpp,
// match_options.cpp) and never reach the title, which has no such commands;
// whatever queued one -- MW2_CONSOLE, a key, the app's console entry -- comes
// through here. Only a title that has bots is hooked here: the others'
// Cbuf_AddText is engine_log.cpp's to trace.
#if (defined(T_SV_BotUserMoves) || defined(T_MatchRules_BotDefaults)) && defined(T_Cbuf_AddText)
GUEST_HOOK(T_Cbuf_AddText)
{
    if (ctx.r4.u32) {
        const char* text = reinterpret_cast<const char*>(base + ctx.r4.u32);
        static const bool traced = diag::Flag("MW2_TRACE_CBUF");
        if (traced) LOGI("cbuf(%u): %s", ctx.r3.u32, text);
        if (bots::ConsoleLine(text)) return;
#ifdef T_ClientEndFrame
        if (match_options::ConsoleLine(text)) return;
#endif
    }
    GUEST_ORIG(T_Cbuf_AddText)(ctx, base);
}
#endif

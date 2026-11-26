// Surfaces IW4's own diagnostics. The engine funnels them through Com_Error and
// Com_Printf, both printf-style, so hooking them turns the title's internal
// reasons for giving up into runtime output. Com_Error is always reported; the
// chattier print goes behind MW2_ENGINE_LOG.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "diagnostics.h"
#include "guest.h"
#include "log.h"
#include "crash.h"
#include "platform.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // PowerPC passes the first eight integer arguments in r3..r10, so a format
    // with up to six varargs needs no guest stack walking.
    struct Args
    {
        const PPCContext& ctx;
        int next = 5;                       // the first vararg: r5 after (code, format)
        uint64_t Next()
        {
            switch (next++)
            {
            case 5:  return ctx.r5.u64;
            case 6:  return ctx.r6.u64;
            case 7:  return ctx.r7.u64;
            case 8:  return ctx.r8.u64;
            case 9:  return ctx.r9.u64;
            case 10: return ctx.r10.u64;
            default: return 0;              // spilled to the stack; not rendered
            }
        }
        bool Exhausted() const { return next > 10; }
    };

    std::string Format(const PPCContext& ctx, uint32_t formatAddress, int firstArgument = 5)
    {
        if (!formatAddress) return "(null format)";
        const char* f = reinterpret_cast<const char*>(guest::Base() + formatAddress);
        Args args{ ctx, firstArgument };
        std::string out;
        char scratch[64];

        for (const char* p = f; *p; p++)
        {
            if (*p != '%') { out += *p; continue; }
            p++;
            if (*p == '%') { out += '%'; continue; }
            // The engine's messages do not use flags, width or precision.
            while (*p && !std::strchr("diuxXcsfgeEp", *p)) p++;
            if (!*p) break;
            if (args.Exhausted()) { out += "<arg spilled>"; continue; }

            const uint64_t raw = args.Next();
            const uint32_t v = uint32_t(raw);
            switch (*p)
            {
            case 's': out += v ? GuestAnsi(v) : std::string("(null)"); break;
            case 'c': out += char(v); break;
            case 'd': case 'i': std::snprintf(scratch, sizeof scratch, "%d", int32_t(v)); out += scratch; break;
            case 'u':           std::snprintf(scratch, sizeof scratch, "%u", v);          out += scratch; break;
            case 'x':           std::snprintf(scratch, sizeof scratch, "%x", v);          out += scratch; break;
            case 'X': case 'p': std::snprintf(scratch, sizeof scratch, "%X", v);          out += scratch; break;
            // A variadic double travels in the next GPR, whole: viewpos stores
            // each one and loads it back with ld before the call.
            default:  std::snprintf(scratch, sizeof scratch, "%g", std::bit_cast<double>(raw)); out += scratch; break;
            }
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
}

GUEST_HOOK(T_Com_Error)
{
    // The caller's address: the same message is raised from several places, and
    // which one gave up is half the answer.
#ifdef T_COM_ERROR_TREYARCH
    // Treyarch's is (file, line, code, format, ...).
    LOGE("Com_Error(%u) at %s:%u from %08X: %s", ctx.r5.u32, ctx.r3.u32 ? GuestAnsi(ctx.r3.u32).c_str() : "?",
         ctx.r4.u32, (uint32_t)ctx.lr, Format(ctx, ctx.r6.u32, 7).c_str());
#ifdef T_DATA_AssetPools
    // "Exceeded limit of N assets" is raised when a pool's list of free
    // entries is empty, not when its count reaches N: said with the count,
    // it tells a pool that really ran out from a list that lost its entries.
    if (ctx.r4.u32 == T_DB_POOL_EXHAUSTED_LINE)
        for (uint32_t type = 0; type < T_DB_ASSET_TYPES; type++)
        {
            const uint32_t pool = T_DATA_AssetPools + type * 24;
            if (PPC_LOAD_U8(pool + 12) == 0 && PPC_LOAD_U32(pool + 20) == 0 && PPC_LOAD_U32(pool + 8) != 0)
                LOGE("  asset pool %u: %u of %u entries in use, and no free entry", type, PPC_LOAD_U32(pool + 16),
                     PPC_LOAD_U32(pool + 8));
        }
#endif
#else
    LOGE("Com_Error(%u) from %08X: %s", ctx.r3.u32, (uint32_t)ctx.lr,
         Format(ctx, ctx.r4.u32).c_str());
#endif
    static const bool stack = diag::Flag("MW2_TRACE_ERRORS");
    if (stack)
    {
        platform::PrintBacktrace("the call stack that gave up");
    }
    GUEST_ORIG(T_Com_Error)(ctx, base);
}

#ifdef T_Lua_Error
// Advanced Warfare's menus are Lua (Havok Script, linked in), and the
// interpreter raises every error of a script or of its own library through
// one function, (state, format, ...), which leaves by longjmp. The engine
// shows none of them; a menu that stops answering usually said why here.
GUEST_HOOK(T_Lua_Error)
{
    LOGE("Lua error from %08X: %s", (uint32_t)ctx.lr, Format(ctx, ctx.r4.u32).c_str());
    GUEST_ORIG(T_Lua_Error)(ctx, base);
}
#endif

#ifdef T_Lua_Require
// Black Ops III's `require`, (state): the module's name is the first value on
// the state's stack (a value is a pointer and a type, 4 for a string, whose
// characters start 12 bytes into what the pointer names). The menus are made
// of hundreds of modules, each in some fastfile, and a menu that fails says
// only "attempt to index a nil value": MW2_TRACE_LUA shows which module was
// being asked for.
GUEST_HOOK(T_Lua_Require)
{
    static const bool traced = diag::Flag("MW2_TRACE_LUA");
    if (traced)
    {
        const uint32_t value = PPC_LOAD_U32(ctx.r3.u32 + 40);
        const bool string = value && (PPC_LOAD_U32(value + 4) & 0xF) == 4;
        LOGI("lua: require %s from %08X", string ? GuestAnsi(PPC_LOAD_U32(value) + 12).c_str() : "(not a string)", (uint32_t)ctx.lr);
    }
    GUEST_ORIG(T_Lua_Require)(ctx, base);
}
#endif

#if MW2_DIAGNOSTICS
#ifdef T_Com_Printf
// Com_Printf(channel, format, ...): viewpos answers through it, so it is the
// general print and not only the error one.
GUEST_HOOK(T_Com_Printf)
{
    static const bool verbose = diag::Flag("MW2_ENGINE_LOG");
    if (verbose) LOGK("Com_Printf(%u): %s", ctx.r3.u32, Format(ctx, ctx.r4.u32).c_str());
    GUEST_ORIG(T_Com_Printf)(ctx, base);
}
#endif

#if !defined(MW3_TITLE) // MW2-only prints and console hook

// The title's other prints, with the same arguments: its parsers report through
// these, and without them a playlist that fails says nothing.
#ifdef T_Com_PrintError
GUEST_HOOK(T_Com_DPrintf)
{
    static const bool verbose = diag::Flag("MW2_ENGINE_LOG");
    if (verbose) LOGK("Com_DPrintf(%u): %s", ctx.r3.u32, Format(ctx, ctx.r4.u32).c_str());
    GUEST_ORIG(T_Com_DPrintf)(ctx, base);
}

GUEST_HOOK(T_Com_PrintError)
{
    static const bool verbose = diag::Flag("MW2_ENGINE_LOG");
    if (verbose) LOGK("Com_PrintError(%u): %s", ctx.r3.u32, Format(ctx, ctx.r4.u32).c_str());
    GUEST_ORIG(T_Com_PrintError)(ctx, base);
}

GUEST_HOOK(T_Com_PrintWarning)
{
    static const bool verbose = diag::Flag("MW2_ENGINE_LOG");
    if (verbose) LOGK("Com_PrintWarning(%u): %s", ctx.r3.u32, Format(ctx, ctx.r4.u32).c_str());
    GUEST_ORIG(T_Com_PrintWarning)(ctx, base);
}
#endif

// Cbuf_AddText(localClientNum, text): what the menus and the title's own code
// queue for the console. MW2_TRACE_CBUF shows the commands a menu runs, so a
// flow the player clicks through can be typed instead. A title with bots has
// console lines of the runtime's own, and console.cpp's hook traces there.
#ifndef T_SV_BotUserMoves
GUEST_HOOK(T_Cbuf_AddText)
{
    static const bool traced = diag::Flag("MW2_TRACE_CBUF");
    if (traced) LOGI("cbuf(%u): %s", ctx.r3.u32, GuestAnsi(ctx.r4.u32).c_str());
    GUEST_ORIG(T_Cbuf_AddText)(ctx, base);
}
#endif
#endif

#endif

// DB_FindXAssetHeader gives up here when a lookup fails and the loader is not
// running: DB_MissingAsset(type, name).
#ifdef T_DB_MissingAsset
GUEST_HOOK(T_DB_MissingAsset)
{
    const char* name = ctx.r4.u32 ? reinterpret_cast<const char*>(base + ctx.r4.u32) : "(null)";
    const char* thread = crash::CurrentThreadName();
    LOGE("db: missing asset type %u '%s' on %s", ctx.r3.u32, name, thread ? thread : "?");
    GUEST_ORIG(T_DB_MissingAsset)(ctx, base);
}
#endif


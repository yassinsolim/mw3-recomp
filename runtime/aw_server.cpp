// How long a match this copy hosts waits for a player it has stopped hearing
// from, in Advanced Warfare's multiplayer.
//
// The title's server drops a player in a match after sv_timeout seconds of
// silence, 10 by default, and one that is still connecting after
// sv_connectTimeout, 40 (SV_CheckTimeouts, title.h). On the console a player's
// main thread goes on sending while a level loads. Here the first frames of a
// level translate and compile every shader the level draws with, on the thread
// that consumes the title's command stream, and the title's main thread waits
// for that thread: a copy joining a match was silent for 6 to 15 seconds on
// Windows, and past the tenth the host sent it away ("Server Disconnected"
// on the loading screen). Whether a join held was a matter of that one stall,
// on the local network as through a relay.
//
// So a host built from this source is as patient with a player in the match
// as the title already is with one connecting. What it costs is that a
// player whose copy died stands in the match that much longer.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest.h"
#include "log.h"

#ifdef T_SV_RegisterDvars

// (): registers the server's dvars, once, as the title starts.
GUEST_HOOK(T_SV_RegisterDvars)
{
    GUEST_ORIG(T_SV_RegisterDvars)(ctx, base);
    const uint32_t timeout = *GuestPtr<be32>(T_DATA_SvTimeoutDvar);
    const uint32_t connecting = *GuestPtr<be32>(T_DATA_SvConnectTimeoutDvar);
    if (!timeout || !connecting) return;
    be32& seconds = *GuestPtr<be32>(timeout + T_DVAR_INTEGER);
    const uint32_t patient = *GuestPtr<be32>(connecting + T_DVAR_INTEGER);
    if (seconds >= patient) return;
    LOGI("server: a player in a match this copy hosts may be silent for %u seconds, not %u", patient,
         uint32_t(seconds));
    seconds = patient;
}

#endif

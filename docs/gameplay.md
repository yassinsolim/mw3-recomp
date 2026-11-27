# Getting into a level

What the runtime provides between boot and a playable level. The multiplayer's
additional needs (network link, sessions) are in [multiplayer.md](multiplayer.md).

## Game data

The title reads its fastfiles from the game root (`mw2/game` for a developer
build; see [building.md](building.md)). File paths resolve case-insensitively
onto that one flat directory.

| files | status |
|---|---|
| `.ff` fastfiles, `.pak` image archives, `.bik` Bink movies | all of them are extracted, by `build.sh` from the ISO or by the launcher (`launcher/`) |

Every fastfile is needed, not only the current level's: a zone whose file is
missing makes the title report a dirty disc and fail, and the multiplayer picks
its own zones.

The multiplayer's fastfiles are signed, and it refuses one that is not with the
same dirty-disc report. With `MW2_CUSTOM_MAPS=1` (the launcher's MAPS → CUSTOM
MAPS), `runtime/unsigned_fastfiles.cpp` lets it load them, which is what a map
converted from the PC game is: its two files (`<map>.ff`, `<map>_load.ff`) in
the game root, and its menu picture (`<map>_preview.ff`). Its `<map>.arena` beside them, which the PC map comes with,
puts it in the menus (`runtime/custom_map_list.cpp`), for every map whose two
files are there:

- **The arena table**, where the lobby looks a map's name and picture up: the
  title reads only `mp/basemaps.arena`, and the hook parses the custom ones
  after it with the title's own parser, as the PC game reads the loose ones.
- **The map menus.** GAME SETUP's map choice is `settings_map` and its BONUS
  MAPS `settings_bonus_map`, a button each written out by hand in
  `patch_mp.ff`. The base menu is full; the bonus one gets a copy of its last
  map button per custom map, below it, with the map in its action, its label
  and the side panel's three variables. The copy drops the downloadable maps'
  colour expression, which greys one the console has not bought.
- **Their text.** The menus show a name through the `@` translation lookup,
  which marks one it has no translation for `UNLOCALIZED(...)`. The
  arena's longname and description translate to themselves
  (`SEH_StringEd_GetString`), only when the game has no translation of its own.
- **Their picture.** The side panel shows the material `preview_<map>`; a
  stock map's is in `ui_mp.ff`, a downloadable one's in `dlc1_ui_mp.ff` or
  `dlc2_ui_mp.ff`, which `Com_LoadUiFastFile` loads in one `DB_LoadXAssets`
  call. A converted map comes with its own zone, `<map>_preview.ff`, and the
  hook adds it to that call, with the menus' flags: a level's load frees it
  with them. The loader holds 32 zones; a picture past that is left out with
  a warning. A map without one shows `black`.

The launcher's MAPS screen adds (converting a PC map first, [maps.md](maps.md)),
removes and lists the same maps, and puts at most 8 in the game, the menu's room; a ninth there is left out with a warning in
the log (`runtime/install/arena.h`, `launcher/maps.h`). `map <map>` at the
console (`MW2_CONSOLE`) starts one directly.

A signed file is still checked. Nothing checks an unsigned one: the loader
takes its sizes, offsets and scripts as they come, which is why the switch is
off by default and the launcher warns before it turns it on.

## Movies

Bink is linked into the title (its own `BINK` section) and decodes on the CPU,
on two threads of its own per movie, and its sound reaches the output with the
rest of the title's. Nothing in the runtime is specific to it.

The campaign plays `IW_logo` and `legal` at boot, and a level's briefing while
the level loads: `video/cin_levels.txt` in `common.ff` names it, `<map>_load`.
When the load finishes, `UI_SetActiveMenu` opens the `pregame` menu, whose item
`press_to_skip` runs `uiScript playerstart` on A, and the level starts; it also
starts when the briefing ends. With `ui_autoContinue` set it starts as soon as
the load finishes, which a scripted run wants:

    MW2_CONSOLE="1:set ui_autoContinue 1;2:map trainer"

With a movie's file missing the title logs it, skips it, and the level starts
when it is loaded.

## Loading a level from the command line

`MW2_CONSOLE` queues commands into the engine's own console command buffer,
which is the shortest path to a level:

    MW2_CONSOLE="30:map trainer"            # one command at 30 s
    MW2_CONSOLE="30:map af_caves;45:god"    # several, separated by ';'

The time is wall-clock seconds since the runtime started (the clock
`MW2_INPUT_SCRIPT` uses). Only `;` separates entries and only the first `:`
separates the time, so a command may contain spaces. It is kept in release
builds.

Call of Duty 4's multiplayer takes `map` only once Start was pressed on its
title screen. Before that no controller owns the game, the local client is
never connected to the server `map` starts, and its first packet has
nowhere to go: `EXE_LOSTCONNECTION` ("Network connection lost") two seconds
after the map starts loading, with any online service or none. Press Start
first:

    MW2_INPUT_SCRIPT="18:start"
    MW2_CONSOLE="30:set onlinegame 0;30.5:set systemlink 1;33:map mp_crash"

It is the title's own order of things (the menus sign the controller in
the same way), not something the runtime decides; Modern Warfare 2's and
3's multiplayer take `map` from their title screens.

`runtime/console.cpp` calls the title's `Cbuf_AddText` (`T_Cbuf_AddText` in
[`runtime/title.h`](../runtime/title.h)) from `XamInputGetState`, which the title
polls once a frame on its main thread, so a live guest context is always
available. `Cbuf_AddText` copies its text, so the string only has to outlive the
call: it is written 256 bytes below the guest stack pointer, and the call is
made with the stack pointer moved further down so the callee's frame cannot
overlap it. The whole guest context is saved and restored around the call,
because the guest is in the middle of a kernel import. One command is queued per
poll; a command longer than 192 bytes is refused.

Level names are the fastfile names: `trainer` is S.S.D.D., the first campaign
mission; `af_caves` is "Just Like Old Times". In the campaign, `setviewpos x y
z yaw pitch` places the player (`F9` logs the current `viewpos`); the
multiplayer refuses it ([switches.md](switches.md)).

## Controllers

`runtime/kernel/input.cpp` backs `XamInputGetState`, `XamInputGetCapabilities`
and `XamInputSetState` with SDL3 gamepads; controller *n* is SDL's *n*-th
gamepad, looked for again once a second while absent. Capabilities report a
wired 360 pad: every control at full range and both motors at `0xFFFF`.

`XamInputSetState(user, flags, vibration)` takes the motor speeds from the third
argument (`flags` is unused). The left motor is the heavy low-frequency one,
SDL's first rumble argument. A 360 motor keeps its speed until the next call, so
each rumble is given SDL's longest duration (`0xFFFF` ms); a duration of zero
would also stop SDL resending the rumble to pads that let one lapse. The log
names the first rumble per controller, or that the pad cannot rumble.

The title sends zero speeds unless the profile's rumble option is on (byte 67
of its per-controller profile record, on by default) and the local player is in
play: while spectating (`pm_type` 5) or following another player it clears every
rumble each frame. A headless test has to spawn first (`MW2_WALK_PATH`) and then
fire (`MW2_INPUT_SCRIPT`).

## Signing in

Every player is a profile of this machine: a line of `saves/profiles.txt`,
twelve hexadecimal digits -- the number the title knows him by -- and a name
(`runtime/signin.cpp`).

The first controller's profile is signed in from the start. It is made the
first time the game runs, named after the login (or `MW2_NAME`), and is the
same every run: the file's `first` line names it, and the launcher's PROFILES
screen puts another there. The online service is not who the player is. Steam
gives him the name other players see and brings his friends; the lan gives
him nothing but the network. His rank is the profile's, in files on this
machine ([saves.md](saves.md)), whichever service he plays through.

The file's `machine` line is a hash of the machine's own id (`MachineGuid`,
`/etc/machine-id`). Read on another machine, where a copied game folder ends
up, every profile gets a new number and what was kept under the old one is
renamed (`kernel::MovePlayerData`): two machines playing as one player is two
of the same player in a match, and the title drops the host when a player it
already has connects again.

Nobody is at the other controllers until the sign-in screen puts someone
there, and that is not kept between runs.
It is the console's screen, which a title asks for with `XamShowSigninUI` and
never draws: the multiplayer asks from SIGN IN PROFILE and CHANGE PROFILE in
split screen, and when a controller nobody is signed in at chooses SYSTEM
LINK. The Guide button at the second to fourth controllers opens it too, as on
the console, or Back and Start together where the system keeps that button.

At the first controller the Guide button (or the keyboard's Home) opens the
guide (`guide.h`): the player's name, the time, what the title says they are
doing, the network and the friends on it, then Resume, Invite friends and Quit
to desktop, which asks once more. "What the title says" is its rich presence,
as the console's guide showed it ("Playing Team Deathmatch in Afghan"): the
title sets contexts with `XUserSetContext`, and the strings for each are in its
XLAST, gzipped in the SPA resource of its image (`presence.h`). The system's
screens are drawn over a blurred and darkened copy of the game
(`gpu/vulkan/backdrop.h`).

The console's screen also signs a guest in beside a player on Live. This one
does not: the title's split screen is offline only, and its system link takes
one player a copy.

It lists the profiles of this machine that are not already playing, and "New
profile", which makes "Player 2" or the next free number. A profile is signed in
locally (state 1) under the offline XUID `0xE000` over those digits; the title
names its stats by that XUID and its settings are `profile_<digits>.bin`
([saves.md](saves.md)). The launcher's PROFILES screen renames one, and puts one
at the first controller.

The screen opens for the controller pressed last, since the call does not say
which. While it is open the title reads every controller as idle and hears
`XN_SYS_UI`; a choice is `XN_SYS_SIGNINCHANGED` with a bit per signed-in
controller, on which the title reads the new profile, stopping for a second.

The picture is drawn on the CPU with the launcher's fonts and laid over the
middle of the frame by the presenter, as a copy: it is opaque.
`MW2_DUMP_FRAMES` writes each state of it as `signin_<n>.ppm`.

## The save-device prompt

The campaign asks which storage device to save to until each controller has
one. `Memcard_InitializeSystem` keeps the choice in a table of one word per
controller, and zero means "not chosen". On the console the choice lives in the
profile and is made once; here the table starts empty every launch. The runtime
hooks `Memcard_InitializeSystem` (`runtime/kernel/content.cpp`) and, after it
returns, writes the hard disk's device id into every empty slot, so the prompt
does not appear. `MW2_NO_AUTO_SAVE_DEVICE=1` leaves the prompt to the title.
The table's address is known only for the campaign (`T_DATA_DeviceTable` is zero
in the multiplayer build). Saves themselves are in [saves.md](saves.md).

## What the level load relies on

These kernel and translation details are not visible in the menus, and the
first level load fails without them.

**Processor numbers.** The title pins its threads with `KeSetAffinityThread` (and
the top byte of the thread creation flags), a one-bit mask over the console's
six hardware threads. The runtime stores the chosen processor in
`KPCR.CurrentProcessorNumber` (offset 268), because the engine reads it: the
renderer's command-buffer jobs are admitted only on processors 1 and 2, whose
per-processor contexts they write into, and XAudio2's mixer threads wait on each
other by processor number. With every thread reporting 0, the first frame of a
level never finishes. Threads pinned to one processor run concurrently here,
which the console never does, so per-processor data is a place to look when
something races.

**Two windows onto physical memory.** D3D9 writes its recorded command chunks
through `0xA0000000` and reads them back through `0xC0000000`; both windows are
mapped over the same pages ([runtime.md](runtime.md)).

**Packed vertex formats.** Level geometry is packed and unpacked on the CPU
(skinning among others) with the VMX128 `vpkd3d128`/`vupkd3d128` instructions,
in formats upstream XenonRecomp does not implement.
`patches/xenonrecomp-mw2.patch` adds type 2 -- three 10-bit signed normalised
components with `w` in the top two bits, laid out like D3DCOLOR; like that case
the instruction only moves each field into the mantissa of `3.0f`, and the
caller's multiply-add by `(511 * 2^-22, -3.0)` finishes it, which is why full
scale is 511 -- and the `float16_2` pack.

## Field of view

`MW2_FOV` (`runtime/field_of_view.cpp`) widens the view. The number is counted
as the title's own `cg_fov` is: the angle across a 4:3 picture, 65 by default.
The title makes the 16:9 picture as tall as the 4:3 one and wider (the angle's
tangent times 0.75 is the half height, and that times the picture's shape the
half width), so 65 is 81 degrees across the screen, 90 is 106 and 120 is 133.
A 16:10 picture (`MW2_RENDER`, [rendering.md](rendering.md#the-titles-frame-and-the-pictures-shape))
is as tall again and narrower: 65 is 75 degrees across it.

Neither of the title's two settings can carry the player's choice, because its
scripts write both. `cg_fov` is set to 65 at every multiplayer spawn
(`_playerlogic.gsc`) and walked through other angles by the campaign's climbs
and cinematics (`_climb.gsc`, `lerp_fov_overtime` in `_utility.gsc`);
`cg_fovScale` is written at every level load and every multiplayer connect,
0.75 in split screen and 1 otherwise (`_load.gsc`, `_playerlogic.gsc`), and
walked by `lerp_fovscale_overtime`. A value put in either lasts until the next
script that writes it.

So the settings are left alone, and the title is made to see `cg_fovScale` as
what its scripts last wrote times `MW2_FOV / 65`, in the two functions that
read it:

| | campaign | multiplayer | |
|---|---|---|---|
| `T_CG_ViewFov` | `8210FCD8` | `8215B9A8` | the angle a client's view is drawn with: `cg_fov`, or the weapon's while aiming (blended in as the sight comes up), or a turret's or a kill camera's; times `cg_fovScale`; held between `cg_fovMin` and 170 |
| `T_CG_CullZoom` | `8210FF80` | `8215BC28` | the zoom the distance culls take from that view, divided by `cg_fovScale` so the scale does not bring them nearer |

Each hook scales the function's result: the first multiplies it and holds it
under the title's 170 again, the second divides it. That is the result the
function would have had with the scale in the setting, except for a view the
title was already holding at one of its bounds, which is scaled from the bound.

Modern Warfare 3's multiplayer has the same two functions at `8214C5A0` and
`8214C8B0` (a leaf), and reads the switch as well; its campaign's are not
located. The Android app's field of view slider sets `MW2_FOV` there, since
the `cg_fov` it sends the other titles is put back to 65 at the next spawn.
That executable also turns by the view's angle, in two places that compare it
with the hip view the scripts know of: the pad's turn rate is multiplied by
tan(half the view) over tan(half of 65 times `cg_fovScale`), which the aim
assist keeps (`820D31A0`, at its state `+280`), and the input's sensitivity
scale is the same ratio against a fixed 65 (`8214C7B0` stores it at cg
`+0x76580`). Both hooks divide by what the hip's ratio became, so the hip
turns as on the console and a sight slows by its zoom against the wider view.
Measured with `MW2_TRACE_MATCH=1`, the right stick held 0.4 s three times: 83
degrees a press from the hip at 65 and 80 at 100; 26 down a red dot sight at
65 and 23 at 100.

Advanced Warfare reads the switch in its campaign and in both builds of its
multiplayer, and the Android app's slider sets it in the multiplayer. In
the campaign the app still sends `cg_fov`: with `MW2_FOV=85` the visual
suite's Induction failed at the squad mate's prompt on both phones in every
attempt, where it passes with `cg_fov`, and why is not known yet; the
switch itself was seen wider at 110 on Windows and through two restarts
from a checkpoint on a Retroid Pocket Flip 2. Until 26
November 2026 the app sent `cg_fov`, which this title's scripts put back to
65 at every spawn as Modern Warfare 3's do: a tester's view of 110 was 65
again after each death. The four functions, found from the pointer each
executable keeps to `cg_fovScale`:

| | campaign | multiplayer (disc) | multiplayer (title update 17) |
|---|---|---|---|
| `T_CG_ViewFov` | `822199C8`, not hooked | `82630250` | `825DD8B8` |
| `T_CG_CullZoom` | `822174B8` | `8262DEF8` | `825DB658` |
| `T_CG_SetViewFov` | `82216E88`: the view's two tangents, cg `+0x27010` and `+0x27014` | `8262E608`: the sensitivity scale, cg `+0x44914` | `825DBDD8`: the sensitivity scale, cg `+0x43114` |
| `T_AimAssist_FovScale` (the turn rate's scale) | `82106710`, state `+276` | `827D0A18`, state `+260` | `827734F0`, state `+260` |

The multiplayer multiplies `cg_fovScale` by a scale of the scripts' for each
client (the omnvar `fov_scale`, `sub_8262DE80`) wherever it reads it, so the
aim assist's hook does not read the setting: it takes the hip's half angle
back out of the scale the title has just stored. Seen on Windows in the
update's multiplayer (Drift, ten bots): at `MW2_FOV=110` the view is as wide
after a death and a respawn as before it, where `cg_fov 110` is back to 65.
The turn rate's scale is 1.0 from the hip at 65 and at 110, and the
sensitivity scale 1.0 from the hip at both and, down the ASM1's red dot
sight, 0.57 at 65 and 0.47 at 110.

The campaign's angle is left the title's. Its `CG_SetViewFov` writes the
angle it is handed back into `cg_fov` and into the view a script's change
over a time starts from, so a scaled angle was scaled again the next frame:
the drop into Seoul, whose script walks the view, was drawn at the title's
bound of 170 degrees. The hook scales the two tangents of half the view that
the function has just kept instead, which is what the view and the weapon
are drawn with; the sensitivity scale is then the console's untouched. The
setting is not a script's to write, so a checkpoint, a death and the next
level keep it. Seen on Windows in Induction at 65 and at 110
(`private-captures\awfeedback4\pc-fov-campaign-seoul-65-vs-110.jpg`).

Everything the title draws is scaled by the same factor, as its own
`cg_fovScale` does: a sight zooms in less at a wider view, and a scripted zoom
or split screen's narrower view keeps its proportion. The weapon in the
player's hands is drawn with the view's angle as well: at 120 it is smaller
and longer, and more of the arms shows. A wider view has more in it: the S.S.D.D. flight spends
14.9 ms a frame in the renderer at 90 where it spends 13.2 at 65.

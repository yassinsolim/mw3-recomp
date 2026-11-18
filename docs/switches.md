# Switches

The runtime is configured through `MW2_*` environment variables. A flag is on
when set to anything but empty or `0` (`runtime/env.h`).

## The settings file

A file named `.env` beside the executables (in the current directory for a
development run) holds switches as `NAME=value`, one a line. The runtime reads
it first thing (`runtime/settings.cpp`) and sets each `MW2_` switch the
environment does not already have, so a variable given on the command line
decides. The launcher writes its own settings there (FULLSCREEN is
`MW2_FULLSCREEN`, ASPECT RATIO and RESOLUTION are `MW2_RENDER` and `MW2_SCALE`, STRETCH is `MW2_STRETCH`, ANTIALIASING is `MW2_MSAA` or
`MW2_NO_MSAA`, ANISOTROPY is `MW2_ANISOTROPY`, SHADOWS is `MW2_SHADOW_SCALE`, FPS LIMIT is `MW2_FPS_LIMIT`, FOV is `MW2_FOV`, CUSTOM MAPS is `MW2_CUSTOM_MAPS`) and keeps the other lines. When the
file has no `MW2_FPS_LIMIT`, the launcher writes the screen's refresh rate as
it opens (60 for a 60 Hz screen, which is the console's pacing). `MW2_LAUNCHER_SOUNDS=0`, written there
by hand, keeps the launcher's own menus silent. `MW2_PC_GAME=<folder>` is the PC game's folder, whose
pictures and sounds ADD MAP gives a converted map (MAPS → PC GAME chooses it). When `.env` has no such line,
the launcher looks for Steam's copy once a run and writes the line when it finds one; an empty
`MW2_PC_GAME=` says there is none. The environment's value decides over the file's
([maps.md](maps.md#inputs)).

Switches marked **R** are read by every build. The rest are diagnostic: a build
without diagnostics (`RELEASE=1 ./build.sh`, `-DMW2_DIAGNOSTICS=OFF`) treats
them as unset whatever the environment says, and also drops the keys `F5`, `F7`,
`F9`, `F10` and `F11`, `kill -USR2` and the end-of-run reports.

## Running

| switch | | effect |
|---|---|---|
| `MW2_WINDOW=0\|1` | R | the window, which every build opens; `MW2_WINDOW=0` keeps it shut, for a headless run. Closing it ends the run as `MW2_RUN_SECONDS` does, with the reports. The frame keeps its proportions in any window shape |
| `MW2_FULLSCREEN=1` | R | starts fullscreen, borderless at the desktop's resolution. `F8` toggles it |
| `MW2_STRETCH=1` | R | the frame fills the window or screen whatever their shapes, instead of keeping its proportions between black bars: for a TV that shows a widescreen signal at 4:3, with a 4:3 frame (`MW2_RENDER`) |
| `MW2_MOUSE=0` | R | Windows: the window does not hold the mouse and the game does not read it. Without it the window holds the mouse while it has the keyboard -- the cursor hidden and kept inside, the mouse the first player's right stick and triggers -- and `F2` or leaving the window (`Alt+Tab`) lets it go; `F2` again, a click in the window or coming back takes it again ([runtime.md](runtime.md#input)) |
| `MW2_MOUSE_SENSITIVITY=<n>` | R | Windows: how far the mouse turns the view, as the PC game's `sensitivity` (0.022 degrees a count each). Default 5. The title's own Sensitivity option multiplies it, and decides how fast the view turns at most |
| `MW2_LOG_FILE=<path>` | R | writes the log there, creating its folder. The log goes to stderr, so `> file` does not catch it. A title started from the other one's menus writes on in the same file |
| `MW2_REPORT=1` | R | what a bug report needs, as the launcher's REPORT A BUG sets it: the log names the graphics driver and ends with the run's frame rate, frame time spread, what was compiled at a draw, how much of the time the graphics chip was at work by its own clock (a timestamp pair around every submission), and the processor and memory in play: how busy, how much of it this game's and which threads, how much memory was free, what the memory watch's protection changes cost ([building.md](building.md)) |
| `MW2_LOG_TIME=1` | | stamps every log line with the time since start |
| `MW2_LOG_ALL=1` | | writes every line as it comes. Without it a line that repeats is written once and then counted -- the 1st, 2nd, 4th, 8th ... with the size of the repetition beside it, which is never above the truth and within a factor of two of it -- and two lines that differ only in an address, a size or a shader hash are one kind of line. A run that logs one thing a thousand times buries the line that says what it was |
| `MW2_CONSOLE="30:map af_caves;45:god"` | R | queues commands into the title's console command buffer at the given wall-clock seconds; see [gameplay.md](gameplay.md) |
| `MW2_CUSTOM_MAPS=1` | R | the multiplayer loads fastfiles nobody signed: a map converted from the PC game. Off, it refuses them as the console does, and the launcher's shader pass reads only the game's own fastfiles ([gameplay.md](gameplay.md)) |
| `MW2_UNLOCK_ALL=1` | R | Modern Warfare 3's multiplayer only: every weapon, attachment, camouflage, reticle, proficiency, perk, killstreak, title and emblem reads as unlocked. The title's own unlock checks are answered, and nothing is written to the stats, so without it the menus show what was earned again. The Android app's "Unlock all camos and items" sets it and is on by default; unset on the desktop ([MW3-MP-BRINGUP.md](MW3-MP-BRINGUP.md#everything-unlocked)) |
| `MW2_BOTS=<0-17>` | R | Modern Warfare 3's multiplayer only: that many bots join every match this copy hosts and leave when a player needs the slot; nothing happens in a match somebody else hosts. They pick a team and a class through the stock menus, shoot at enemies they can see and walk towards the nearest one, feeling their way round walls (the maps have no path data). The Android app's "Bots in my matches" sets it. In a match the host's console takes `bots <n>`, `bots +`, `bots -` and `bots skill <0-3>` (through `MW2_CONSOLE` too) (`runtime/bots.cpp`) |
| `MW2_BOT_SKILL=<0-3>` | R | how well the bots shoot: 0 Recruit, 1 Regular (the default), 2 Hardened, 3 Veteran. It sets their reaction time (600 to 120 ms), aim error (5° to 0.5°), turn rate, view cone, and whether they strafe and crouch in a fight |
| `MW2_BOT_TEAMS=against` | R | every bot joins the side the players are not on. Unset, they are auto-assigned and fill both teams |
| `MW2_NET_LINK=0\|1` | R | whether the Ethernet link is reported up. Default down for the campaign, up for MW2's multiplayer, which starts no match without one, and for MW3's multiplayer up when an online service started |
| `MW2_NO_AUTO_SAVE_DEVICE=1` | R | leaves the save-device choice to the title's prompt instead of choosing the hard disk ([saves.md](saves.md)) |
| `MW2_NO_AUDIO=1` | R | opens no playback device; the mixer and decoder still run ([audio.md](audio.md)) |
| `MW2_RUN_SECONDS=<n>` | | reports and exits after n seconds |
| `MW2_WATCHDOG=<seconds>` | | when it fires: the `MW2_DUMP` blocks, what every guest thread is blocked on (handle and caller) or spinning on, and every thread's backtrace, which is the guest call stack; then exits |
| `kill -USR2 <pid>` | | every thread's stack, as the watchdog prints them, and the run continues (Linux). Sent a few times, it tells a stuck thread from a looping one |
| `MW2_INPUT_SCRIPT="20:start,26:a"` | | presses pad inputs at wall-clock seconds. An entry names a button (`a b x y lb rb start back up down left right lthumb rthumb`), a trigger (`lt`, `rt`) or a stick direction (`lx+ lx- ly+ ly- rx+ rx- ry+ ry-`), with an optional hold time: `"40:lt:25"` holds the left trigger 25 s. Default hold 0.2 s. A name ending in `@2`, `@3` or `@4` is that player's controller, which the script then stands in for: `"5:a@2"`. `guide` is the Guide button, which opens the sign-in screen |

## Online

Read by every build; see [multiplayer.md](multiplayer.md).

| switch | effect |
|---|---|
| `MW2_NETWORK=none\|lan\|server\|steam` | the online service; `none` (default) keeps system link on this machine, `steam` is lan when Steam is not running. The launcher's NETWORK entry sets it, and the Android multiplayer apps' Settings. MW3's multiplayer has none, lan and server |
| `MW2_LIVE=0\|1` | MW3's multiplayer: signed in to Xbox LIVE through the service. On by default with `server`, where the Xbox LIVE menu offers the relay's playlists and Find Game, and the title's DemonWare lobby is answered inside the game (`runtime/online/demonware.h`); off by default with `lan`. System Link works either way |
| `MW2_SERVER=<host>[:<port>]` | server: the relay's address (port 3074 when none is given) |
| `MW2_SERVER_PASSWORD=<text>` | server: the password the relay asks for |
| `MW2_NAME=<name>` | lan and server: the player's name, instead of his profile's; the name of the first profile when it is made |
| `MW2_LAN_PORT=<port>` | lan: the shared broadcast port (3074); lan and server: each copy's own port is the first free one above it |
| `MW2_LAN_JOIN=<name>` | lan and server: joins that player's party or lobby when it is seen |
| `MW2_LAN_ACCEPT=1` | lan and server: accepts invitations |
| `MW2_STEAM_APPID=<id>` | steam: the app id to run as (480, Spacewar) |
| `MW2_STEAM_ROOT=<path>` | steam: the Steam install to use, when it is not found or several are running |
| `MW2_VOICE=1` | MW3's multiplayer: voice chat. Off by default; on, the microphone is opened and what the player says goes to the other players through the service, unencrypted ([multiplayer.md](multiplayer.md#voice-chat)). The Android app's "Voice chat" sets it |
| `MW2_VOICE_PTT=0\|1` | voice: send only while the push-to-talk key (T) or the on-screen Talk button is held. Default 1; 0 is an open microphone, as the console had |
| `MW2_VOICE_VOLUME=<percent>` | voice: how loud the other players are, in percent of what the title sent (100; at most 400) |
| `MW2_VOICE_TONE=1` | voice: a 440 Hz tone stands in for the microphone, which is then not opened; still gated by push-to-talk (diagnostic) |
| `MW2_VOICE_DUMP=<file.wav>` | voice: writes what the other players said, as decoded, to a 16 kHz mono WAV file (diagnostic) |
| `MW2_TRACE_ONLINE=1` | every datagram through the service (diagnostic) |
| `MW2_TRACE_NET=1` | the same in every build, with the session messages, the sockets' ports, what the service drops, and a line every five seconds while the title's main thread runs; `net-trace:` lines |
| `MW2_TRACE_DW=1` | MW3's multiplayer: a line for the logon to the DemonWare lobby stand-in and for every task the title sends it, with the service, the task number, the file it names, the error and the size of the reply; `dw:` lines, in every build |

## Moving the player

Diagnostic. The multiplayer refuses `setviewpos`, so a spot is reached by
walking to it.

| switch | effect |
|---|---|
| `MW2_WALK_TO="x,y[,radius[,yaw]]"` | walks to a point and holds there, facing the yaw given or turning slowly; reports the closest approach and how often it got itself unstuck |
| `MW2_WALK_PATH=<file>[,<file>...]` | walks a recorded route (`x y z yaw pitch` per line), looking where the player looked; with several, the one starting nearest the spawn. A line `left x y z yaw pitch` or `fire x y z yaw pitch` goes to the spot, turns to the aim, and presses d-pad left or the right trigger there |
| `MW2_TEAM_UP=1\|2` | on the team selection, taps Up that many times before choosing, instead of Auto-assign, so a run spawns on the same side every time |
| `MW2_WALK_REACH=<units>` | how near a route's point the walk has to come before heading for the next: 24 for a point recorded with its view, 140 for the older routes that stop at yaw |
| `MW2_WALK_PAUSE=<seconds>` | stands still at every corner of the route |
| `MW2_QUIT_AFTER_ARRIVAL=<seconds>` | ends the run that long after the walk arrives, or after it stops getting nearer for `MW2_QUIT_IF_STUCK` seconds (45) |
| `MW2_RECORD_FLIGHT=<file>` | writes where the camera is and when, twenty times a second, as a flight for `MW2_FLY_PATH`; noclip and all |
| `MW2_FLY_PATH=<file>` | replays a recorded flight by placing the camera with `setviewpos`, at the same moments counted from the first frame with a player; campaign only, and `MW2_QUIT_AFTER_ARRIVAL` counts from its end |
| `MW2_RECORD_PATH=<file>` | writes where the player goes as a route for `MW2_WALK_PATH`, a point every `MW2_RECORD_SPACING` units (110), with d-pad left and right-trigger presses and their aim |
| `MW2_F5=<command>` | with the window open, `F5` runs that console command: `MW2_F5=noclip` after `devmap`, to fly once a level's opening is behind |
| `MW2_TRACE_VIEWPOS=1` | logs where the player stands once a second |
| `F9` | with the window open and `MW2_ENGINE_LOG=1`: runs the title's `viewpos` and logs the position, whose five numbers `setviewpos x y z yaw pitch` takes in single player |

## The guest

| switch | effect |
|---|---|
| `MW2_TRACE_INDIRECT` | CMake option, on by default: reports an indirect call with no recompiled target instead of jumping to null (one outside the code section is reported in every build) |
| `MW2_ENGINE_LOG=1` | the title's own `Com_Printf` output in the log |
| `MW2_TRACE_ERRORS=1` | the guest call stack under every `Com_Error`; one wrapper raises every error, so the message alone does not say where it came from |
| `MW2_TRACE_CBUF=1` | every command queued to the title's console, which shows what a menu runs |
| `MW2_TRACE_BOTS=1` | the bots: the menus' names, each menu answer, where every bot is and what it is doing every five seconds, and each target that went down while a bot fired at it |
| `MW2_TRACE_FILES=1` | every file opened, read and written. The files asked for and not found are listed at exit regardless |
| `MW2_TRACE_XMA=1` | every XMA decoder kick with its buffers, and FFmpeg's log |
| `MW2_DUMP=<hex>[:words][,...]` | blocks of guest memory to print when the watchdog fires (16 words by default) |
| `MW2_WATCH=<hex>[:bytes]` | a write watchpoint (Linux): the page is made read-only, and every guest write into the range is reported with its call stack and thread, up to `MW2_WATCH_REPORTS` (20); the command processor's writes there are reported too. It logs when it gives up on a busy page |

## The command stream

| switch | effect |
|---|---|
| `MW2_DUMP_SHADERS=<dir>` | every distinct shader's microcode, for `tools/xenos_shader.py` and `translate-shader` |
| `MW2_DUMP_TEXTURES=<dir>` | read by every build (see Drawing). With diagnostics, also: every distinct texture shape bound at a draw, each further mip level as `<key>_level<n>.png`, its fetch constant logged. `MW2_DUMP_TEXTURES_RAW=1` also writes the guest bytes; `MW2_DUMP_TEXTURES_AFTER=<seconds>` starts later, since a streamed texture is empty at its first bind |
| end-of-run reports | packets by opcode, frame times, draws and presents per second, arena copies, draws per render target, texture and shader work |

## Drawing

| switch | | effect |
|---|---|---|
| `MW2_SCALE=<2 or 3>` | R | draws every surface that many times wider and taller than the title's: a 2560x1440 or 3840x2160 frame from the console's 1280x720. The launcher's RESOLUTION entry sets it in the settings file ([rendering.md](rendering.md#resolution-scale)) |
| `MW2_RENDER=<w>x<h>` | R | the frame the title draws, in place of the console's 1280x720, and so the picture's shape: 960x600 is 16:10, 640x480 4:3. 640x360 to 1280x800, a whole number of 16 pixels wide and 4 tall. The launcher's ASPECT RATIO and RESOLUTION entries set it in the settings file, with `MW2_SCALE` ([rendering.md](rendering.md#the-titles-frame-and-the-pictures-shape)) |
| `MW2_FPS_LIMIT=<n>` | R | how many frames a second the title draws at most. Unset or 60 is the console's pacing, a frame at every blank. Any other number takes the title off the blank, with that number as its own limiter's; 0 is no limit. The limiter counts whole milliseconds a frame, so 120 gives 125 and 144 gives 166. The launcher's FPS LIMIT entry sets it in the settings file ([rendering.md](rendering.md#more-than-60-frames-a-second)) |
| `MW2_FOV=<degrees>` | R | the field of view, 65 (the console's) to 120, counted as the title's `cg_fov` is: the width of a 4:3 picture, so 65 is 81 degrees across the 16:9 one and 90 is 106. It scales every view the title draws, a weapon's sight included, and leaves the title's own settings to its scripts. The launcher's FOV entry sets it in the settings file ([gameplay.md](gameplay.md#field-of-view)) |
| `MW2_MSAA=<n>` | R | draws every surface the title multisamples at n samples, rounded down to what the device offers |
| `MW2_NO_MSAA=1` | R | draws the title's 2x and 4x surfaces at one sample |
| `MW2_SHADOW_SCALE=<2 to 4>` | R | draws the sun's and the spots' shadow maps that many times wider and taller than the title's 1024x2048 and 512x2048, and never smaller than `MW2_SCALE` draws them. The launcher's SHADOWS entry sets it in the settings file ([rendering.md](rendering.md#the-shadow-maps-scale)) |
| `MW2_ANISOTROPY=<2, 4, 8 or 16>` | R | the anisotropic filtering every mipmapped texture gets at least: the title's `r_texFilterAnisoMin`, the PC game's setting, registered with it as its default. Unset or 1 is the console's, 1:1 to 4:1 as each material asks. The launcher's ANISOTROPY entry sets it in the settings file ([rendering.md](rendering.md#anisotropic-filtering)) |
| `MW2_ARENA_MB=<n>` | R | the upload arena for constants, vertices and indices, shared by the frame slots (512) |
| `MW2_TEXTURE_BUDGET_MB=<n>` | R | the texture cache's budget, past which it lets go of what has not been bound lately, even textures whose memory is unchanged (defaults to half the device-local memory within 256 MB to 2 GB on desktop, and 128 MB on Android) |
| `MW2_TEXTURE_REPLACE=<dir>` | R | the folder of texture replacements, in place of `replace` in the game folder (used when it exists): `<key>.png` or `<key>.pnm` there, or in any folder below it, is uploaded in place of the texture whose content has that key. Frames `<key>.0.png`, `<key>.1.png`, ... (or numbered pictures in a folder named `<key>`) are an animation, shown by the clock at the `fps=<n>` of `<key>.txt`, or 12. The log says how many were found and how many are animated, names each as it is read or refused, and counts them and the animation frames staged at the end of the run ([textures.md](textures.md#texture-replacement-skins)) |
| `MW2_DUMP_TEXTURES=<dir>` | R | writes every texture the cache reads as `<key>.png` in that folder (made if it is not there), once a key, with a line in `textures.txt` giving its format, size and levels: the pictures a replacement is made from, and the names it must have. The files are not compressed |
| `MW2_PIPELINE_CACHE=<file>` | R | keeps the driver's compiled pipelines across runs, in place of a player's `shaders/pipelines.bin` |
| `MW2_SHADER_CACHE=<file>` | R | records every pipeline a run needed and builds them at the next start-up |
| `MW2_NO_PIPELINE_LIBRARIES=1` | R | builds each pipeline whole at its first draw instead of linking it from shaders compiled at load ([rendering.md](rendering.md)) |
| `MW2_ASYNC_PIPELINES=1` | R | a thread of its own (normal priority, a performance core) compiles a pipeline never built before, and its draws are skipped until it is there, so what they draw appears late; unset or 0 compiles it at its first draw, which holds the frame up. Never used for a pair linked from libraries. The Android app's "Compile shaders in the background" sets it and is on by default; unset on the desktop. On the AYN Thor, a first Survival play with empty caches: 25 frames lost instead of 146, the longest wait 785 frames, the map whole by 30 s ([rendering.md](rendering.md#pipelines)) |
| `MW2_TRACE_SHADER_LOADS=1` | | each shader handed over at load (a pixel shader as it is made, a vertex shader once D3D binds it: "made" and "bound"), each distinct shader the command stream loads (an `IM_LOAD_IMMEDIATE` at address 0), and each the renderer builds at a draw ("shader drawn"), with its hash |
| `MW2_SHADERS_COMPILED_FOR=<id>` | R | set by the launcher on a game it starts: the device the shaders were compiled for. A player's start that still finds them not fitting on that device says so and plays on, instead of starting the launcher again ([rendering.md](rendering.md#every-shader-compiled-before-play)) |
| `MW2_TRACE_DRAWS=<n>` | | describes n draw packets in full; `MW2_TRACE_DRAWS_AFTER=<presents>` starts later |
| `MW2_TRACE_FRAME=<n>` | | narrates n frames' render targets and resolves |
| `MW2_TRACE_RENDER_AFTER=<frames>` | | where the traces below start, unless the walk arrives first |
| `MW2_TRACE_PASSES=<n>` | | a frame's passes in order: target, colour mask, depth state, scissor |
| `MW2_TRACE_SHADER_VERTS=<hex>[,...]` | | for draws with those shaders: the first float constants, the fetch constants and the vertices they name |
| `MW2_RENDER=0` | | executes and counts the stream but draws nothing |
| `MW2_ONLY_SHADER=<hex>[,...]`, `MW2_SKIP_SHADER=<hex>[,...]` | | draws only, or all but, the draws using those shaders |
| `MW2_NO_CULL=1`, `MW2_NO_DEPTH_TEST=1` | | take culling or the depth test out of every pipeline |
| `MW2_NO_SHADOW=1` | | no shadow of guest memory: every draw copies its vertex data into the arena. It is 512 MB behind the arena, so Android sets it itself on a device of six gigabytes or less ([android.md](android.md)) |
| `MW2_ADRENO_BC=hw\|dxt1` | | how much of the Snapdragon workaround applies when the driver's own BC sampling is on: `hw` keeps it entirely, `dxt1` keeps it and decodes only the BC1 family in software. Unset is every BC format in software, which is the default on Adreno ([textures.md](textures.md)) |
| `MW2_FORCE_BC_HW=1` | | on an Adreno device: leave BC sampling to the driver rather than the software decoder |
| `MW2_FORCE_BC_DECOMPRESS=1` | | decode every BC texture in software on any device, which is how a desktop driver is made to exercise the decoder the Snapdragon path uses |
| `MW2_RECORD_THREAD=0` | | makes the Vulkan calls on the ring consumer instead of the recorder thread |
| `MW2_TEXTURE_THREADS=<n>` | R | how many threads besides the ring consumer read, byte-swap and software-decode the textures a segment's end reads again because the title rewrote their memory (3, at most 8). The consumer hands them over, takes its share and waits for all of them, then stages each in the order it would have; a single texture stays on the consumer. `0` reads them one after another on the consumer, as before |
| `MW2_NO_MERGE=1` | R | makes every draw a hardware draw of its own, with a draw table of its own, instead of one `vkCmdDrawIndexed` for a run of indexed draws that change no state: what merging saves, measured on the device ([rendering.md](rendering.md#the-recorder-thread)) |
| `MW2_TIME_RENDER=1` | | where the ring consumer's time goes, and how long it waits on the GPU |
| `MW2_DRIVER_ENV="NAME=value;NAME=value"` | R | variables for the graphics driver, set before Vulkan starts (Mesa's `TU_DEBUG`, `IR3_SHADER_DEBUG` and the like): `.env` passes only `MW2_` switches, and on a phone the app's process environment is not otherwise reachable |
| `MW2_SHADER_STATS=1` | | on a device with `VK_KHR_pipeline_executable_properties`: every pipeline drawn with is built capturing the driver's statistics (`pipelineExecutableInfo` is enabled only with this switch), and each executable is logged as `shaderstats: vs <hash> ps <hash> stage <stage>/<executable>: <statistic>=<value> ...` (instruction and register counts and the like, whatever the driver reports; a name's spaces are `_`). The end-of-run report lists the 40 vertex shaders drawn with the most vertices as `shaderstats: weight vs <hash>: <n> draws, <n> vertices`, which says which shader's statistics matter |
| `MW2_TIME_PASSES=<seconds>` | | from that far into the run, the GPU's time a frame by rendering instance (by target size, samples and format), resolve and the work between them, from a timestamp at each; a table at exit |
| `MW2_TRACE_PACING=<file>` | | a timeline written at exit: the consumer's batches and swaps, the title's presents, and every guest thread's engine waits, kernel waits and sleeps with their callers ([multiplayer.md](multiplayer.md)) |
| `MW2_STUTTERS=1` | | (at the console's pacing only, not under `MW2_FPS_LIMIT`) from the first 30 consecutive world frames on, logs `STUTTER` for every world frame 25 ms or more after the previous one, with what the renderer spent the gap on, and `DISPLAY` when a frame is held or dropped by the window; totals at exit. When play stops for a second it logs `STALL` with what every guest thread waits on and every thread's stack (Linux), once a stall. The pad's Y and `F7` log a `STUTTER MARK` |

## Frames and captures

| switch | effect |
|---|---|
| `MW2_DUMP_FRAMES=<dir>` | finished frames as PNM, every `MW2_DUMP_FRAME_EVERY` frames (600); `MW2_DUMP_FRAMES_AFTER_SECONDS` starts later, `MW2_DUMP_WORLD_FRAMES=1` keeps only frames that drew the world |
| `MW2_DUMP_TARGETS=1` | every colour surface beside the presented frame |
| `F7` | with the window open: marks a flash in the log. `MW2_FLASH_FRAMES=<n>` keeps the last n frames and writes them at each mark into `MW2_FLASH_DIR` (`diagnosis/flashes`). `MW2_FLASH_HUNT=1` (or `MW2_FLASH_DRAWS=1`) also writes every draw of the kept frames as `draws_rNNNNNN.txt`; `MW2_FLASH_HUNT=1` makes the pad's Y a mark and logs every texture drawn empty. `MW2_MARK_EMPTY=1` paints those magenta. `MW2_FLASH_MARK_AT=<seconds>` makes a mark nobody pressed. `tools/flash_hunt.sh` sets this up |
| `MW2_RENDERDOC=<seconds>[:<frames>]` | captures that many world frames (4) once the run is that old; `MW2_RENDERDOC_OUT` names the file prefix. RenderDoc must be injected before the Vulkan instance exists: run under `tools/capture_by_hand.sh` or `renderdoccmd capture` |
| `F10`, `F11` | with the window open: F10 captures the next four world frames; F11 captures every frame until pressed again (at most 120). The window runs a frame behind, so the picture in capture N was rendered in frame N-1 |
| validation | `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` enables the validation layer; its messages go to the log as `validation error:` / `validation warning:` lines, counted at exit. Add `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` for hazards between submissions |

## Tools

| switch | effect |
|---|---|
| `MW2_TITLE=sp\|mp` | which executable the Python tools work on (`tools/title.py`); `build.sh` sets it from `TITLE` |

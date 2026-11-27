# Testing on a Mali phone

Nobody who works on this port has a phone with an Arm Mali GPU (Samsung
Exynos, Google Tensor, MediaTek Dimensity and Helio). What is known about
them was worked out on a PC ([android.md](android.md#mali)): real phones'
driver reports say the renderer's requirements are met on a Mali-G57 or
newer, and Arm's own shader compiler takes every shader. Whether the picture
is right, and how fast it is, only a phone can say. If you have one, this is
what helps.

## Before you start

- A Mali-G57, G68, G77, G78, G310, G510, G610, G615, G710, G715, G720 or an
  Immortalis. A Mali-G52, G72, G76 or older cannot run it, and the log's
  `device:` line says what is missing.
- Leave the graphics driver on **System**. Turnip is for Qualcomm phones
  and is not loaded on anything else.

## What to do

1. Install the app, then the game code: it is not in the app, and is built
   from your own disc by `setup\setup.cmd` on a PC with the phone connected
   over USB ([game-library.md](game-library.md)). Without it the app says
   "The game code is not on this device" and no run starts, whatever the
   phone.
2. Let the app set the game up, and start it.
3. Play five minutes: the menus, a level's opening, a firefight. Note what
   looks wrong and roughly when (a screenshot is worth more than a
   description).
4. Leave the game from its own menu if you can, so the log is finished.

## What to send

The launcher's **Log** screen, **Share**: that one text is enough (v0.1.1 on;
[what it holds](android.md#what-the-log-share-holds)). Read it first: it
names folders on your phone. It opens with the app's version, the phone, its
chip, memory, GPU and driver, then how Android says the last runs ended
(killed for memory, crashed, with a crash's backtrace), then the logs.

If the game closed by itself, open the app again and share from the launcher:
the record of how the run ended is read then.

Add what you saw: nothing drawn, wrong colours, flicker, missing textures,
a freeze, a crash, or "it plays", and the frame rate the game's statistics
line showed.

These lines of the log are what gets read first, so check they are there:

| Line starts | Says |
|---|---|
| `device:` | whose GPU and driver this is, which Vulkan, and every fallback the renderer chose for it, or `refused:` and what the device lacks |
| `driver:` | which Vulkan library was opened (the system's, on a Mali) |
| `vulkan: subgroups of` | which form of pixel shader the device was given |
| `renderer: ready on` | the depth format and the size of the frame arena |
| `textures: uploading on` | whether the driver samples BC textures or they are decoded |
| `pipeline:` | what the pipeline cache held and how long shaders took to compile |
| `report:` | frame times and what the GPU and each thread spent, written as the run ends |
| `app: the run was not started` | the app did not start a run, and why (the game code is missing or was built for another version, the game files are not installed) |
| `step:` | start-up, a line before each part; the last one is what the run was doing if the log ends there |
| `last step:` | a fault's report: the signal, the step and the backtrace follow |
| `vulkan: the device was lost` | the driver reset the GPU: the lines just above are what it was doing |
| `stall: no frame has reached the window` | ten seconds (then 20, 40, 80...) with no new frame on screen, and where each stage of a frame was: how many frames the title swapped in that time, whether the ring consumer is stuck in one batch (and at which draw stage) or has nothing to read, the pipelines asked for and built and how long the one under way has taken, the newest frame the renderer finished, and what the window thread is waiting for |
| `cpu:` | which cores the ring consumer, the title's threads and the background work were given, and whether by the kernel's capacity or by clock |

## If something is wrong

Each of these is one line in a file named `.env` in the game folder
(`Android/data/<the app>/files/game/.env`, made with any file manager), or a
setting in the app. Try one at a time and send the log of each run.

| Try | What it changes |
|---|---|
| `MW2_PIXEL_RECORDS=subgroup` | the other form of pixel shader (the one an Adreno 740 runs). A Mali gets the form without subgroup operations by default; if this one is faster and looks the same, say so |
| `MW2_PIXEL_UNIFORMS=0` | a pixel shader's constants fetched from the frame arena instead of bound as a uniform block |
| `MW2_FORCE_LEGACY_RENDERING=1` | render passes instead of dynamic rendering (what a Vulkan 1.1 Mali driver uses anyway) |
| `MW2_NO_SHADOW=1` | no shadow of guest memory behind the arena: half a gigabyte less, and the path every Adreno phone runs. Only a phone of more than 6 GB with a driver from r44 on has the shadow at all |
| `MW2_FORCE_BC_DECOMPRESS=1` | BC textures decoded by the runtime even where the driver says it samples them (some r54 and later drivers do) |
| Settings → **Keep pipeline cache between runs** (`MW2_PERSIST_PIPELINE_CACHE=1`) | keeps the driver's pipeline cache between runs under a Mali driver older than r38 too; from r38 on it is kept anyway (`MW2_PERSIST_PIPELINE_CACHE=0` refuses it there). A run that crashes at start right after one that compiled shaders: **Clear shader cache** in Settings, and say so in the report |
| Settings → **Smooth edges (multisampling)** off | the default; turn it on only to test it |

A first run that stutters while a level loads is shaders compiling, not a
fault. A second run that stutters the same way is worth reporting: the log's
`pipeline: the pipeline cache is kept/not kept between runs on this Mali` line
says which it was.

## Answering a first report

A report from v0.1.0 has neither the device nor the reason a run ended.
The first one from a Mali phone was a `game.log` that stopped after
`android: the surface votes for a display rate`: it has no `game library:`
line and none of the settings (`android: MW2_FULLSCREEN=1` and on), so the
app never started a run; it stopped where the game code is looked for and
showed "The game code is not on this device". The launcher's log ended with
`install: done`: Install & Play had started the game with the files in place
and the code missing. Nothing in it is about the GPU; the driver's
`Could not open module param file '/sys/module/mali_kbase/parameters/large_page_conf'`
is printed on every start and means nothing.

A reply to send:

> Thanks for trying it on a Mali phone, nobody here has one. Your log shows
> the app stopping before the game starts: it could not find the game code,
> which is not part of the app (did it say "The game code is not on this
> device"?). It is built from your own disc on a PC: run `setup\setup.cmd`
> from the project folder with the phone connected over USB, then start the
> game again. Please also update the app to v0.1.1 first, tell us the
> phone's model, and after the next try send the text from Log → Share in
> the launcher: it now says which phone and GPU it is and how the run ended.

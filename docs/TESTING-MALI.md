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

1. Install, let the app set the game up, and start it.
2. Play five minutes: the menus, a level's opening, a firefight. Note what
   looks wrong and roughly when (a screenshot is worth more than a
   description).
3. Leave the game from its own menu if you can, so the log is finished.

## What to send

From the app's **Log** screen (**Share**), or
`Android/data/<the app>/files/game.log` on the phone:

- **`game.log`**, whole. Read it first: it names folders on your phone.
- The phone's model and Android version.
- What you saw: nothing drawn, wrong colours, flicker, missing textures,
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
| `vulkan: the device was lost` | the driver reset the GPU: the lines just above are what it was doing |

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
| Settings → **Keep pipeline cache between runs** (`MW2_PERSIST_PIPELINE_CACHE=1`) | keeps the driver's pipeline cache between runs. Off on a Mali, because older drivers reloaded it badly; with it off every run compiles its shaders again, which is slow in the first minute of each level |
| Settings → **Smooth edges (multisampling)** off | the default; turn it on only to test it |

A first run that stutters while a level loads is shaders compiling, not a
fault. A second run that stutters the same way is worth reporting with
**Keep pipeline cache between runs** on.

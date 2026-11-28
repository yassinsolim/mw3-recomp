# Rendering

This document covers how draws and resolves from the PM4 stream become Vulkan
work and reach the screen. Stream parsing is in [d3d9-seam.md](d3d9-seam.md),
shader translation in [shaders.md](shaders.md), and the texture cache in
[textures.md](textures.md). The code is in `runtime/gpu/vulkan/`, and
`renderer_state.h` lists the parts.

A draw packet carries only the primitive and the indices. Everything else
(shaders, constants, fetch constants, targets, blend and depth state) is
whatever the register file holds, so `vk::renderer::Draw` takes the register
file plus a `DrawCall`.

The **ring consumer** does everything tied to its point in the stream. It reads
guest memory, chooses targets and pipelines, and fills the arena. It queues the
Vulkan calls to the **recorder thread**. A **window thread** presents. Worker
threads compile shaders and optimised pipelines.

## Render targets: an image per EDRAM surface

EDRAM is not emulated. MW2 neither aliases surfaces nor renders in tiles, so a
surface is a Vulkan image keyed by `TargetKey { baseTile, pitch, host format,
samples, depth }`. The key comes from `RB_COLOR_INFO` / `RB_DEPTH_INFO` and
`RB_SURFACE_INFO` (pitch in bits 0-13, samples in bits 16-17).

- **Depth keys have no pitch.** EDRAM has one depth buffer per base tile, and
  MW2 tests world depth in a pass that describes it at another pitch than the
  pass that wrote it. Depth keys keep the sample count, which must match the
  colour image beside them.
- **Keys hold the host format**, so `8_8_8_8` and `8_8_8_8_GAMMA` at one tile
  are one image, as the title's resolves expect.
- A target is as wide as its pitch and as tall as the bottom of the window
  scissor, each at most 4096. A target that must grow is rebuilt.
- A new target is cleared once. Depth goes to 0 (MW2 tests `GEQUAL` and
  clears to 0). Colour goes to dark blue-grey, so an undrawn surface is not
  mistaken for black.
- Only render target 0 is bound.

| Guest colour format | Vulkan |
|---|---|
| 0, 1, 10 (`8_8_8_8`, `_GAMMA`) | `R8G8B8A8_UNORM` |
| 2, 3, 12 | `A2B10G10R10_UNORM_PACK32` |
| 4, 5 | `R16G16_SNORM`, `R16G16B16A16_SNORM` |
| 6, 7 | `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` |
| 14, 15 | `R32_SFLOAT`, `R32G32_SFLOAT` |

Depth is `D24_UNORM_S8_UINT`, or `D32_SFLOAT_S8_UINT` on devices without it.
Draws use dynamic rendering. Both attachments are `LOAD`/`STORE`, because a
frame is many rendering instances over the same images and only the title
clears. Barriers at `BeginRendering` and `EndPass` stand in for render-pass
dependencies.

### Clears

- **With a resolve.** `RB_COPY_CONTROL` bits 8/9 clear what the resolve read:
  `RB_COLOR_CLEAR` is ARGB, and `RB_DEPTH_CLEAR` is depth << 8 | stencil.
- **The EDRAM clear idiom.** MW2 draws a rectangle list with depth writes on,
  `ZFunc` `ALWAYS`, and the X/Y viewport scale off. The rectangles come in pairs:
  most of a row at half pitch and twice the samples, then the leftover columns,
  because a surface is not a whole number of 80-sample tiles.
  - In depth-only mode, the rectangles are not drawn. `ClearAliasedDepth` zeroes
    every depth target at or above their base tile. This happens before any
    target is sized, because the scissor is at its 8192 reset value.
  - **Part of a depth surface.** D3D's `Clear` of a rectangle inside a depth
    surface is the depth-only form too, in two draws: the tiles the rectangle
    covers whole as one rectangle of a surface half as wide at 4x, and the
    columns and rows round them as strips of the surface itself. Advanced
    Warfare writes the nearest depth (z 1) so into the middle of the sun's
    far shadow map, the part the near map is read for, in the pass' command
    buffer before its casters and again after the buffer has run. A
    rectangle with a depth other than zero, in columns short of both ends of
    the row, where a depth surface is, is left undrawn (`DrawInSlot`,
    "depth written into part of a surface"): emptying the images for it
    emptied the map the casters had just been drawn into. The casters are
    drawn into that part as well, which the title does not read. Advanced
    Warfare only (`AW_TITLE`): the other titles' frames have not been
    looked at for such rectangles with their sun shadows on.
  - **A colour surface cleared as depth.** D3D's `Clear` of a colour surface
    is the depth-only form as well: `RB_DEPTH_INFO` names the colour surface's
    own base tile as D24S8, at half its pitch and 4x, and one rectangle covers
    it with the stencil operation `REPLACE`. The sample is left holding
    depth << 8 | stencil reference, which read as `8_8_8_8` is the clear
    colour (z 0xFF0000 / 0xFFFFFF and reference 0 are opaque black). MW3
    clears its interface this way once a frame and in no other. An
    `8_8_8_8` image at that base tile, with as many tiles a row and no more
    rows than the rectangle, is cleared to those bits (`ClearAliasedColour`).
  - In colour+depth mode, each half is drawn into the colour surface with the
    lowest sample count at that tile and format whose `pitch << samples` spans
    the same row.
- **Ordinary draws.** Nothing clears through a load op.

## A draw

`Draw` resolves the topology, both shaders (by microcode hash) and the scissor,
and handles the clear idiom. It finds the targets and opens a rendering
instance if they changed. It writes indices, constants and vertex data into the
arena, builds the pipeline, finds each sampled texture's place in the texture
set, and writes the draw's record: where its constants are, its textures, and
the register state its shaders apply. It queues a `DrawOp` holding only the
dynamic state that differs from what the command buffer has bound, and the
draw, whose entries in a draw table name its record. Draws that cannot be made
are counted by reason (`Skip`).

Primitives 1-6 map to Vulkan's point, line and triangle topologies, and 12 (line
loop) to a line strip. 8 (rectangle list) becomes triangles `a b c a c d`, with
a fourth vertex `v0 + v2 - v1` appended per rectangle, computed per dword as a
float except where all three corners agree (packed colours).

| State | Registers | Where |
|---|---|---|
| blend, write mask | `RB_BLENDCONTROL0`, `RB_COLOR_MASK` | pipeline |
| culling, winding | `PA_SU_SC_MODE_CNTL` | dynamic |
| depth and stencil tests | `RB_DEPTHCONTROL` | dynamic |
| stencil ref and masks | `RB_STENCILREFMASK(_BF)` | dynamic |
| depth bias | `PA_SU_POLY_OFFSET_*` | dynamic |
| viewport, scissor | `PA_CL_VPORT_*`, `PA_CL_VTE_CNTL`, `PA_SC_WINDOW_SCISSOR_*` | dynamic |
| alpha test | `RB_COLORCONTROL`, `RB_ALPHA_REF` | the draw's record |

- **Viewport.** The Vulkan viewport is `height = 2 * scale`, `y = offset -
  scale`, **with signs kept**. Xenos expresses +Y up as a negative Y scale.
  Taking the magnitude flips the frame and every winding. A term that
  `PA_CL_VTE_CNTL` disables is scale 1 and offset 0 (Xenia's
  `GetViewportInfo`).
- **Pre-transformed vertices.** With the X/Y scale off, the shader has emitted
  window coordinates (D3D9's `XYZRHW`), which Vulkan would clip away. The
  vertex epilogue applies `(2 / guest pitch, 2 / height, -1, -1)` from the
  draw's record over a whole-target viewport, and the identity for other draws.
- **Depth bias** uses Xenia's conversion. The offset is scaled by 2^24
  (`D24FS8`) or 2^24 - 1 (`D24S8`), and the slope is divided by 16.
- **Alpha test** runs in the pixel shader, and is forced to `ALWAYS` in
  depth-only mode, where the console runs no pixel shader. Shadow casters have
  none and would otherwise run whatever the stream loaded last.
- **A draw that writes no colour** is not drawn with the pixel shader the
  stream loaded last. In depth-only mode, and with every channel masked when
  the shader neither kills, nor writes depth or memory, nor has its alpha
  tested, the pixel shader is one that does nothing
  (`vk::shaders::kNoPixelCode`, a program of the console's microcode that
  ends at once), and the blend and mask are one pair. The pipeline is then
  the vertex shader's alone: named after the leftover shader, the sun's
  shadow casters were a new pipeline each time the shader before them
  changed (`WritesNoColour`; `MW2_DEPTH_ONLY_PIPELINES=0` is the old way).

## The frame arena

Everything a draw reads that is not a texture is copied into one host-visible
buffer. The runtime asks for 512 MB (`MW2_ARENA_MB`) and halves the request
down to 32 MB. The buffer is split into one share per frame slot, each 4 KB
short so that a full 256-vec4 uniform range stays in bounds. A draw that finds
its share full is skipped.

The buffer is written and never read back. The driver maps it write-combined,
where a read is many times the cost of one from ordinary memory, so whatever
has to be looked at again after it is written (the fetch constants while their
addresses are rewritten, a rectangle's corners) is put together in ordinary
memory and copied across once. Reading the fetch block in place cost a fifth
of the ring consumer's time on Team Player. Of the fetch block's 96 pairs only
those the draw's two programs read are put together and written (the
translator records them, `fetchPairsRead`); the rest of it is whatever the
arena held, which nothing reads.

| Set | Contents |
|---|---|
| 0 | nothing |
| 1 | every texture and sampler, bound once per command buffer |
| 2 | the arena as dwords and as vec4, bound once per command buffer; where a pixel shader's float constants are a uniform block ([shaders.md](shaders.md#the-module-interface)), bound again at a draw whose pixel constants are others than the last's; where a stage's shaders read the arena as texels, the slot's share twice more as texel buffer views |

A draw's record says where each of its five constant blocks is in the arena
([shaders.md](shaders.md#the-module-interface)). Consecutive draws whose
records would be the same share one.

Indices and draw table entries are two streams of their own, each handed out
in order from a chunk of the share (1 MB of indices, 64 KB of entries; a new
chunk when one is used up), so that what consecutive draws write lies in a
row. Indices are always 32 bits wide: an index's high sixteen bits name the
draw's entry in the draw table, an entry is the dword offset of the draw's
record and a guest index, and the low sixteen bits count from that guest
index. A draw whose indices span more than 65,536 vertices takes an entry for
each 65,536. A table has at most 32,768 entries, so an index stays below
2^31, and fewer where the device's `maxDrawIndexedIndexValue` is smaller.
The title's 16-bit indices are written in one pass with a base of zero; its
32-bit indices take a pass to find the lowest and highest and a pass to
write. No pipeline has primitive restart on: a 16-bit 0xFFFF is vertex 65,535
as it always was, and a 32-bit 0xFFFFFFFF, which would name an entry outside
the table, becomes the draw's lowest vertex.

Where the vertex shaders read the arena as texels (an Adreno 6xx under Turnip,
[shaders.md](shaders.md#the-module-interface)) an entry is eight dwords at a
multiple of four: the same two, and six that hold what the draw's vertex
program would otherwise load through the record -- the first dword of each
vertex fetch constant it names and the place of each block of float
constants it reads -- written from what the draw has just put in the arena
(`WriteDrawTable`; the fetch constants' first dwords are kept beside the
block as it is written, the arena not being memory to read back). A fetch
constant whose data such a program fetches four dwords a texel is pointed at
a multiple of four dwords: a copy made for the draw is one, the arena's
alignment being coarser and such a vertex a whole number of texels long; a
kept window whose buffer is not sixteen bytes' multiple from its range's
start, or a copy another slot of the draw made, may not be, and the draw
then copies its own. The fetch block is kept for the next draw only if that
draw wants the same constants at texels.

Where the pixel shaders read the arena as texels (the same devices, the same
section) a record holds a texel for each slot the pixel shader samples
instead of a word: the word, the fetch constant's LOD bias and exponent
adjust, and one over the size of the image's level 0, which the texture cache
keeps with each image (`vk::textures::Handle`; a resolve's copy is adopted
with its image's size). `TexturesFor` writes them, from the register file's
fetch constants and the slots' images, so the pixel shader's fetch constants
are not copied into the arena at all.

- **Float constants.** Each stage reads 256 from its `SQ_VS_CONST` /
  `SQ_PS_CONST` base. A pixel program's window is copied as far as the shader
  reads (all of it under relative addressing), and reused while the register
  file's write stamp over that range is unchanged. A vertex program's is
  copied a block of 16 at a time: a block whose stamp has not moved since its
  last copy into this frame's arena is not copied again, and the draw gets a
  table of where the blocks are. With four players in split screen that halved
  the constants copied a frame, 25 MB to 13 MB. D3D sends a draw's constants
  whole, so a write of the value a register already holds does not stamp it,
  and a run of them is swapped, compared and stored in one pass straight from
  the command buffer, and stamped, a block of 64 registers at a time
  (`RegisterFile::WriteRun`). Fetch constants are stamped by every write: the
  vertex data they name is copied with them, and the title rewrites some
  buffers in place.
- **Vertex data is relocated, not mapped.** Each vertex fetch's reachable
  window (lowest to highest index times the stride, plus the fetch tail,
  clipped to the declared size) is copied. Its address in the uploaded fetch
  constant is rewritten to point at the copy. A copy is shared only within
  **one draw**, because the title rewrites dynamic geometry in place during a
  frame. Without the shadow of guest memory the copy is made in host byte
  order ([below](#vertex-data-in-host-byte-order)).
- **Indices** are byte-swapped from big-endian for every draw, never cached,
  and scanned for their range, ignoring the restart value.

### The shadow of guest memory

The same buffer continues with a 512 MB shadow of guest physical memory, in
which physical address N sits at offset N. A vertex window in pages the title
has not written since they were copied is read there instead of being copied.
`gpu/memory_watch.cpp` tracks writes as Xenia does. Copied pages are made
read-only through both windows of the physical bank, and the title's first
write faults once, which marks the page and makes it writable again.

- A page's copy is replaced only once no submission on the GPU has read it.
- A page rewritten within 1024 frames of its copy waits 2^strikes frames (at
  most 2^10) before it is copied again. This keeps the dynamic-geometry ring
  from costing faults and `mprotect`s on every lap.
- The host kernel must never write into guest memory, because a system call
  fails with `EFAULT` on a read-only page. File reads and network packets go
  through a runtime buffer. Bulk runtime writes call `gpu::watch::WillWrite`.
- A page marked written is writable, always: the handler answers a fault on
  one without another look, so a page left marked and read-only is a write
  that faults for ever. Copying a page and marking it written are two steps
  each (the protection and the mark). `WillWrite` and the copy exclude each
  other with a lock; the handler takes none, so a page it marked while the
  copy was being made is made writable again by the copy when it finds the
  mark.

The texture cache uses the same watch. `MW2_NO_SHADOW=1` turns the shadow off,
and it is read by every build rather than only a diagnostics one, because it is
512 MB of resident memory before the first frame is drawn: the Android runtime
sets it on a phone of six gigabytes or less ([android.md](android.md)).

### Vertex data kept without the shadow

The shadow needs arena and shadow within what one storage buffer descriptor
reaches. Turnip reaches 128 MB, so on an Adreno under it there is none, and
every draw copied its vertex windows: 12 to 31 MB a frame of Modern Warfare 2,
half of the ring consumer's time on a Snapdragon 865, and in Team Player more
than a slot's share held, so its far buildings and its sky went undrawn
(`draws skipped: frame arena full`, five million of them in one flight).

There a frame slot keeps what it copies (`KeptWindow`, `renderer_arena.cpp`),
at the end of its share, the frame's own allocations coming up from the
start:

- **By block.** What is kept is the block of guest memory the window lies
  in: a power of two of 64 KB chunks at a multiple of its own size, so two
  blocks are apart or one inside the other, never partly over each other.
  The windows of other draws into the same buffer fall inside it, and a
  window that reaches past it makes the block that holds both, which takes
  the place of the ones inside it. A block is at most four times its window
  rounded up and at most 16 MB; a window no block is allowed for, or whose
  block holds a page the title keeps writing, is kept alone. Kept window by
  window, Team Player's filled the share every few frames; kept as the window
  rounded out to chunks, the ranges lay over each other and a longer range
  from the same start threw the shorter one's place away, 76 MB of a share
  for 41 MB of vertex data.
- **For as long as the watch says the window's pages are unwritten**
  (`gpu::watch::TrackSteady`, `Unwritten`). `TrackSteady` is `Track` with the
  shadow's pacing: pages the title keeps writing are not watched again for
  2^strikes frames, and a window in them is copied by its draw as before.
- **At the second frame the slot meets the range unwritten.** The title's
  dynamic geometry goes round a ring, each stretch drawn once and written
  over a lap later; kept at first sight it took a megabyte of the share a
  frame.
- **Brought up to date at its first use in a frame** (`Refresh`): the pages
  written since the copy was made, a soldier's skinned vertices for one, are
  copied again where they are, page by page. In place because the slot's
  earlier frames are done before it begins another and no draw of this one
  has named the range yet; a window written later in the frame is copied by
  its draw. The written pages are watched again a run at a time, and the
  watch refuses a run as a whole when one page of it waits its turn or is
  written meanwhile. The run's other pages may be watched all the same, by
  another slot's frame since: they are copied too, a page at a time. Left
  as they were (until 27 November 2026) they read as current once a later
  run's stamp was the range's, and the slot drew from what it had copied
  laps before, one frame in three, for as long as the title left the page
  alone. Counted with the copy compared against guest memory at every draw
  (a build made for it): four to twenty-five windows a three-minute match of
  Advanced Warfare on Windows with `MW2_NO_SHADOW=1`, four on the Flip 2,
  and none in either after.
- **Places are used again.** A range another took the place of, and one the
  slot lets go, leave their places to the ranges that come; neighbouring
  spare places join, and the lowest goes back to the frame's own.
- **Let go a part at a time** when the copies reach two thirds of the share
  (the frame's own keeps a third, 24 MB at least): the ranges the slot has
  not drawn from for 300 frames go. All of them go only when that frees less
  than a sixteenth of the share, or when what is kept leaves a single draw no
  room.

The ranges are looked up by every vertex fetch of every draw, so a slot
holds them as two rows in the order of their starts, the starts by
themselves (`keptStarts`, `kept`): a few kilobytes that stay in the nearest
cache. The ranges met once and not yet copied are a table with one range in
a place (`met`); a range another pushes out is met again, a frame later than
it would have been. As trees (`std::map`) a window that is not kept walked
three of them, a dozen nodes each scattered over the heap: 6 to 8% of the
ring consumer on the AYN Thor, 3.5 to 4% since.

Each of the three slots has its own, so a range is copied three times in all.
On the Flip 2 the vertex data copied a frame went from 31 MB to 4 in Team
Player (33 MB a frame drawn from kept copies), the ring consumer from 76% of
its core to 46%. `MW2_KEEP_VERTICES=0` copies every window. The arena is
384 MB on a phone of 7 GB and more, a share of 128 MB where the reach is
that.

### A frame larger than its slot's share

A slot's share cannot grow past what one storage buffer reaches, and a frame
can need more: one whose slot keeps nothing yet copies every window of every
pass, 105 to 116 MB of vertex data in Team Player on the AYN Thor, beside
11 MB of indices and 5 MB of constants. That is the level's first frames, the
frames after a checkpoint is loaded again, and the frames after a slot let
its copies go, which used to be whenever its frame's own copies ran out of
room: 34,197 draws skipped in one flight of two minutes there and 8,486 on
the Retroid Pocket Flip 2, each in a frame with most of the scene missing.

A draw the share has no room for is now made in the next slot
(`vk::renderer::Draw`): what the frame has recorded is submitted, as a
resolve does with a share half used, and the draw is made again from its
start in the slot after, whose share is empty but for what it keeps. A slot's
kept copies stay when its frame's own run out; they go only where they alone
leave a draw no room. So a draw is skipped as `frame arena full` only if a
share with nothing kept cannot hold it. The report counts the draws made
that way (`draws found their frame slot's share of the arena full`): 1 or 2
in that flight on either device, at the level's start, with no draw skipped
and no slot letting its copies go (61 to 72 MB kept at most). With the kept
part held to 40 MB for a test the slots let stale ranges go 16 times and all
of them 15 times in the flight, and still no draw was skipped.

### Vertex data in host byte order

The console stores a dword of vertex data one of four ways, which the fetch
constant names: as it is, 8-in-16, 8-in-32 or 16-in-32. A vertex shader made
the exchange itself, ten operations for each of the ten or so dwords a vertex
loads, every time the vertex was shaded. Without the shadow every dword a
vertex shader loads is one the renderer copied, so the copy makes the
exchange instead (`CopyVertices`, `renderer_arena.cpp`): one of four loops
over the window, once a copy, and a kept window is drawn from for many
frames. The shaders are built to load the dword as it lies
(`shader::VertexOrder::Host`), and the renderer's copy of the fetch constant
carries no endianness.

The shader skips its exchange exactly when the copy made it:

- **One answer for both.** `vk::pipeline::HostOrderVertices` is what the
  translator is built with and what the copies ask. The renderer says
  whether it has a shadow each time it makes its arena, before any shader
  is built. With the shadow, vertex data is read where the guest's bytes
  lie, so there the shaders exchange as before and the copies are plain.
- **Whatever the format.** The shader's exchange was of the whole dword it
  loaded, before any format was decoded, and a fetch address is whole
  dwords from a buffer that starts on one. So the same exchange at the copy
  gives every format the dword it had, and a buffer two fetches read as
  different formats is still one copy.
- **A copy belongs to one endianness.** Two fetch constants of a draw that
  name one buffer with different endianness get a copy each. A kept range
  records the endianness it was exchanged for, and a draw whose constant
  names another copies its own window: a 64 KB chunk holds the buffers of
  many draws. In Team Player the kept copies serve as much as before (54 MB
  a frame drawn from them, 9 MB copied, either way).
- **Rectangle lists** have their fourth corners worked out from the guest's
  bytes, and the finished vertices are exchanged as they go into the arena.

On the AYN Thor (Adreno 740, 120 Hz, two runs each way): the opening of
S.S.D.D. went from 103.3 to 105.9 frames a second and 9.2 to 9.0 ms of GPU
a frame; Team Player's heaviest views from 11.5 to 11.0 ms for the scene,
74.5 to 76.5 frames a second over the run; Modern Warfare 3's Survival from
87.3 to 91.7. The flight over S.S.D.D. and Rust are near the display's rate
either way (113 and 118; their scenes 4.15 to 3.96 and 3.79 to 3.64 ms). On
the Retroid Pocket Flip 2 (Adreno 650, two runs each way, alternated) Team
Player's heaviest views went from 17.0 to 15.8 ms for the scene and 42.7 to
45.6 frames a second; the opening of S.S.D.D. is at its limit of 60 either
way, its scene 10.7 ms. Five fixed views of S.S.D.D. differ by 0.06 to 1.4
of 255 a pixel between the two, and by 0.03 to 1.8 between two runs of one;
a sixth, of soldiers moving, by 5.1, and by 2.2 to 6.1. `MW2_HOST_ORDER=0`
keeps the exchange in the shaders.

Vertex attributes were tried with it and are not in the tree (branch
`vertex-input`): a vertex shader's first stream bound as a vertex buffer
and fetched by the fixed-function stage. A run of merged draws then needs
its vertices in one buffer, so draws whose vertices lay under another
draw's record were copied again (40 MB a frame in Team Player against 9),
and the runs were shorter. On the Thor the opening of S.S.D.D. fell from
103.6 to 82.3 frames a second and Team Player's views from 74 to 49, with
no less GPU time (9.2 to 9.6 ms); for world shaders only, 103.8 and 62. On
the Flip 2 the scene went from 16.5 to 17.3 ms, and to 18 to 19 for world
shaders only.

## Resolves

A draw in `Copy` mode is a resolve over the window scissor. `RB_COPY_CONTROL`
selects colour or depth (source 4) and the clears, `RB_COPY_DEST_BASE` gives
the destination, and `RB_COPY_DEST_PITCH` / `_INFO` describe the destination
surface.

**Resolves are not written to guest memory.** They fill a Vulkan image keyed by
destination address, and a texture fetch of that address binds the image.

- **An image per frame slot.** The first resolve of a frame advances to the next
  image, and the frame's later resolves reuse it. Frames overlap on the GPU,
  and the title resolves several shadow cascades into one destination per
  frame.
- The image has the size of the destination surface, exactly so on the frame's
  first resolve, because fetches normalise over the image. Each rectangle lands
  at its own offset.
- **The surface's width is its texture's.** `RB_COPY_DEST_PITCH` gives the
  pitch, the width rounded up to 32 texels, and a fetch normalises over its
  own width. A 960-wide scene's quarter-size blur buffers are 240 wide in
  256-wide surfaces, and an image as wide as the pitch stretched them: the
  aimed sight's blur doubled, the menus' blurred backdrop slid off the right.
  The first fetch narrower than its copy is remembered
  (`State::fetchedWidth`), and the resolves after it make the image that
  wide. The console's sizes never differ.
- **A resolve that lands inside a surface started this frame** goes into that
  surface's image at the matching row, without discarding it. The sun shadow
  map is one 1024x2048 surface filled by two resolves.
- Colour and depth are copied, or resolved straight into the image from a
  multisampled surface ([Multisampling](#multisampling)), at the place the
  rectangle names, with nothing in between. Colour also gets an sRGB view.
  Depth is sampled through a depth-only view, since the title reads scene
  depth as a texture; nothing reads a resolved stencil, so a copy takes the
  depth alone.
- **The frame** is the front buffer the swap packet names (`PM4_XE_SWAP`, which
  `VdSwap` writes): a resolve destination like any other. At the swap its
  newest copy is blitted into one of four present images, run through the
  display colour table, and submitted. A resolve is never shown for having the
  display's size: the title also keeps the screen in a 1280x720 texture, before
  the interface is drawn, for the next frame's blur.

A resolve does not end the submission unless the slot's arena share is half used.

## Gamma

- **Encode on write.** Xenos applies a piecewise-linear gamma curve to RGB
  written into an `8_8_8_8_GAMMA` target, where MW2 draws the world. The pixel
  shader does the same (Xenia's `LinearToPWLGamma`) when the flag in the
  draw's record is set.
- **Linearise on read.** A fetch whose component signs say `gamma` samples
  through an sRGB view or format, which matches the Xenos curve to within a
  code value.
- Both are required. With only one, the post chain compounds the encoding.
- **Display colour table.** The title writes `DC_LUT_30_COLOR` 256 times from
  `DC_LUT_RW_INDEX` 0, which auto-increments, with channels selected by
  `DC_LUT_WRITE_EN_MASK`. Each entry is 10 bits per channel, blue lowest. Only
  a whole sweep is a curve, so the table is published under a lock at entry 255,
  and frames are shown untouched until then. `display_table.cpp` applies it as
  a compute pass on the present image.
- **Whose table it is.** D3D9 writes the table at a present, from the ramp
  the title last gave `SetGammaRamp` (the title's Brightness setting) or,
  for a frame whose block from `VdGetSystemCommandBuffer` is not zero at
  +8, from a straight ramp
  ([d3d9-seam.md](d3d9-seam.md#the-vd-imports-runtimekernelvideocpp)). While
  that block was left unwritten, Advanced Warfare's table alternated: the
  straight ramp for most frames and the title's for one frame, seven times
  in eight seconds of the multiplayer's lobby on a Retroid Pocket Flip 2 and
  ten in twelve seconds of a match, each a flash of the whole frame, HUD
  included, of 13 levels of 255 at the owner's Brightness. It is not the
  graphics chip's: the logs of 40 of 58 earlier Advanced Warfare runs,
  Windows' among them, count hundreds of table changes each. With the block
  zeroed the table is written twice in a run (D3D9's own at start-up, then
  the title's) and stays: no flash in 20 seconds of the lobby nor in 12 of a
  match, at 60 frames a second as before, and the picture is the brighter
  of the two, the one the title's Brightness asks for (the lobby's mean
  level 47 of 255 where it was 34). The other three on the same device, at
  their first screens, with the block unwritten and zeroed: Modern Warfare
  3's table was the straight ramp alone and is now the title's (a mean
  level of 33 where it was 30), so its Brightness reaches the screen;
  Modern Warfare 2's and Call of Duty 4's are the same table either way.

## Resolution scale

`MW2_SCALE=2` or `3`, read once at start-up, makes every EDRAM surface, every
resolve copy and the present images that many times wider and taller. The
title is not told. A `Target`'s and a `Resolved`'s sizes, the keys, and every
rectangle worked out from the registers stay in the title's pixels, and are
multiplied where they are handed to Vulkan: image extents, render areas,
viewport and scissor, and the regions of resolves, copies and blits. So the
frame is still "a colour resolve of 1280x720". The scale is lowered until
4096 of the title's pixels fit one of the device's images.

The scale is a whole number, so the title's scissors and resolve rectangles
land on pixel boundaries, and it applies to every surface, since the passes
read each other's at matching sizes. Four things are not a plain
multiplication:

- **Fetches of a resolve's copy.** The translated shader takes a texture's
  size from the bound image, for half-texel offsets and unnormalised
  coordinates, and a copy is larger than the surface the title means. Each
  draw pushes a bit per slot bound to a copy, with the reciprocal of the
  copy's scale, and a shader built under a scale divides those slots' sizes
  by it. A shader built without one has no such code ([shaders.md](shaders.md)).
- **Occlusion counts** are divided by the scale squared, each query's by its
  pass's: the title compares them with numbers of its own pixels.
- **Depth bias.** The slope term is multiplied by the scale, as Xenia does: a
  slope is depth per pixel, and the pixels are smaller.
- **A surface the title reads from memory** (Advanced Warfare). Its exposure
  comes from a 16x16 surface of one float, the scene's luminance, written
  back to the resolve's destination for the processor to read
  ([AW-BRINGUP.md](AW-BRINGUP.md)). Under a scale that surface is 32x32 or
  48x48, and each texel written back is the mean of the four or nine drawn
  for it. Without the write-back the title takes the scene for pitch dark:
  a match at 2x was a white wash on the AYN Thor and on Windows.

The launcher's RESOLUTION entry keeps the scale in `.env` as
`MW2_SCALE=<n>`, with the title's frame below
([switches.md](switches.md#the-settings-file)).

### The shadow maps' scale

`MW2_SHADOW_SCALE=2` to `4` draws the shadow maps at a scale of their own,
never below `MW2_SCALE`'s. The sun's map is 1024x2048, two cascades, and the
spots' 512x2048, as on the PC; a shadow's edge steps by a texel of them.

**Which passes** (`runtime/shadow_resolution.cpp`). `R_InitRenderTargets`
fills `gfxRenderTargets`, `{ image, colour surface, depth surface, width,
height }` a target; 11 and 12 are the two shadow maps. A D3D surface keeps
the registers D3D writes when it is bound: `RB_SURFACE_INFO` at +24 and
`RB_DEPTH_INFO` at +28. Both maps draw depth at tile 0 in D24FS8 at one
sample, and no other target the title made does; a pass with no depth
surface has `RB_DEPTH_INFO` 0, tile 0 in D24S8. So a pass whose depth
registers name that tile, format and sample count is drawn at the shadow
maps' scale (`PassScale`), its colour surface too.

**Keys.** A `TargetKey` and a `Resolved` carry the scale they were made at,
and a copy takes its source surface's. The spots' map has its colour surface
at the tile and pitch of the half-size particles' (tile 0, 560 wide), and
each pass gets its own image at its own scale: nothing reads one through the
other.

**Height.** The title also draws a stencil rectangle into the shadow surface
in depth-only mode, with the scissor at its 8192 reset value, which makes the
surface 4096 of its rows tall at 1x. A shadow pass's surface is held to the
height the title made the maps, 2048: at 4x the depth surface is then
4160x8192 rather than twice that.

The shaders depend only on whether either scale is past 1, so the
precompiled shaders' marker records that (`scaled yes`), not the scales.
The launcher's SHADOWS entry keeps it as `MW2_SHADOW_SCALE=<n>`.

## The title's frame and the picture's shape

`MW2_RENDER=<w>x<h>` (`runtime/render_size.cpp`) gives the title another frame
than the console's 1280x720, and so another shape: 960x600 is 16:10, and at
`MW2_SCALE=2` the window gets 1920x1200. The present images are the frame
times the scale; the window letterboxes them as before.

**Where the title takes its sizes.** Not from the video mode: `R_VideoSizes`
writes a 1024x600 scene and a 1280x720 frame whenever the mode is high
definition (896x576 and the mode's own size, clamped to 640x480-1280x720,
otherwise). `R_SetVidConfig` makes the `vidConfig` of them, with the
picture's aspect 16/9 or 4/3 by the widescreen flag alone, and the pixel aspects
`h * aspect / w` of the scene and the frame. Everything after reads that
`vidConfig`: D3D's back buffer and render targets, the views' projections,
the HUD's and the menus' placement, the cinematics' letterbox. It is the PC
game's engine, which draws at any shape. The hooks write the sizes after
`R_VideoSizes`, and after `R_SetVidConfig` the aspect, the frame's width over
its height, with square pixels. With the console's frame they do nothing.

**A 4:3 frame.** The video mode (`runtime/kernel/video.cpp`) says the TV is
widescreen only for a frame wider than 4:3, so a 4:3 frame is the console on a
4:3 TV: `R_SetVidConfig`'s aspect is already 4/3, `dvarbool(widescreen)` is 0
and the menus take their own 4:3 layout, 640 of their units across. The
launcher's 4:3 rows are 640x480, 800x600 and 1024x768 at 1x, and 1280x960 to
2880x2160 scaled.

**The sizes EDRAM allows** (`runtime/render_size.h`). EDRAM is 2048 tiles of
80x16 samples. The title puts a surface's depth at the top and its colour
below it: the console's scene, 2x and 1040 wide, is colour at tile 98 and depth
at 1073, 975 tiles each. So:

- the frame, drawn at 1x, fits when its colour and depth together take 2048
  tiles or fewer. The frames taken are 640x360 to 1280x800, a whole number of
  16 pixels wide and 4 tall.
- the scene, drawn at the console's 2x, is given no more tiles than the
  console's takes, 1950: what the title keeps beside it is not known. It is the
  frame itself when that fits, else the largest of the frame's shape that does,
  16 pixels narrower at a time: 960x600 for a 1280x800 frame. 1024x640, 2080
  tiles, would not fit.

The title's own count of samples stays 2, the console's: `MW2_MSAA` and
`MW2_NO_MSAA` act on the surfaces the title multisamples, and a title drawing
at 1x would leave them none.

**The scene by itself.** `MW2_SCENE=<w>x<h>` gives the title a smaller scene
under the frame it has: 512x300 up to the one that goes with the frame, a
whole number of 16 pixels wide and 2 tall. The title stretches the scene over
the frame as it does the console's 1024x600 over 1280x720, working the
scene's pixel aspect out of the two sizes itself, so the scene's shape is
free; the HUD and the menus are drawn into the frame and keep its size. It is
the cheaper picture for a GPU that the scene's pixels hold back: both titles
on an Adreno 6xx under Android start with 832x480
([android.md](android.md#adreno-6xx-snapdragon-865-class)). Modern Warfare 3
takes the scene through the same function (`R_VideoSizes`, found in both its
executables by its instructions); its frame keeps the console's size.

**Pictures made for 16:9.** A cinematic is letterboxed by the title itself:
`R_DrawCinematicLetterboxed` draws the 16:9 video whole, as wide as the frame,
between black bars. A 2D picture over the whole frame (a loading screen, a
menu's background, a full-screen overlay) would be stretched; it covers the
frame instead: the hook on `R_DrawStretchPic` makes it as large as the frame
and cuts what spills over, its sides on a 16:10 or 4:3 frame. It does so for a
picture whose every texture was loaded from a file
(`IMG_CATEGORY_LOAD_FROM_FILE`). A copy of the screen (a render target), which
the menus blur behind them, is drawn over the frame too, and is left whole.

The run's log names each material the first time it is cut ("a picture covers
the frame (mw2_main_background)").

**The menus' own width.** The menus place most things by their alignment,
which follows the `vidConfig`, but some lay out across the screen by
expression, taking its width in their units as `640 + 213.333 *
dvarbool(widescreen)`: 640 at 4:3, 853.33 at 16:9. The experience bar's
width is one. Before the evaluator (`UI_EvaluateStatement`) runs a
statement, a constant it multiplies `dvarbool(widescreen)` by, on either
side, is scaled by the share of the way from 4:3 to 16:9 the picture's shape
is: 0.6 at 16:10, so the width is 768. Each constant is scaled once; one a
zone loads again is scaled again. `widescreen` itself stays 1: the menus also
test it (the loading screen shows its bar `when( dvarbool(widescreen) )`), and
a test makes a whole number of what it is given.

### The scene drawn smaller under load

`MW2_SCENE_DYNAMIC=1` (`runtime/gpu/vulkan/renderer_scene.cpp`; unset, on
under Android, for every chip since 26 November 2026 and for an Adreno 6xx
alone before) draws the scene in fewer pixels while a frame
takes the graphics chip longer than the display gives it. `MW2_SCENE` cannot
do that: the title makes its surfaces once. So the surfaces keep the title's
size and the renderer draws into a part of them, from the first pixel:

- **Which surfaces.** Those as wide in EDRAM as the scene (its width rounded
  up to 80 samples: 880 for 832x480, where the frame's are 1280), drawn at
  one sample and at the title's own size (`MW2_SCALE` off), of a format the
  device can stretch (a blit; filtered, for colour). A pass on one has its
  viewport and scissor taken to the part (`ToDrawnScene`), its depth
  surface's with it; a surface remembers the part its contents were last
  drawn in.
- **A resolve out of one** stretches the part over the rectangle the title
  names, filtered for colour and as it is for depth, instead of copying
  (`SceneBlit`). The copy the title reads back is then the size it expects,
  so the passes after the scene -- the depth of field's and the bloom's
  smaller buffers, the stretch over the frame, soft particles reading the
  depth -- know nothing of it. A rectangle that reaches the scene's edge
  stops a pixel short of the part's: the filter reads a pixel past where it
  is told to, and what lies past the part is an older frame's.
- **The frame**, with the HUD and the menus, is another surface and keeps
  its pixels.
- **Occlusion counts** of a pass drawn smaller are divided by the share of
  the title's pixels it drew, as `MW2_SCALE`'s are by the scale squared.
- **The steps** are 92, 84, 76, 70, 64 and 58% of the scene's width and
  height (85, 71, 58, 49, 41 and 34% of its pixels; the last two since every
  chip has them, for one that starts at the console's 1024x600 and is
  weaker than the Adreno 650 whose own scene starts at 832x480). Where the
  sun's shadows are the runtime's to choose and on, the first step takes
  them instead and the scene waits twelve frames for what that gives
  (`adaptive::GiveUpShadows`, [android.md](android.md#what-a-device-is-given-and-what-it-is-measured-for)).
  Once a window of ten frames the frames'
  time on the chip (its own clock, `gpu_time.h`, which is therefore on
  whether `MW2_REPORT` is or not) is set against what the display gives a
  frame: a step down when they took 97% of it, or 90% of it reckoned at the
  chip's fastest clock (an Adreno's governor lowers the clock while there
  is time in hand, which is no reason to draw less; the kernel says the
  clock in `/sys/class/kgsl/kgsl-3d0/gpuclk`), two steps when they took 112%;
  a step up when for a second and a half the step above would have fitted
  in 82% of it, taking half of a frame's time for its pixels.
- **What the picture loses** is what a smaller `MW2_SCENE` loses, for the
  seconds the chip is late and no others: at the whole size nothing is
  changed, the copies are copies. Two frames of one view of Modern Warfare
  2's Gulag with the game's clock all but stopped
  (`MW2_SCENE_SHARE=70,2`, `timescale 0.001`, `MW2_DUMP_FRAMES`): the mean
  difference of a channel between the whole scene and 70% of its width is
  5.1 to 5.5 of 255, between two frames at one size 2.5 to 3.5 (the weapon
  sways and the smoke drifts at that clock too).

The log says at start-up what the scene may be drawn down to and at exit
what share of the world's frames had each size; `MW2_REPORT_SECONDS` says it
a second ([android.md](android.md#the-campaigns-slow-places-24-november-2026)).

## Detail at distance

`MW2_LOD_BIAS=<1 to 8>` (`runtime/model_detail.cpp`, Modern Warfare 2's disc
executables) has a model change to its coarser meshes that many times nearer
than on the console. The app's Detail at distance setting gives 1 (as on the
console), 2 (reduced) or 4 (low); unset it is 1 on every device. Whether an
Adreno 6xx should start at 2 waits for a measurement of S.S.D.D. there.

- **The title's own settings do nothing.** `R_RegisterDvars` (`sub_823A2F98`
  in the campaign, `sub_823DB2F8` in the multiplayer) registers the PC
  game's `r_highLodDist`, `r_mediumLodDist`, `r_lowLodDist`,
  `r_lowestLodDist` and `r_forceLod`, all cheats (flag 4), and
  `sub_82392EE0` hands their values to `XModelSetTestLods`
  (`sub_82300BC8`), which in the console's build is a lone `blr`.
  `r_forceLod lowest` in a level started with `devmap` draws the same
  7.85 billion indices over the same views as without it. The PC's
  `r_lodScaleRigid`, `r_lodBiasRigid` and their skinned pair are not in
  either executable.
- **Where it is done.** `XModelGetLodForDist` (`sub_82300B48`, `sub_8232D650`
  in the multiplayer; a leaf with no unwind entry) takes a model and two
  distances and walks the model's meshes, each with the distance it is drawn
  within: the first distance picks the mesh, the second only says whether a
  model past its last mesh's distance is drawn at all. Its eight callers are
  the renderer's (`sub_822F0358`, which fills in a mesh for each model of
  an object, and the scene's adders); they pass the distance to the
  viewpoint scaled for the field of view. The hook multiplies the first
  distance and leaves the second, so nothing disappears sooner than on the
  console, a player sees of another what every other device shows, and
  what a host or a client has set makes no difference to the other.
- **What it gives** is the vertices of soldiers, vehicles and props, not of
  the world's own surfaces, which have no coarser form. On the AYN Thor the
  opening of S.S.D.D., rows of soldiers, goes from 101.9 frames a second to
  111.2 at 2, 116.0 at 4 and 116.9 at 8 (8.0, 6.1, 5.3 and 5.2 billion
  indices over the same 44 seconds), and its flight from 112.2 to 115.8 at
  2. Team Player's heaviest views on the Adreno 650 are the city's own
  surfaces: at 8, with 22% fewer indices, they stay within 3 frames a
  second of the 35 to 39 they were.
- **The picture.** At 2 the difference from the console's is of the order
  of two screenshots of one setting: over six fixed views of S.S.D.D. the
  mean difference is 1.1 to 2.7 of 255 (6.3 where the soldiers stand,
  whose poses differ from run to run) against 0.7 to 2.7 between two runs
  at 1. At 4 distant wire, railings and foliage are visibly simpler.

## Multisampling

The title's 2x and 4x surfaces are drawn with that many samples. The count is in
the target, pipeline and output-library keys, rounded up to one the device
supports for colour, depth and stencil. Multisampling needs Vulkan 1.2 and a
`SAMPLE_ZERO` depth resolve. Without them, or under `MW2_NO_MSAA=1`, everything
is 1x. `MW2_MSAA=<n>` forces n.

A multisampled image cannot be blitted, copied or sampled, so every read of
one is a resolve. Colour goes through `vkCmdResolveImage`, which averages as
the console does. Depth goes through an empty rendering instance with
`SAMPLE_ZERO`, because an averaged depth describes a surface nothing was drawn
at. Both resolve straight into the resolve's image, which for depth must be an
attachment as well. The rendering instance writes where the rectangle sits in
its surface. So a depth resolve goes through a single-sampled **twin** of the
surface, and is copied from there, in two cases: when it lands lower in its
image, and when the image is not yet an attachment but already holds an
earlier resolve of the frame. The target dumps use the twin too. Sample
positions are Vulkan's standard ones.

## Anisotropic filtering

A fetch constant carries the anisotropy the title asks for, and the sampler
takes it, up to the device's `maxSamplerAnisotropy`; a fetch whose LOD does not
come from derivatives filters along none, as the hardware does. The title
picks it from two settings of the PC game's that the console keeps:
`r_texFilterAnisoMax`, 16, and `r_texFilterAnisoMin`, 1, between which each
material's sampler state falls. On the console the materials ask for 1:1 to
4:1, and samplers that filter without mips (the menus, the HUD) for none.

`MW2_ANISOTROPY=<n>` (`runtime/texture_filtering.cpp`) is the PC's ANISOTROPIC
FILTERING slider: `Dvar_RegisterInt` registers `r_texFilterAnisoMin` with n as
its default. The setting has no flags, so it is no cheat, and nothing the title
runs sets it; with 16 every mipmapped sampler of a level asks for 16:1, and the
menus' stay at none.

## Submissions, segments and frame slots

There are three frame slots, each with a command pool, a fence, an arena share
and a submission serial. `BeginFrame` moves to the next slot and waits only if
the GPU still holds it. A submission is normally one frame.

A submission is cut into **segments** at each completion point
([d3d9-seam.md](d3d9-seam.md#completion-points)). Once the title learns that
draws have finished, it may reuse the memory they read, which the console's GPU
had already read. So `BeforeCompletion` closes the segment:
`vk::textures::EndSegment` re-reads the segment's watched textures, and the
draws end in a command buffer of their own. Each segment is submitted as its
uploads followed by its draws. `BeforeStreamWrite` also closes a segment before
the image pool's copy overwrites memory the segment binds.

Before an image a submission may use is destroyed (a target that grows, or a
resolve image that is replaced), `RetireBeforeDestroy` submits and waits,
drains the recorder, and idles the device. Otherwise a command buffer points at
freed memory and the GPU page-faults.

## The recorder thread

`vk::record` queues each Vulkan call as a lambda holding a copy of its
arguments, in a byte ring. The recorder thread makes the calls in order,
including submissions and the frame's hand-off to the window. The swap is
parsed on the consumer, so the title's frame retires at parse time.

The recorder works in bursts. It sleeps while less than 64 KB is queued and
none of it is due, and is woken when that much is waiting, at once for a
submission or a hand-off, when a thread waits for it, and after a millisecond
in which the consumer had nothing to read. Calls come every couple of
microseconds and the recorder is faster than that, so a recorder that spun
until the next one never slept, and held a core; one that waited on the queue
position itself woke at every call, because the wait spins first.

Draws are made in runs. On the Adreno 740 the GPU pays about 5 us for every
hardware draw whatever its size, and each entry of a
`vkCmdDrawMultiIndexedEXT` is one, which is most of a frame of 6,600 draws.
So a draw with indices that changes no state since the last one is held back,
and the run is made as one `vkCmdDrawIndexed`: the first index of the first
draw, the count of them all, and the draw table as the base vertex. The
consumer says whether a draw joins the one before it (`DrawOp::joins`): it
changes no state, its indices follow that draw's in the index stream for the
same table, and that draw was a list of whole primitives (a strip or a fan
would run on into the next draw's vertices). The recorder joins it if the run
is still open. A run ends at a draw that changes any state, and at any other
call: the recorder runs `MakeGatheredDraws` before every call not queued as
`Joining`. `MW2_NO_MERGE=1` makes every draw a hardware draw of its own: on
the Adreno 650, S.S.D.D.'s 2,900 draws a frame are about 210 hardware draws,
and each on its own took the frame from 24.7 ms to 35.2.

A segment of the frame -- the draws between two points where the command
processor reports work done -- has its texture uploads ahead of its draws,
so it begins a command buffer when it has any. Whether it has is known at
its end, so the boundary is a call that waits on the recorder for the
consumer to say: nothing uploaded, and the segment's draws go on in the same
buffer and rendering instance; something uploaded, and the buffer ends there
and the segment's calls, queued for it, go into the next
(`vk::record::Redirect`; `CloseSegment` in `renderer_frame.cpp` has the
rule). A thread about to wait for the recorder settles a pending boundary as
a split first (`vk::record::BeforeWaiting`). `MW2_JOIN_SEGMENTS=0` splits at
every boundary. With `MW2_TIME_PASSES` a split's uploads are counted in the
instance they interrupt.

Draws without indices have no indices to lay in a row. A run of them is one
`vkCmdDrawMultiEXT`, each with its own draw table entries as its first
vertex; without `VK_EXT_multi_draw` each is made on its own.

An indirect draw over commands the consumer writes into the arena does as much
for the recorder, but it puts twenty more bytes a draw on the consumer, which
is what four players wait on, and each of its commands is a hardware draw
still.

- A queued call must copy what it uses. It must never point at a local or read
  `g.`, which will have moved on by the time the call runs.
- A slot's fence means nothing until the recorder has submitted to it.
  `WaitForSlot` first waits for the recorder to reach `slot.queuedAt`.
- Anything that destroys what a frame may use, or that submits on its own,
  calls `vk::record::Drain` first.

`MW2_RECORD_THREAD=0` (diagnostics builds) makes the calls on the consumer.

## Presenting

The swap waits (`vk::WaitUntilTaken`) until the window has copied out
the present image it is about to reuse. With four present images, the renderer
can run three frames ahead. The window presents every frame once, in order,
one per blank (`FIFO`). Frames the window has not taken within 250 ms are
dropped, so a hidden window never stalls the game. The window is resizable and
letterboxed, or stretched to fill it under `MW2_STRETCH=1`. `F8` or
`MW2_FULLSCREEN=1` switches to borderless fullscreen.

**The guest's blank follows the display.** On the console the blank the title
counts is the display's own. `VK_KHR_present_wait` timestamps each present;
where a device has none, Android's swapchain says when each present reached
the screen a few frames afterwards (`VK_GOOGLE_display_timing`, over any
driver, Turnip's too; every present is given an id, since the window keeps
the times of its last eight buffers only).
The display period comes from the mode's exact rate, or from a slow average of
single-frame gaps. For a 59-61 Hz display, `vk::GuestBlankPeriod` returns that
period, 0.5% longer while frames wait: more than 1.5 behind the one on screen
with present wait, a frame on screen more than two blanks after the renderer
finished it with the swapchain's times (one blank for the compositor and one
for the display is what an empty queue costs). So latency drains unseen. It is
never shorter. It returns 0, and the vblank thread keeps its own 60 Hz, when
the display runs at another rate, neither extension is there, nothing has
been shown for half a second, or under `MW2_FOLLOW_DISPLAY=0`.

Without it both phones ran the guest's blank on a timer beside the display,
and whenever the timer was the faster the renderer ran the three frames ahead
that its present images allow. At the console's half rate (a limit of 30) it
always did: the title made its frames faster than the window showed them, two
blanks each, and the window's taking them was what held it to 30. The frame
there now waits until the window has taken the one before it
(`vk::renderer::Swap`), as under the display's pacing. On screen after the
renderer finished a frame (`MW2_REPORT`, `in play, a frame was presented ...`;
25 November 2026, F.N.G. and S.S.D.D.):

| Limit | Retroid Pocket Flip 2, 60 Hz: before | after | AYN Thor, 120 Hz: after |
|---|---|---|---|
| 30 | 158 ms (116 ms until the window presented it) | 58 ms (16 ms) | 42 ms (8 ms) |
| 45 | 22.5 ms | 22.5 ms | 15.5 to 17 ms |
| 60 | 48 ms with the timer the faster, 27 ms with it the slower | 30 to 32 ms | 15.5 to 17.5 ms |

At 30 a frame still waits a blank behind the second showing of the one
before it, which the window presents as soon as the first.

### More than 60 frames a second

`MW2_FPS_LIMIT` (`runtime/frame_rate.h`) at anything but 60 takes the title off
the blank. Two things hold it at 60, and each is released its own way:

- **D3D's present interval.** The title's present puts the device's
  `D3DRS_PRESENTINTERVAL` in the flip request, and D3D's flip handler queues
  the frame for the next blank. `T_D3D_Present`'s hook sets the state to
  `D3DPRESENT_INTERVAL_IMMEDIATE` first, and the handler then retires the frame
  when the command processor reaches the swap
  ([d3d9-seam.md](d3d9-seam.md#the-flip-handshake)).
- **The title's limiter.** `Com_Frame` sleeps until `1000 / com_maxfps` whole
  milliseconds have passed since the last frame; `com_maxfps` is 60 unless
  set. The hook queues `com_maxfps` on the title's console, and
  `r_vsync 0`: with `r_vsync` on, the title takes a frame's length from the
  blanks it counted between swaps instead of from its clock.

What paces the frames then depends on the display, and the log says which in
one line (`pacing: MW2_FPS_LIMIT 120 on a display at 120.00 Hz, a blank every
8333.3 us -- ...`), again whenever the display's mode changes:

- **The display** (`frame_rate::kByDisplay`), when the limit is the display's
  refresh rate to within 3%: 120 on a 120 Hz panel, whose real rate may be
  119.88 or 118.4. The limiter is off (`com_maxfps 0`): counting whole
  milliseconds it would let 125 frames through against 120 shown, five a
  second dropped. Instead the swap waits (`vk::WaitForDisplay`) until the
  window has taken the frame before the one it is finishing. The window shows
  every frame, takes one a blank (`FIFO`), and so the renderer and the title
  behind it run at the display's rate with one frame waiting at most; a frame
  that takes too long simply stays two blanks. A swapchain that does not wait
  for the blank (a desktop driver set so showed 575 frames a second on a
  240 Hz display) is held by a clock instead: a frame is never finished
  sooner than 98% of a period after the one before, the 2% so that this
  clock never paces where the display does.
- **The title's limiter** (`frame_rate::kByLimiter`), for any other limit:
  `com_maxfps <limit>`, or 0 for none. The window still presents one frame a
  blank, without tearing, but the newest: frames finished before it and not
  yet shown are let go. Nothing waits behind the frame on screen. Call of
  Duty 4's limiter does not hold a limit: its `Com_Frame` keeps three clocks,
  one for every third frame, and each waits `1000 / com_maxfps` ms for its
  own turn, so a limit of 45 let 136 frames a second through. There the
  title's present returns no sooner than the limit's period after the one
  before (`HoldToLimit`, `d3d_hooks.cpp`), `com_maxfps 0`: 45.0 a second,
  set at the start or in the game's menu.

The guest's blank keeps its 60 Hz in both, and `VdQueryVideoMode` its 60:
with `r_vsync 0` nothing times a frame by blanks. The simulation's speed does
not change: a route walked at 93 frames a second takes the time it takes at
60.

The display's rate is its mode's (`frame_rate::DisplayHz`), which chooses the
pacing; its period is measured from the presents reaching the screen where
the mode does not give it exactly, and logged once it has settled (`vulkan:
the display's blank measures ...`). On Android the surface votes for the
limit as the display's rate (`ANativeWindow_setFrameRate`, Android 11 on),
beside the window's preferred mode, and the app's display listener passes
every mode change on: the system may refuse 120 or take it back for the
battery or the heat, and the pacing follows.

`MW2_STUTTERS` and the report count against the display's blank at any rate:
a frame is due every blank, or every second one where the limit is half the
display's rate. The report's `in play, N% of frames came within 1.05 display
periods of the one before, M% within 2.05` is the line a 120 Hz run is judged
by: N is the share of frames that kept the display's rate, 100 - M the share
that missed two blanks.

What keeps a scene under the display's rate is not always the renderer.
Advanced Warfare's Fusion on the AYN Thor missed 120 with the title's own
threads presenting late: half of its recompiled functions kept every
register in the context for one bit of XER
([building.md](building.md#local-changes-to-xenonrecomp)). With the bit
left in the context (November 2026; the game code of all four games is
built again for it, once), old code against new under one app, each scene
scripted the same way both times:

| AYN Thor, 120 Hz | FPS | On time | Stutters | of them the title late |
|---|---|---|---|---|
| Advanced Warfare, Fusion, seven minutes, before | 115.6 | 82.9% | 356 | 230 |
| after | 118.8 | 93.1% | 95 | 51 |
| Advanced Warfare, Retreat with six bots, before | 119.0 | 94.5% | 24 | 13 |
| after | 119.0 | 94.1% | 9 | 6 |
| Modern Warfare 3, Black Tuesday, before | 118.1 | 92.1% | 88 | 51 |
| after | 118.9 | 93.6% | 31 | 7 |
| Modern Warfare 3, a match with six bots, before | 118.6 | 94.5% | 69 | 46 |
| after | 119.0 | 95.1% | 30 | 18 |

On the Retroid Pocket Flip 2 at 60 the rate was held before and is held
after: Black Tuesday 59.9 and 59.8 frames a second with 80.8% and 80.5% of
frames on time before, 59.8 twice with 87.0% and 85.6% after; the match
60.0 with 78.5% and 86.7%; Advanced Warfare's Seoul and its Retreat match
60.0 both ways. Modern Warfare 2's and Call of Duty 4's recompiled code is
the same byte for byte, and what their runs differ by is what two runs of
one build differ by (Thor: S.S.D.D. 116.6 and 117.8, a match on Rust 119.0
and 118.9, Charlie Don't Surf 114.4 and 116.0, a match on Crash 119.1 and
119.0).

### Sharpening on the way to the window

`MW2_SHARPEN=<1 to 100>` (`sharpen.cpp`) draws the frame into the window
through a filter in place of the linear copy: FidelityFX Super Resolution
1.0's RCAS, five taps a window pixel apart read through the linear sampler,
one pass over the frame's place in the swapchain image (the bars around it are
the pass's clear). The number is the strength in percent of the filter's most.
Unset or 0 is the copy, as before. The picture has been enlarged twice by
then, the scene to the frame by the title and the frame to the window here,
both linearly, and the filter gives some of that back. Where a device has no
dynamic rendering the frame is copied, and the log says so.

The Android app's Sharpening setting (Graphics) sets it, off by default, and
makes the surface the screen's own size while it is on: the app otherwise
draws into a surface of 70% of the screen and lets the compositor enlarge
that, which would soften the sharpened edges again.

The two shaders are GLSL (`shaders/sharpen.vert`, `.frag`), where the
runtime's other SPIR-V is written by `spirv.h`: the fragment shader is a
published filter kept as its authors wrote it. `tools/gen_sharpen_shaders.py`
compiles them with `glslc` into `shaders/sharpen_shaders.inc`, which is
committed, so no build needs a GLSL compiler; `--check` says whether the file
is what the shaders compile to.

**What it costs**, measured 2026-11-24 with `MW2_REPORT=1` (the GPU's time in
the window's submissions over the frames of a run), Modern Warfare 2, Team
Player's three heavy views, a 1920x1080 window, Turnip v27.0.0-R2:

| the frame's way to the window | Retroid Pocket Flip 2 (Adreno 650), GPU ms a frame | frames a second at the views, on time | AYN Thor (Adreno 740), GPU ms a frame | frames a second in play |
|---|---|---|---|---|
| linear copy into a 70% surface, enlarged by the compositor (the app's default) | 0.19 | 58.8, 77.8% | 0.18 | 83.9 |
| linear copy into a full-size surface | 0.32 | 58.6, 79.3% | 0.35 | 81.7 |
| RCAS, `MW2_SHARPEN=50` | 0.54 | 59.0, 75.3% | 0.66 | 81.0 |
| Snapdragon Game Super Resolution 1 (not kept) | 0.58 | 58.0, 73.7% | 0.64 | 79.0 |
| FSR 1: EASU, then RCAS (not kept) | 1.46 | 58.0, 68.9% | 1.15 | 79.1 |

One run each. The Flip 2 is at its cap of 60 there with any of them; the Thor
is bound by its GPU in those views and gives up three frames a second in 84
for the sharpening, which is why it is off until the player asks.

**What it gains**, against a picture whose truth is known. The three views
drawn at twice the size (`MW2_SCALE=2`) and photographed at 1920x1080 are the
truth; that picture was taken down to the scene's size and enlarged linearly
to the 1280x720 frame as the title does, brought to 1920x1080 by each filter
(the shaders rewritten for the PC, operation for operation), and compared:

| | 832x480 scene (Adreno 6xx): PSNR, SSIM | 1024x600 scene: PSNR, SSIM |
|---|---|---|
| linear copy into a 70% surface, enlarged again | 27.75 dB, 0.796 | 28.66 dB, 0.834 |
| linear copy into a full-size surface | 28.23 dB, 0.817 | 29.44 dB, 0.862 |
| RCAS at 50% | 28.59 dB, 0.833 | 30.03 dB, 0.882 |
| RCAS at 100% | 29.59 dB, 0.873 | 31.59 dB, 0.925 |
| Snapdragon Game Super Resolution 1 | 29.40 dB, 0.854 | 31.00 dB, 0.900 |
| FSR 1 (EASU, RCAS at 100%) | 29.52 dB, 0.870 | 31.07 dB, 0.914 |

The measure flatters all of them in one respect: the picture taken down is
smooth at its edges, and the title's scene, drawn without multisampling on a
phone, is not. On the devices every filter also makes the scene's stair-stepped
edges and the grain of smoke crisper, Snapdragon GSR most (its picture looks
painted at 720 to 1080), RCAS least at the same strength. Text and the HUD,
which are drawn at the frame's size, gain most.

### Not built, and built and not kept (November 2026)

What the title gives a filter is a finished 1280x720 picture: the HUD is
drawn into the same surface as the world, there are no motion vectors, and
the depth of a frame is in another surface the window never sees. What each
technique would need, and what was measured of the ones tried:

| technique | needs | licence | here |
|---|---|---|---|
| [Snapdragon GSR 1](https://github.com/SnapdragonStudios/snapdragon-gsr) (spatial, one pass, twelve taps of luminance) | the picture | BSD-3-Clause | tried: as much GPU as RCAS, a lower SSIM in both tables above and the harsher picture; not kept |
| [FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) EASU + RCAS (spatial, two passes) | the picture | MIT | tried: two to two and a half times RCAS's GPU for the same PSNR; EASU not kept, RCAS is the sharpening above |
| FidelityFX CAS | the picture | MIT | not built: RCAS is its successor in FSR 1 and the same five taps |
| [Snapdragon GSR 2](https://github.com/SnapdragonStudios/snapdragon-gsr/tree/main/sgsr/v2), FSR 2 and 3 upscaling, [Arm ASR](https://github.com/arm/accuracy-super-resolution) (temporal) | the scene before the HUD, its depth, motion vectors (the camera's can be made from depth and the two frames' matrices; a moving object's cannot), and the projection moved by a part of a pixel every frame | BSD-3-Clause, MIT, MIT | not built: the title's view matrices would have to be found and jittered in four executables, and the scene taken before its own stretch to the frame |
| TAA, TAAU | the same | | not built, for the same reason |
| Variable-rate shading (`VK_KHR_fragment_shading_rate`) | the extension, which Turnip has from the Adreno 7xx on ([Mesa's features list](https://docs.mesa3d.org/features.html): `tu/a7xx+`), so not on the Flip 2 | | not built: it saves pixels, and on the one device that has it a scene of two thirds the pixels changed nothing ([android.md](android.md#adreno-7xx-snapdragon-8-gen-2-class): 106.2 against 107.1 frames a second) |
| FSR 3 frame generation | two frames, depth, motion vectors, and the HUD apart (its own advice); written for Direct3D 12 compute shaders | MIT | not built |
| Qualcomm's AFME | on the phone's own driver, [`GL_QCOM_frame_extrapolation`](https://registry.khronos.org/OpenGL/extensions/QCOM/QCOM_frame_extrapolation.txt) makes a frame from two textures in OpenGL ES; the Thor's driver lists it and `GL_QCOM_motion_estimation`, the Flip 2's the second only. Neither is in Vulkan or in Turnip; the system-wide "AFME 2.0" is a phone maker's feature and not an app's | none: part of the driver | not built: it would need an OpenGL ES context on Qualcomm's driver beside the Vulkan one on Turnip and the frames shared between them |
| A frame from the last one turned by the camera's motion (reprojection) | a view newer than the frame's, which the title has not got: it reads the pad once a frame | | not built |
| Frame generation from two frames alone | two frames; it holds each frame back by a blank of the display | | built and measured, below; not kept |

**Frame generation as tried.** Where a frame is due on screen for two of the
display's blanks (a limit of 60 on a 120 Hz display, 30 on a 60 Hz one), the
window showed, for the first of them, a frame made halfway between the frame
before and this one. The motion was searched for in the two pictures: from a
place in the frame between, the displacement that makes the last frame one
way and this frame the other way alike, down four levels of their luminance
from 80x45 to 640x360, nine taps a candidate, a median of nine after each
level. The frame between is the two read half the motion away and averaged; a
pixel that did not change is left alone, which keeps a HUD over an unmoving
world. (The branch `nextgen-gfx-proto` has it, `MW2_FRAME_GEN`.)

| | AYN Thor, 60 to 120 | Retroid Pocket Flip 2, 30 to 60 |
|---|---|---|
| GPU a frame of the title's, without and with | 0.29 ms, 2.06 ms | 0.94 ms, 4.34 ms |
| a frame's present after the renderer finished it, without and with | 2.0 ms, 20.5 ms | 115.5 ms, 132.7 ms |
| the frame between against the frame the game really drew there (PSNR) | 31.0 dB | 26.3 dB |
| the same place shown the frame before, as now | 23.8 dB | 19.7 dB |
| the plain average of the two frames | 26.9 dB | 22.3 dB |
| OpenCV's DIS optical flow on a PC in the search's place | 32.5 dB | 27.6 dB |

The pictures' rows are from a recording of a walk through Terminal at 120
frames a second on the Thor: every second frame left out, made from its
neighbours by the same shaders rewritten for the PC, and compared with the
one left out (40 frames each; for 30 to 60, every second pair). It is not
kept, for three reasons. The latency: a frame reaches the screen 17 to 18 ms
later, where the scheme itself asks for one blank (8.3 ms on the Thor). The HUD: it is in the
picture, so where the world moves behind it the mini-map smears, and digits
of the clock and the score were missing from a generated frame; a
generated frame is only as good as the search, and the search knows no HUD
from a wall. And the need: Modern Warfare 2 and 3 already draw 115 to 118
real frames a second on the Thor and 58 to 60 on the Flip 2, so the frames
it would add stand where real ones are now. What would change the verdict is
the frame taken before the HUD is drawn, and the motion from the title's own
matrices and depth instead of a search.

## Occlusion queries

The title brackets counted draws with two `EVENT_WRITE_ZPD` events, with the
record address in `RB_SAMPLE_COUNT_ADDR` (`0x2325`). A query owns 64 bytes: the
end record at the base and the begin record at +32. A record is eight
**little-endian** dwords (D3D reads them with `lwbrx`):

    +0 total  +8 z-fail  +16 z-pass  +24 stencil-fail     (lanes A and B)

The title computes the end record's z-pass minus the begin record's. D3D
writes `FFFFFEED` into the z-pass lanes and polls until they change, so **every
end record must eventually be written**, or the title hangs.

- Begin writes zeros at once and resets 64 queries of a 256-query ring.
- A Vulkan query cannot span rendering instances, so the bracket opens one in
  each instance it covers and sums them.
- End answers 0 at once if nothing was covered. Otherwise the result is
  written by `CollectOcclusionQueries` (in the consumer loop and during
  `WAIT_REG_MEM` polls) once the GPU has finished. Results go out in the order
  the title closed the brackets, saturated at `0x7FFFFFFF`.
- A count is read once its submission's fence has signalled and is never
  waited for in the driver (`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, not
  `WAIT_BIT`). A query still unavailable then counts nothing. Qualcomm's
  driver on an Adreno 740 leaves one or two such queries in a first visit
  (pipelines still compiling), and the wait for one never returned: the
  consumer sat in `vkGetQueryPoolResults`, Call of Duty 4's back end polled
  the sun's count (`sub_82108830`) and its main thread waited for the back
  end, a still picture for the rest of the run (The Bog on an AYN Thor).

## Pipelines

`PipelineKey` holds the two shader hashes, the attachment formats, the
topology, `RB_BLENDCONTROL0`, `RB_COLOR_MASK` and the sample count. Culling and
the depth and stencil tests are dynamic state. Dynamic rendering and extended
dynamic state are required, and so are what the module interface is built on
([shaders.md](shaders.md#the-module-interface)): descriptor indexing with
update-after-bind and partially bound sampled images, room for 65,536 of them
and 4,096 samplers in one set (counted, with set 2's two storage buffers and
the colour target, against each per-stage, per-set and pool limit as the
specification sums it), shader
draw parameters, non-uniform indexing of sampled images and seventeen
interpolators. A device without one says which at the start. Subgroup ballot
in the fragment stage is used where there is one; a device without it reads
a draw's record a pixel at a time
([shaders.md](shaders.md#the-module-interface)).
`VK_EXT_multi_draw` is used where there is one.

On the console a shader is ready as soon as it is loaded. With
`VK_EXT_graphics_pipeline_library` the runtime compiles it at that point too
(`renderer_libraries.cpp`, `runtime/shader_preload.cpp`):

- `T_D3D_InitPixelShader` and `T_D3D_PatchVertexFetches` are hooked. The
  shader object's header (magic `0x102A11xx`) is at +40 (pixel) or +872
  (vertex). Its seventh word locates `{offset in program, byte length}`, which
  gives exactly the bytes the stream later `IM_LOAD`s. Workers compile each
  shader as a pre-rasterisation or fragment-shader library, built by
  `shader_build.cpp`, which the launcher's compile pass shares.
- A vertex shader is handed over once D3D has bound it, not when it is made.
  Its fetches name no format until the title binds it to a vertex declaration
  (`IDirect3DVertexShader9::Bind`), and D3D rewrites them in place: fetch
  constant, format, offset and stride, with neighbours on one stream merged
  into mini-fetches. The shader as made is never drawn.
- **A fragment library must carry the depth format in its own rendering info.**
  Without it, the library legally ignores depth and stencil state, and every
  draw comes out empty.
- State libraries are made per topology (vertex input) and per blend, mask,
  formats and samples (output). A draw fast-links the four. An idle-priority
  thread re-links them with link-time optimisation and swaps the result in.
- Workers pause during play, defined as 30 consecutive world frames, the last
  under 500 ms ago. A world frame has at least 8 colour draws into a
  multisampled target, or 200 into any target. Shaders not seen at load are
  compiled at their first draw.

Without pipeline libraries, or under `MW2_NO_PIPELINE_LIBRARIES=1`, whole
pipelines are built at their first draw.

Built there, a pipeline holds the frame up for as long as the driver takes
over it: up to 46 ms each on Turnip, and some 510 of them the first time a
map is played. `MW2_ASYNC_PIPELINES=1` is the option of not waiting, which
the Android app turns on by default (unset on the desktop): threads of their
own, at normal priority on the performance cores, build the pipeline instead
(`PipelineWhenReady`), and the draws that need it are skipped until it is in
the map, so what they draw appears late. A skipped draw queues no command
and changes none of the bound state, and the indices it wrote keep the next
draw from joining the run before it.

A draw that writes no colour is never skipped: its pipeline is built at the
draw. What such a draw leaves out is not a surface that comes late. It is
every shadow of the sun for a frame, or the depth the models after it are
drawn against, and those models with it: a frame checker found single
frames with a fuel tank, half a wall and all the shadows gone in Modern
Warfare 2's Rust, each at a second where a shadow caster's pipeline was new
only by the pixel shader left loaded before it.

Nor is a draw skipped the moment its pipeline is found missing. An object
already on screen that changes its shaders, as the held weapon does when
its lighting changes, was gone for the frame its new pipeline compiled in:
the weapon blinked. The consumer now stands still for the workers
(`WaitForPipeline`) within an allowance: 32 ms at most
(`MW2_PIPELINE_WAIT_MS`), which comes back a millisecond a frame and is
drawn on only when half of it is there. A pipeline alone, as in play, is
waited for and its draw made in its own frame, a frame late at worst. Of a
first visit's hundreds the first is waited for and the rest are skipped as
before, with a wait of 16 ms every sixteen frames or so.

A known case the allowance does not cover: no wait is begun while more
pipelines are awaited than there are workers, so a held weapon that needs
more new pipelines at once than that still goes for a frame. On an AYN Thor
with a fresh install (empty caches), Solar with the sun's shadows on, the
weapon and arms were gone for one frame at 134.7 s but for a sight and a
magazine: seven pipelines for them were found missing together (six
workers), each 30 to 57 ms to build. The run at `c41a440` showed it once in
three runs of that scenario. The run at `7c8a5f1` had the same path and as
many skips, 68,660 draws against 80,045. It is kept as a labelled recording
in the visual QA set (`runs/thor_aw-shadows_viewmodel.mp4`,
[visualqa.md](visualqa.md#where-the-suite-stands)). Changing the rule
(waiting for an object already on screen even in a crowd, say) needs a
first visit's frame times measured in every title.

The consumer builds none of the others itself. It did once a draw had been
skipped for 20 frames, one pipeline a frame, behind a single worker
compiling the same ones: a first visit's hundreds were then 25 to 35 ms of
every frame, 193 of an AYN Thor's 201 logged stutters in one multiplayer
match. Now there are `CompileThreads` workers (all but one of the
performance cores, three at most; `MW2_COMPILE_THREADS=<n>`), and Turnip is
given several creations at once (`vk::pipeline::OneCreation`,
`MW2_ONE_PIPELINE_CREATION`; Mali and Qualcomm's driver still one). A first
Seatown match on the Thor with empty caches: 400 pipelines compiled, none of
the renderer's time, the longest frame 68 ms where it was 463, and the
longest wait for a surface 311 frames (2.6 s, at the first spawn) where one
worker and the consumer took 124 frames with 4,967 ms of the renderer's.
The run's log counts the pipelines built in the background, the threads,
the draws skipped and the longest wait in frames.

The warm-up worker rebuilds a previous run's recorded pipelines, on the
little cores. One the driver has in its cache costs a millisecond and is
followed by no pause (it was paced like a compile, 200 ms apart in play: a
multiplayer's 1,500 took five minutes); `warmup: N recorded pipelines ready
in X ms, M of them compiled by the driver` says when they are all there.
None of these threads lowers its priority: each takes locks the consumer
takes, and a lowered thread held back with one held the consumer.

While no world is drawn -- a menu, a loading screen -- the draws' threads
take the warm-up's queue as well, without its pauses. It matters when the
list is there and the driver's cache is not (another driver chosen, a cache
thrown away, a driver that keeps none): on a Retroid Pocket Flip 2 with
Modern Warfare 3's 438 recorded pipelines and no driver cache, the 413 the
start does not build itself were ready 5.7 s after the start where the one
thread on a little core took 39.9 s (`p5_*` logs). A match reached later than
that had no surface waiting either way (the longest wait 2 frames, the spawn
75 s after the start). A map never played before has nothing recorded: its
first visit is as it was, the surfaces there within about three seconds.

What a run records and compiles is written by a thread of its own
(`saver`, every 10 s while something is new), when the activity pauses,
when its surface goes and when memory is trimmed
(`vk::renderer::SaveCaches`): the pipelines' list and the driver's cache
were written by the consumer in the middle of a frame, every 25 new
pipelines, and otherwise only at a clean end. A game killed any way has
lost ten seconds of compiling at most.

### Every shader compiled before play

The workers compile a level's shaders as it loads, but a driver with a cold
cache still takes tens of milliseconds a shader, and a level has thousands. So
a player's copy is played only with every shader of the game already in the
pipeline cache it starts with, `shaders/pipelines.bin`. The launcher fills it
(`launcher/precompile.cpp`, `mw2-launcher --compile-shaders` from a terminal):

- **The list.** `launcher/zone/` walks the fastfiles in `game/` the way the
  game's loader streams it, using the OpenAssetTools zone definitions in
  `launcher/zone/defs/`. Every pixel shader is listed as it is loaded. Every
  vertex shader is listed as the title binds it: the pass's declaration, the
  title's declaration tables (`sub_823EA570`) and D3D's patch routine are
  reproduced from the same data (`vertex_binding.cpp`). The title binds only
  a shader loaded fresh into one of a pass's 15 `vertexShaderArray` slots
  (`sub_821BBE70` → `sub_823C2E28` → `Bind`). A slot whose source has no
  stream in its declaration type is not bound, and neither is the pass's own
  `vertexShader`, which `sub_821BBC08` loads alone. Those stay as made and are
  never drawn. The executables add D3D's own shaders, the ones sent with
  `IM_LOAD_IMMEDIATE` and the image pool's memory move, bound to the
  declaration the title made for it. The list is 15,044 shaders. The
  fastfiles are the disc's and the update's, by name, unless custom maps are
  on (`MW2_CUSTOM_MAPS`): then every one in `game/`, as the multiplayer loads
  them then.
- **The build.** Each one is translated and compiled on every core as the
  game would (`vk::shaders::Build`): same translator, layout, depth format and
  scale, on the device the game's window would draw on. The pass reuses the
  last cache when only the game's files changed.
- **The marker.** `shaders/compiled.txt` is written last, after the cache is in
  place. Both files are written beside their place and renamed over it. The
  marker records what the cache was made for:
  - the build of the shader code: `MW2_SHADER_BUILD`, a SHA-256 CMake takes of
    the translator's, the layout's and the reader's sources;
  - the device (`deviceUUID`), its driver version and `pipelineCacheUUID`;
  - the resolution scale;
  - each fastfile's and executable's name, size and time.

  A change to any of these means the cache no longer holds what the game will
  build.
- **The gates.** The launcher checks the marker before PLAY and REPORT A BUG,
  and runs the pass again first when it no longer fits; it also runs the pass
  after an install. The game checks the marker on a player start, once the
  renderer has settled its device and scale and before the title runs. If it
  does not fit, the game starts `mw2-launcher --play <title> --device <id>` and
  ends, without writing its own cache over the launcher's. Its window is made
  hidden and shown only after this check (`vk::ShowWindow`), so a start that
  hands over shows nothing. The launcher then
  compiles for that device and starts the game, naming the device in
  `MW2_SHADERS_COMPILED_FOR`. A game that still finds no fit after that says so
  and plays on. Development runs are not gated.
- **The check.** `VK_EXT_pipeline_creation_feedback` says, for each library,
  whether it came out of the cache. The run's log and its bug report count the
  ones that did not ("N of them not in the launcher's"). That count should be
  0.

A device without pipeline libraries compiles a shader only together with a
draw's state, so the pass compiles nothing there. It leaves a marker that
says so, and the game builds pipelines at their first draw as before.

`MW2_SHADER_CACHE=<file>` records every pipeline a run builds: microcode,
formats and state (`SMW2`, version 3). The next run rebuilds them at start-up.
It has to be recorded, because the state a shader pair is drawn with exists
only in a previous run. The file is written back as a union, so it never
shrinks. `MW2_PIPELINE_CACHE=<file>` persists the driver's `VkPipelineCache`
in place of `shaders/pipelines.bin`.

### The driver's cache in several files

A driver's pipeline cache only grows. It keeps a pipeline for every form a
shader ever had, so each update that changes the translator leaves the last
one's pipelines in it, never asked for again: an AYN Thor's file held 5,656
pipelines in 47 MB (8.7 KB each) when its campaign's recorded pipelines
numbered 1,571. Nothing in the driver bounds it: Mesa reads as many objects
as the blob says and copies each into memory (`vk_pipeline_cache_load`), and
Turnip on Android has no cache of its own on disk. The bound was this
runtime's, which did not load a file over 64 MB, so a cache that grew past
that was begun again and a run compiled everything it drew.

With a budget (`MW2_PIPELINE_CACHE_MB`; 64 on Android, none elsewhere, where
the launcher's cache holds every shader of the game and is whole or of no
use) the cache is several of the driver's (`pipeline.cpp`):

- **The one being added to** is `<file>`. When it reaches an eighth of the
  budget it is closed as `<file>.<n>`, which is never written again, and an
  empty one takes its place. A save writes only the one being added to.
- **A creation is asked of each** without compiling
  (`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`), starting
  where the last was found, and compiled into the one being added to only
  when none holds it (`vk::pipeline::CreateGraphicsPipeline`). So what is
  found and what is compiled are counted, on any driver with
  `pipelineCreationCacheControl`; without it the cache stays one file.
- **`<file>.index`** keeps, for each closed file, the last session something
  was found in it and how many pipelines were.
- **At a start** the closed files nothing was found in for eight sessions
  go, and while the whole is over the budget the one least was found in for
  its size. What that one still held of use is compiled once more, into the
  one being added to. The recorded pipelines are rebuilt at every start, so
  a file is found in for as long as anything recorded is in it.

The log says what was loaded, each file closed or dropped, and after a save
"the cache is N KB in M files; F pipelines found in it so far, C compiled
by the driver". A report ends with the pipelines built and how many of them
the driver compiled, which in a second run of the same level is none.

## Characters' normals: the 2_10_10_10 pack

The games skin their animated models on the processor: they unpack each
vertex's normal and tangent from the D3D 10-10-10-2 format (`vupkd3d128`),
turn them by the bone and pack them again (`vpkd3d128`) for the GPU. Until
November 2026 the recompiler's pack wrote x into z's bits and z into x's,
and its unpack added w to 3.0 where Xenia adds 1.0, so the characters were
lit as if the light came from another side
([building.md](building.md#local-changes-to-xenonrecomp)). The fix changes
only those instructions, and with them the game code of every title.

Before and after on the AYN Thor (28 November 2026; android 05d738e's apps
against the fix's, debug builds `.vpka`/`.vpkb`, the same level loaded from
the console with the same pad script, the game's own frame dumps at the
same frame numbers):

| Scene | Before | After | Frames a second (play, before / after) |
|---|---|---|---|
| Call of Duty 4, F.N.G. (Killhouse), Gaz facing the lit window | his cap top and his back lit, as on the PC | his back dark, the light on the side toward the window | 118.9 / 118.9 |
| Modern Warfare 3, Black Tuesday, Sandman in the vehicle and in the street | his face lit evenly from the front | his face darker, lit warm from the fire behind him | 115.2 / 115.2 |
| Advanced Warfare, Induction (Seoul), the soldiers in the pod | faces and chests lit from the front | lit from the window and the light panel behind them | 117.8 / 117.7 |

Nothing else in the pictures changed: the level, the weapon, the effects
and the HUD are the same, and the characters' meshes are whole (no torn or
faceted skinning, no black or speckled normals).

## Not implemented

- Resolve write-back: the CPU cannot read resolved pixels from guest memory.
- Memory export in shaders. Its one use, the image pool's block copy, is made
  on the CPU at its draw (`runtime/image_move.cpp`).
- Multiple render targets, the console's sample positions, and alpha to
  coverage (MW2 never sets `RB_COLORCONTROL`'s alpha-to-mask bit).

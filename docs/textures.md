# Textures

How a Xenos texture is described, how it is laid out in guest memory, and how
the runtime turns it into a Vulkan image that matches what the console's GPU
would sample at the moment a draw executes.

| Piece | Where |
|---|---|
| Fetch constant, tiling, level layout, untiling | `runtime/gpu/texture.{h,cpp}` |
| Xenos to Vulkan formats and swizzles | `runtime/gpu/vulkan/texture_formats.{h,cpp}` |
| Images, samplers, the texture set, re-reads | `runtime/gpu/vulkan/texture_cache.{h,cpp}` |
| The images' memory, in blocks | `runtime/gpu/vulkan/image_memory.{h,cpp}` |
| Write watch on guest pages | `runtime/gpu/memory_watch.{h,cpp}` |
| The image pool's block copy | `runtime/image_move.{h,cpp}` |
| Replacements and the keyed dump | `runtime/gpu/texture_replace.{h,cpp}`, `runtime/png.{h,cpp}` |
| Upload round-trip test | `tools/upload_texture.cpp` (target `upload-texture`) |

## The fetch constant

A texture is described entirely by its fetch constant: six dwords per slot,
thirty-two slots, in the register file at `0x4800`. A shader's `tfetch` only
names the slot. The same window also holds vertex fetch constants (three per
six-dword group); the low two bits of dword 0 say which a slot holds (2
texture, 3 vertex). Field layout follows Xenia's `xe_gpu_texture_fetch_t`
(`gpu::TextureFetch`):

| Dword | Bits | Field |
|---|---|---|
| 0 | 0..1 | type |
| 0 | 2..9 | sign per component: unsigned, signed, biased, gamma |
| 0 | 10..18 | clamp X, Y, Z (3 bits each) |
| 0 | 22..30 | pitch in texels >> 5 |
| 0 | 31 | tiled |
| 1 | 0..5 | format |
| 1 | 6..7 | endianness: none, 8in16, 8in32, 16in32 |
| 1 | 10 | stacked (2D array) |
| 1 | 12..31 | base address >> 12 (physical) |
| 2 | | size minus one: 1D 24 bits; 2D 13+13 (+6 bits of stack depth at 26); 3D 11+11+10 |
| 3 | 1..12 | swizzle, 3 bits per component |
| 3 | 13..18 | exponent adjust (signed) |
| 3 | 19..24 | mag, min, mip filter (2 bits each): point, linear, base map |
| 3 | 25..27 | anisotropy |
| 4 | 2..5, 6..9 | min and max mip level |
| 4 | 12..21 | LOD bias, signed, in 32nds |
| 5 | 0..1 | border colour |
| 5 | 9..10 | dimension: 1D, 2D/stacked, 3D, cube |
| 5 | 11 | packed mips |
| 5 | 12..31 | mip address >> 12 (physical) |

Addresses are GPU physical addresses, not addresses in the runtime's flat
guest space; they must be resolved with `kernel::FromPhysical`. Read as flat
addresses they land in untouched memory, which looks exactly like a texture
the title has not filled yet.

## Layout in guest memory

`gpu::ExtentsOf` (Xenia's `GetSubresourcesFromFetchConstant` and
`GetGuestTextureLayout`) says where the levels are; `gpu::ReadTexture` reads
them into a linear, host-endian buffer, still in the guest's block format, one
level after another.

**Levels.** Level 0 is at the base address with the fetch constant's pitch.
Levels 1 and up follow one another from the mip address, each laid out as if
the base were a power of two on each side, so a level's stride is not its own
size. Pitch and height are rounded up to a 32x32-block tile, for linear
textures too; a linear mip's rows are also 256-byte aligned. Each cube face or
array slice of a level starts on a 4 KB boundary.

A volume's level is strided by its own rows and, down the third axis, by the
**base's** depth rounded up to the four slices of a volume tile -- not by the
level's own depth, which shrinks with it. That term was read out of Xenia
rather than guessed, and `tests/texture_volume_test.cpp` now holds both it and
the level offsets that follow from it to that reading; what no desktop run can
check is a volume captured from a real one (`MW2_DUMP_TEXTURES_RAW`, which
dumps any volume it meets).

Which levels exist:

- no mip address means no chain, whatever the level range says;
- a texture whose min level is above 0 may point both addresses at the chain,
  and then has no base;
- the mip filter is not consulted, because a fetch instruction may override it.

**Packed mips.** When the packed-mips bit is set, from the first level whose
shorter side is 16 texels or less, every remaining level is stored in a single
tile, each at its own offset (`PackedOffset`, Xenia's `GetPackedMipOffset`):
the first three a quarter, an eighth and a sixteenth of a tile along the
shorter side, the rest along the longer one; a volume's 1x1 levels go along its
third axis. Packing can start at level 0, in which case level 0 itself sits
inside the tile rather than at the base address. Reading from the tile's
corner reads other levels of the same texture, which looks almost right.

**Tiling.** A tiled 2D surface groups blocks into 32x32 macro tiles with the
address scattered across two banks and four pipes (`TiledOffset2D`, Xenia's
`Tiled2D`). A volume's macro tile is 32x16x4 blocks and the slice index takes
part in the address (`TiledOffset3D`, Xenia's `Tiled3D`), so a volume cannot be
read as a stack of 2D-tiled slices.

**Endianness.** Guest memory is big-endian; each block is swapped as the fetch
constant's endian field says (`8in16` for the DXT formats, `8in32` for
`8_8_8_8`). A block's internal layout is the format's business.

**What is uploaded.** A cube map uploads six faces as an array, a volume all
its slices as a 3D image, a stacked 2D texture its first slice. 96-bit formats
(block size not a power of two) are not read.

## Formats

`vk::formats::VulkanFormatFor`. On a desktop GPU the block-compressed formats go across
byte for byte, so the untiler's output is uploaded directly without CPU decoding.

A mobile GPU that reports no BC support at all -- ARM Mali, most often -- cannot sample
them, so the runtime decodes BC1, BC2, BC3 and BC5 in software (`bc_decoder.cpp`) into
uncompressed formats (RGBA8 / RG8), with direct 32- and 64-bit aligned word writes.

The decoder writes into the bytes of the caller's own `TextureData`, which is what
lets the cache hand the same object back and keep the buffer: a restage of a texture
whose size has not changed then pays neither the allocation nor the zero fill of the
whole image. Measured on a 2048x2048 DXT1 decode that is 5.64 ms down to 3.39 ms for
the same bytes -- 40% of a restage, on the format that costs the most (an eightfold
expansion). The buffer is per thread, and is released when the texture budget is
lowered, which is what a phone's `onTrimMemory` arrives as.

Snapdragon handsets are a third case. Adreno reports BC support and samples it wrongly:
surfaces that read back black or missing for a frame at a time, and vertex garbage in
vegetation. So the runtime turns the driver's own BC sampling off there and software-decodes
instead (`pipeline.cpp`, `bc::AdrenoMode`), which is what the workaround costs: eight times
the bytes in the cache for a BC1 texture and four times for the rest, plus the decode.
`MW2_ADRENO_BC=dxt1` keeps the driver's sampling for everything but the BC1 family, which
is the case its sampler is known to get wrong; `MW2_ADRENO_BC=hw` (the same as
`MW2_FORCE_BC_HW=1`) drops the workaround altogether on a driver that proves clean; and
`MW2_FORCE_BC_DECOMPRESS=1` makes a desktop driver exercise the same decoder, so a decode
bug can be told apart from a driver one.

That is Qualcomm's driver. Under Turnip the driver's sampling stays on (`MW2_ADRENO_BC=sw`
asks for the decoder): Modern Warfare 3 streams textures in and its pool moves them all
match long, and decoding them was most of the long frames left once every pipeline was
built -- on an AYN Thor, a Seatown match lost a blank 127 times in four minutes decoding
and 32 times sampling, with 80 MB of textures in place of 300 and the same picture.

A texture read in parts (the model lighting volume) is read in parts with replacements on
as well, once the title has rewritten it 64 times (`kOwnBytes`): Modern Warfare 3's
multiplayer has replacements on for its camouflage packs and read the 2 MB volume whole at
a segment's end of every frame, 5.6 GB in 85 s against 0.7 GB for every other texture.

| Xenos (id) | Vulkan |
|---|---|
| 8, 8_A, 8_B (2, 8, 9) | R8 |
| 5_6_5 (4) | R5G6B5 |
| 8_8_8_8 (6, 14, 50) | R8G8B8A8 |
| 2_10_10_10 (7, 54) | A2B10G10R10 |
| 8_8 (10) | R8G8 |
| 4_4_4_4 (15) | R4G4B4A4 |
| DXT1 (18, 51) | BC1 |
| DXT2_3 (19, 52) | BC2 |
| DXT4_5 (20, 53) | BC3 |
| 16, 16_16, 16_16_16_16 (24, 25, 26) | R16 / R16G16 / R16G16B16A16 unorm |
| 16_FLOAT variants (30, 31, 32) | R16 / R16G16 / R16G16B16A16 sfloat |
| 32_FLOAT variants (36, 37, 38) | R32 / R32G32 / R32G32B32A32 sfloat |
| DXN (49) | BC5 |
| DXT3A (58) | R8, expanded on the CPU |
| DXT5A (59) | BC4 |

- **DXT3A** is sixteen explicit four-bit values per block (the alpha block of
  DXT2/3), not BC4's endpoints and indices. Both are eight bytes a block, so
  treating one as the other passes every size check and decodes to noise.
  `ReadTexture` expands it to one byte a texel. MW2 stores the world's light
  map in it.
- **Gamma.** A fetch constant whose colour components are signed `gamma` asks
  the sampler to linearise. The image view uses the sRGB counterpart
  (`SrgbFormatFor`: R8G8B8A8, BC1, BC2, BC3); Xenos' piecewise curve and sRGB
  agree to within a code value. Alpha stays linear.
- **Not supported:** CTX1 (60), which no BC format matches;
  DXT3A_AS_1_1_1_1 (61); the signed, biased and integer component modes.

**Swizzle.** The fetch constant's swizzle (3 bits per component: 0..3 a
channel, 4 zero, 5 one) maps onto the image view's component mapping. It is
not optional: a font atlas with its glyphs in one channel samples as zero
without it. The identity is `0x688`; zero means "X in all four". A guest
format with fewer channels than its host format repeats its last channel
(`HostSwizzleFor`): one-channel formats read R in all four, two-channel ones
(including DXN, whose Y the title reads from alpha) read RGGG. The guest
swizzle is composed through that (`ComposeSwizzle`).

## Images and samplers

`vk::textures::Upload(fetch)` returns an id for the texture the fetch constant
describes, uploading it the first time it is seen.

- **One set for everything.** Set 1 holds every image the cache has, in the
  image array after the three stand-ins (at its id plus two), and every
  sampler at its own slot
  ([shaders.md](shaders.md#the-module-interface)). An entry is written when
  the image is made, or when a resolve's copy lends a new view, and the set
  stays bound: a draw only names, per slot, an image and a sampler
  (`Handle`). An id is given to another image only once the old one has been
  unbound for 600 frames, so no pending draw reads an entry being rewritten,
  and a resolve's copies are replaced only with the device idle.
- **Kinds.** 1D textures are 2D images one row tall (the translator samples
  every 1D fetch as 2D). Cubes are cube images, volumes 3D images. A view
  must match the kind the shader declares; where the title binds a 2D texture
  to a slot the program reads as a cube or volume, the declared kind's 1x1
  stand-in (element 0, 1 or 2 of the image array: 2D, cube, volume) is named
  instead. So are unbound slots. The stand-ins hold neutral opaque white
  texels `(255, 255, 255, 255)`, which keeps a lighting table or ambient map
  that is unbound from drawing black silhouettes.
- **Size limit.** An extent beyond `maxImageDimension2D` is rejected before
  anything is created: creating one is undefined behaviour, not an error, and
  a misparsed fetch constant can describe one. Failures are cached per fetch
  constant.
- **Sampler**, one per distinct state, from the fetch constant:
  - mag/min filter; mip filter linear or nearest. `BaseMap` samples the lowest
    level alone: `maxLod = minLod + 0.25`, so the min filter can still be
    chosen while every level choice rounds to the lowest (Xenia's approach).
  - `minLod` is the lowest level the image holds a picture for.
  - Anisotropy field 2..5 gives 2:1..16:1, capped by the device.
  - Clamp modes map directly; the "halfway" modes clamp at the edge texel's
    centre, which Vulkan lacks, and fall back to the plain edge clamps.
  - Cubes clamp to edge and, where `VK_EXT_non_seamless_cube_map` is
    available, filter each face on its own as Direct3D 9 does.
- **Resolved render targets** that the title samples back are not read from
  memory; the renderer lends the resolve's view (`Adopt`), see
  [rendering.md](rendering.md).
- **One image per texture.** An id is an image and a sampler. The six fetch
  dwords less the sampler's bits (clamps, filters, anisotropy, border colour)
  name the image, so a texture bound under several sampler states is held
  once.
- **Memory** comes from 64 MB blocks on desktop, or **16 MB blocks on Android**
  (`kBlockBytes` in `image_memory.h`), a range per image. A level streams tens of
  thousands of small textures through the cache, and an allocation each is what
  a driver is worst at: freeing one took over half a millisecond with forty
  thousand live. Anything over a quarter of a block gets an allocation of its own.
- **Letting go.** The console holds no copies: when the title puts another
  texture where one was, the old one is gone. `NewFrame` looks over the cache
  periodically (every 64 frames on desktop, or every 32 frames on Android),
  a slice of the images a frame:
  - An image is *idle* once unbound for `kUnusedFrames` (600 frames on desktop,
    or 120 frames on Android), past any submission still running.
  - An idle image whose pages the write watch saw written, and whose bytes
    then sign differently, is released. Bound again it would have had to be
    read again anyway. This is what keeps the cache near what the level is
    using: without it a streaming level left two gigabytes of such copies in
    seven minutes.
  - Past the budget (half the device-local memory clamped between 256 MB – 2 GB on desktop,
    or **128 MB on Android**, overridable via `MW2_TEXTURE_BUDGET_MB`), a **two-pass
    eviction** drops idle images down to three quarters of budget:
    1. *Normal Pass*: Evicts images idle past the sweep window.
    2. *Emergency Pass*: If still exceeding budget after a streaming rush, drops
       images unbound for 16 frames (outside the active GPU ring buffer depth).
  - A descriptor set not handed out for `kUnusedFrames` is freed back to its pool.
    A set is used no later than its images are bound, so an idle image has no
    set left.
  - Released images are destroyed a few a frame (half a millisecond's worth).
  Nothing here waits for the device.

## Keeping images in step with guest memory

A fetch constant always names the same image. A title that rewrites a texture in
place (MW2 rewrites its model-lighting table, a `512x256x4 8_8_8_8` volume in
which each model owns a 4x4 patch) leaves the fetch constant unchanged, so the
bytes themselves must be watched.

**Write watch.** Where the texture's pages are in the physical bank,
`gpu::watch::Track` is called *before* the read and returns a stamp; while
`gpu::watch::Unwritten` says no page has been written since, the texture is
unchanged for certain and nothing is hashed. The watch makes a page read-only
and takes one fault on the first write (see
[rendering.md](rendering.md#the-shadow-of-guest-memory)). Unlike the vertex
shadow, `Track` never backs off from pages the title keeps writing.

**Signing.** A texture that cannot be watched is hashed over every byte of its
base and chain (sampling misses small patch rewrites). An unchanged texture is
signed at most once per frame, then every 2, 4, ... up to 16 frames. A texture
that has ever changed, or that came up empty, is signed every frame. A watched
texture whose pages were written is signed at once, whatever the schedule.

**Ranges written by the runtime.** `vk::textures::MemoryWritten` marks any
texture read from a range the runtime itself just wrote (the pool copy below)
for signing at its next bind.

### Read when the draw would execute

The console's GPU reads a texture when a draw executes, not when the command
processor parses it. Two rules follow:

1. **Not too early.** The parse can run while the title is still writing the
   texture, or before a streamed image's bytes arrive. Textures that came up
   empty must be read again at the end of each segment that binds them.
   A texture the title rewrites is read where a frame first binds it, and not
   again until the next frame does: the parse runs a frame behind the title,
   which by the end of the segment may be writing the next frame's bytes into
   it. Read there, Black Ops III's model lighting table was found half written
   (models lit wrongly for one frame) and Advanced Warfare's drop pod in
   Induction lost the screens over its window for one frame, the window washed
   white. `MW2_TEXTURES_AT_BIND=0` reads rewritten textures at each segment's
   end again.
2. **Not too late.** The title frees a streamed texture's memory and reuses it
   once the command processor reports the draws before it done. Every read owed
   to a draw must happen before that report.

A draw binding a slot to the same six fetch dwords as the last draw that bound
it keeps that draw's image, without a lookup, and so does one binding fetch
dwords another slot was answered for (64 recent answers, by a hash of the
dwords), until `vk::textures::Epoch` moves: at a new frame or segment, when an image is let go, and at
`MemoryWritten`. Until then a lookup would answer the same and find nothing
more to check (the watch is consulted once a frame, and rewritten textures
once a frame, or once a segment with `MW2_TEXTURES_AT_BIND=0`). A resolve also ends it, since it can change which copy an
address names.

A submission is therefore cut into **segments** at every point where the
command processor tells the title that work has finished
(`vk::renderer::BeforeCompletion`, called from `command_processor.cpp`):

- `EVENT_WRITE_SHD` and `EVENT_WRITE_CFL` (end-of-pipe writes; D3D's fence is
  one);
- any memory write or `INTERRUPT` after a `WAIT_FOR_IDLE` with no draw since.

A plain `MEM_WRITE`, scratch write-back or read-pointer write happens when the
packet is fetched, ahead of earlier draws, so it tells the title nothing and is
not a completion point.

At each segment end `vk::textures::EndSegment` re-reads the segment's
empty textures (and with `MW2_TEXTURES_AT_BIND=0` its rewritten ones) and re-stages any that changed. Each segment's
uploads go into a command buffer of their own, submitted ahead of the
segment's draws and after the previous segment, so the frame remains one
submission ordered `uploads 1, draws 1, uploads 2, draws 2, ...`. No image is
versioned. There are four upload slots, each with a staging buffer that a
submission's uploads are sub-allocated from; an upload outside a submission is
submitted and waited for on its own.

**Empty blocks.** `EmptyPercent` counts 64-byte spans that are entirely zero
(memory nothing has written is zero in long runs; a dark or black compressed
texture is not). Above 60% a texture is marked as read from an empty block and
re-read at segment end until it drops to 25% or less. With diagnostics,
`MW2_MARK_EMPTY=1` binds flat magenta in place of such a texture in the world
pass for 60 frames after it is first seen empty, and textures still empty at
segment end are logged with the pool copies around them.

## Texture replacement (skins)

A player's picture can be uploaded in place of one of the title's textures.
Nothing in the title changes: the cache reads the texture as always and hands
the GPU another image.

**The key.** A texture has no name by the time it is bound -- a fetch constant
is an address, a format and a size -- so a replacement is named by what the
texture holds. `gpu::replace::KeyOf` is a 64-bit hash of the untiled,
host-endian bytes of one level as `ReadTexture` returns them, with the format,
that level's width, height and layers, and the number of levels from it down.
Tile padding, addresses and sampler state are not in it, so the same picture
has the same key in every run and on every device, wherever the allocator put
it. The level is the lowest the sampler may use: the base, unless the title
has not streamed the base in yet. A streamed texture therefore has a key for
each state it is drawn in, and a replacement applies to the state it was
dumped in. A texture the title draws into or builds as it runs has no stable
key. The function is frozen: changing it orphans every replacement made.

**Making one.**

1. Run with `MW2_DUMP_TEXTURES=<dir>` (in `.env`, as any switch) and play to
   where the texture is drawn. Every texture the cache reads is written once
   as `<key>.png`, with a line in `textures.txt`: key, format, size, levels,
   `gamma` when it is sampled linearised. A texture that is still all zero is
   left until it has been filled. The files are stored uncompressed.
2. Find the picture, edit it and save it under the same name, as PNG (8 or 16
   bits a channel, grey, RGB, RGBA or palette, not interlaced) or as binary
   PNM (`P5`, `P6`), which has no alpha. Any size: the sampler works in
   normalised coordinates.
3. Put it in `replace/` in the game folder, or any folder below that, or in
   the folder `MW2_TEXTURE_REPLACE` names. Take `MW2_DUMP_TEXTURES` out again.

The picture is edited as the dump shows it. For the one-channel formats (8,
DXT3A, DXT5A) that is grey and only red is read back; for 8_8 red and green.
The dump does not decode DXN, so normal maps cannot be made this way yet.

**The swap.** `vk::textures::Upload` keys what it has just read; with a file
for that key the image is made from the file instead -- `R8G8B8A8`, or its sRGB
form where the title's own image would have been an sRGB one, at the file's
size, with a full chain of levels averaged on the CPU (in linear light for a
gamma texture), under the title's own swizzle and sampler. The fetch constant,
the write watch and the signature stay the title's, so the texture is still
followed in guest memory. Cubes, volumes and arrays are not replaced, and a
file for one is refused with a line in the log; so is a file that does not
decode, or is larger than the device's largest image, and the title's texture
is uploaded.

An image is made as the title's texture or as a replacement and cannot become
the other, since the two differ in size and format. Each re-read
(`Prepare`, on the reader threads) keys what it read. When that asks for
something the image is not -- a streamed texture arriving that has a
replacement, the title putting another texture where a replaced one was -- the
title's bytes are staged as usual for the segment's draws where the image can
take them, and the image is orphaned (`Orphan`): its names are forgotten, so
the next bind makes the right image under a new id, while the old one stays
untouched for draws already recorded and is released once idle. A replaced
image whose memory is rewritten with the same picture stages nothing.

**Cost.** With no replacement folder and no dump, one predictable branch an
upload. With either, a hash of one level at each upload and re-read. A
replacement is decoded the first time it is wanted, on the ring consumer at a
first bind or on a reader at a segment's end, and kept: up to 128 MB of
decoded pictures on desktop and 32 MB on Android, least recently used first to
go, so an image the cache evicted is usually remade without reading the file
again. The image itself counts against the texture budget like any other.

**Animated replacements.** A replacement given as frames moves. Either
`<key>.0.png`, `<key>.1.png`, ... beside each other, or numbered pictures
(`0.png`, `1.png`, ...) in a folder named by the key's sixteen digits; the
frames are shown in the order of their numbers, which need not be contiguous,
and must all be one size. `<key>.txt` anywhere in the replacement folder may
hold a line `fps=<n>` (0.1 to 120, a fraction allowed); without it the rate is
twelve a second. Frames win over a single `<key>.png`, and one frame alone is
a single picture. The frame shown is `floor(seconds since start * fps)` modulo
the count, by the steady clock, so it does not depend on the frame rate and
every image of one animation shows the same frame.

**The Gold camo on weapons (MW3 multiplayer, disc version).** In a match the
gold of every weapon comes from one image, `~weapon_camo_gold-rgb&detail_~…`
in the gold materials' specular slot, loaded at three sizes: keys
`a8357229dfcf400c` (128x128), `2f7e28ad8644733c` (64x64) and
`d6caf021a5eda72a` (32x32). Replacing all three turns Gold into another camo
on every weapon; seen on the Thor with an original animated plasma
(2026-11-18). The menu's swatch for Gold is `73c9431503b443a9` (128x128) and
the banner behind the weapon `6fd4265d799d5a74` (408x152). The camouflage
packs ([below](#camouflage-packs)) are drawn through these five. Original art
for them, made from nothing of the game's:

    for k in a8357229dfcf400c:128x128 2f7e28ad8644733c:64x64 d6caf021a5eda72a:32x32 73c9431503b443a9:128x128; do
      python tools/camo_gen.py --key ${k%%:*} --size ${k##*:} --style plasma --frames 12 --fps 8 --out replace
    done

The texture keeps its one image, id and view. At an animated image's first
bind of a presented frame (`Animate`, from `Upload`) the cache asks
`gpu::replace::Due` for the frame the clock is at; when that is not the one
the image shows, its bytes are staged over the image through `StageInto`, into
the upload commands that run ahead of the binding segment's draws, exactly as
a texture the title rewrote is. So there is at most one re-stage per animated
image per presented frame, none for an image nothing binds, and none in a
frame the clock has not moved the animation in: at 12 fps and 60 frames a
second, one frame in five.

It cannot stall, because nothing but a copy happens after the first use. All
the frames, with their levels, are decoded where a single picture is: when
the replacement is first wanted. `Due` takes a lock, reads the clock and
returns a pointer; it never opens a file. A re-stage is one `memcpy` of the
frame into the slot's mapped staging buffer and one `vkCmdCopyBufferToImage`
-- for a 256x256 frame, 350 KB. When uploads are not open the frame is left
for the next bind rather than submitted alone and waited for.

The frames count against the bound on decoded pictures like any other, and an
animation is never decoded past it: where its frames together would pass the
bound, only the first that fit are shown, with a line in the log (a frame with
its levels is `width * height * 16 / 3` bytes; 32 MB on Android is 23 frames
of 512x512, or 95 of 256x256). An animation that advances is the most recently
used, so single pictures go first; one that is let go all the same -- several
large animations on screen at once -- is logged and stands still on the frame
it shows until its image is made again, rather than being decoded again in
the middle of a frame. The end-of-run line counts the animated replacements
and the frames staged for them.

### Making skins

Two tools in `tools/` (Python 3 with Pillow and numpy, nothing else) take the
work out of steps 2 and 3 above. Neither ships, fetches or reads any third-party
art: `iwi_match.py` only reads files **you** supply from your own PC copy of
the game and from mods you chose to install, and `camo_gen.py` draws original
procedural patterns from a seed. Both have `--help` and a `--self-test` that
runs on synthetic data.

**Reusing your own modded images: `tools/iwi_match.py`.** A dump has a key, not
a name. The tool finds the stock PC image a dump is a copy of, then writes the
image from your modded `.iwi` of the same name under the dump's key:

```
python tools/iwi_match.py --stock "D:/MW3/main" --dumps dumps --mod my_mods        # dry run: the report
python tools/iwi_match.py --stock "D:/MW3/main" --dumps dumps --mod my_mods --apply --out replace
```

`--stock` and `--mod` take folders (searched recursively) and `.iwd` files
(zips of `.iwi`); `--stock` is your unmodified install and `--mod` holds
modded files named as the originals they replace. `--dumps` is the
`MW2_DUMP_TEXTURES` folder. The report lists each dump with its score, the
lead over the runner-up, the stock name and a status: `replace` (a modded
image exists for it), `unchanged` (the match has no modded image, or the same
bytes as stock), `ambiguous`, `weak`. `--apply` writes only `replace` rows, as
`<out>/<key>.png` decoded from the modded `.iwi`; a dump is never copied. For
the one-channel dump formats (`8`, `DXT3A`, `DXT5A`) the picture is written
grey, as those textures are read back.

*Reading IWI.* IW5 files (version 8): a 32-byte header (flags, format,
dimensions), mip levels smallest first, the largest last; BGRA, BGR,
luminance-alpha, luminance, alpha, DXT1, DXT3 and DXT5 are decoded.
Wavelet-coded, DXN, cube and volume images are skipped and counted. The
layout follows public format notes and has been checked only on synthetic
files, so a real install that fails to read says why in the "skipped" lines.

*Matching, and how far to trust it.* A picture is reduced to a 32x32 luma
thumbnail and 8x8 colour and alpha thumbnails (from a mip of about 64x64, so
scanning an install is fast); two pictures score a weighted normalised
correlation after an aspect-ratio gate, and flat pictures compare by mean
colour. A match needs `--min-score` (0.90) and a lead of `--min-margin` (0.02)
over the next candidate, unless the close candidates would all give the same
output (`--allow-ambiguous` takes the best regardless). Expect good results on
textures that are the same picture on both platforms, which is most colour
maps. Expect misses or `ambiguous` for: solid-colour and near-flat textures
(many stock images look alike), normal maps (the console and PC pack them
differently, and DXN is not dumped), textures the console builds or tiles
differently, decals with large transparent areas, images that exist only
inside the PC fastfiles and not as `.iwi`, and a dump taken while a streamed
texture was at a low mip (it still matches by aspect and thumbnail, but with
a lower score). Check the first run's report by eye (`-v` shows weak
matches) before trusting `--apply`; a wrong match puts a good picture on the
wrong surface.

**Drawing a new one: `tools/camo_gen.py`.** Given a dump, it writes a picture
for that key at the dump's size (capped by `--max-dim`, 1024 for a picture and
256 for an animation), optionally with the dump's alpha as the mask:

```
python tools/camo_gen.py dumps/0123456789abcdef.png --style carbon --out replace
python tools/camo_gen.py dumps/0123456789abcdef.png --style digital --palette 1e2a1c,3a4a2a,6b7a4a,a89a6a
python tools/camo_gen.py dumps/0123456789abcdef.png --style marble --keep-alpha --seed 12
python tools/camo_gen.py dumps/0123456789abcdef.png --style iridescent --frames 24 --fps 15
```

Styles: `carbon` (woven fibre), `voronoi` (scales), `marble` (domain-warped
noise with veins), `digital` (pixel camo in four colours), `iridescent`
(thin-film colours), `plasma`, `nebula` (violet gas and stars), `neon_grid`
(glowing circuit traces), `toxic` (bubbling green), `magma` (lava in the
cracks of a crust), `glitch` (sliding blocks with split colour channels),
`aurora` and `frost`. All but `carbon`, `voronoi` and `digital` also
animate with `--frames N`, writing `<key>.0.png` ... `<key>.<N-1>.png` and
`<key>.txt` (`fps=`) in the form of *Animated replacements* above; the loop is
seamless. Keep frames times size inside the decoded-picture bound (the tool
warns). The same seed, style and size give the same picture. `--keep-alpha`
matters for textures whose alpha is a cut-out; a dump that is opaque leaves
the output opaque. Pick the dump of a texture the game draws as a colour map:
a skin on a normal, specular or other data texture is read by the shaders as
data and will look wrong.

`--pack DIR` writes the same styles as a camouflage pack (below) and
`--gallery FILE` a contact sheet of them.

### Camouflage packs

Modern Warfare 3's multiplayer lists extra camouflages below Gold in Create a
Class: thirteen of the project's own, and every camouflage of every pack in
the game folder. Each is an entry of its own with its pack in its name
(`SOLID - RED`, `INTERSTELLAR - SLOTH`), its preview and a description that
credits the pack and its author; it is chosen per class and per weapon like
any other and is drawn on the weapon in a match. **No pack replaces
anything**: the game's thirteen camouflages stay as they are, and two packs
made to replace the same camouflage on PC are two entries. `MW2_CUSTOM_CAMOS=0`
leaves the list as the game has it.

**Adding packs.** A PC camouflage mod for Modern Warfare 3 (the Plutonium
kind: IWI images named after the game's image each replaces) is converted,
never used as it is:

- *Setup* (`setup\setup.cmd`): put the downloads -- `.zip`, `.rar`, `.7z`,
  `.iwd`, folders or loose `.iwi`, in any layout -- in `setup\camo-packs\`, or
  name another folder with `-CamoPacks <folder>`. Setup also fetches the packs
  the project hosts (below).
- *The Android app*: Settings, Multiplayer, Camo packs, **Add a pack from a
  file** takes a `.zip` or `.iwd`, or several loose `.iwi` at once, and
  converts on the phone; **Get the project's packs** fetches the hosted ones.
  The same screen lists each pack with its previews, author and link, a
  switch that takes it out of the game's list, and *Animate*.
- *By hand*: `python tools/import_pack.py <mod> --camos <game folder>/camos`.

**What a mod's file becomes.** The importer (`tools/import_pack.py`, and
`CamoImport.kt` on the phone, which follow the same rules) goes by the name:

| image in the mod | becomes |
|---|---|
| `detail_gold_col`, `~detail_gold_spc-...`, `weapon_camo_menu_gold`, `..._gold_sm` | one camouflage made from the mod's Gold: colour map, specular map, banner, preview |
| `weapon_camo_<x>`, `weapon_camo_menu_<x>`, `..._<x>_sm` | one camouflage made from its replacement of the game's `<x>` (classic, snow, multi, d_urban, hex, choco, snake, blue, red, autumn, marine, winter) |
| `weapon_<gun>_col`, `~weapon_<gun>_spc-...` (whole-weapon skins) | together one camouflage, with a tile cut from each weapon's skin and the richest tile for every other weapon |
| normal maps, scopes, a mod's screenshots | nothing |
| `mod.ff` | nothing: a fastfile is not read (see *Animation*) |

A mod that has the same image in several folders (Sloth's packs have `All`
and a folder a camouflage) lists it once, named after the folder that says
what it is (`autumn - Orange` is Orange); the same image with different
content in several folders is an entry a folder. A missing preview is made
from the camouflage. A mod with none of these names is taken picture by
picture: each PNG, DDS, TGA or JPEG a camouflage, `<x>_swatch` its preview,
`<x>_0`, `<x>_1`, ... the frames of an animation.

A whole-weapon skin is a sheet laid out for one weapon's model, which Gold's
tiled image cannot show. The importer takes the window of the sheet with the
most fine detail in each of its quarters -- the painted pattern rather than
the flat parts round it -- and mirrors it into a tile that repeats without a
seam; the class's weapon then decides which tile is drawn. The skins
themselves, sheet for sheet, are a job for the texture replacement above
(`tools/iwi_match.py`).

**A pack** is a folder `camos/<pack-id>/` in the game folder:

```json
{
  "name": "Solid Colours",
  "label": "Solid",
  "author": "Sloth",
  "url": "https://forum.plutonium.pw/topic/27407/release-solid-colours-camo-pack",
  "camos": [
    { "id": "red", "name": "Red", "target": "weapon_camo_red", "colour": "red_col.png",
      "swatch": "red_swatch.png", "banner": "red_banner.png" },
    { "id": "gold", "name": "Rainbow", "target": "gold", "specular": "gold_spc.png",
      "colour": "gold_col.png", "swatch": "gold_swatch.png", "animation": "scroll" },
    { "id": "lava", "name": "Lava", "frames": ["lava/0.png", "lava/1.png"], "fps": 8 },
    { "id": "skin", "name": "Jewel", "image": "skin.png", "weapons": { "iw5_ak47": "skin/iw5_ak47.png" } }
  ]
}
```

What is drawn on the weapon is `frames` (an animation at `fps`, 12 when
absent), else `image`, else `specular`, else `colour`: Gold's image on the
console is a specular map, so a mod's specular map is the one that looks as
it did on PC, and its colour map is kept for a mod that has no other. The
alpha of Gold's image is its gloss, 183 of 255 on every texel, and every map
of every PC mod looked at is opaque: read as gloss, 255 is a mirror's, which
left a weapon near black but for pinpoints of light. A picture on a weapon
that is opaque throughout is therefore drawn with Gold's alpha
(`gpu::replace::Alpha::Title`); one that has alpha of its own keeps it as its
gloss. The pack's files are not changed.
`weapons` gives a weapon, as a class names it, a picture of its own. `swatch`
(or `swatch_frames`) is the menu's preview and `banner` the strip behind the
weapon; both default to the camouflage. `target` notes which image the mod
made it for. `label` is the pack's short name for the list, where an entry is
`<label> - <name>`, or `<name> - <author>` for a pack of one. Pictures are PNG
or binary PNM of any size and are made the size of the texture they stand in
for (128x128 on a weapon). Packs are listed in the order of their folders'
names; the log names each pack as it is read and each entry it leaves out.

**Animation.** A still camouflage can move: `"animation"` is `scroll` (the
picture slides across itself, the look a scrolling material gives), `shimmer`
(a band of light runs over it) or `hue` (its colours turn round the wheel).
The renderer makes 48 frames from the one picture when the camouflage is
chosen and shows them as it shows a file animation's (above), a loop of three
seconds; nothing is computed while drawing. *Sloth's style*: on PC, Sloth's
Animated Camos Mod animates Gold with a scrolling material in a `mod.ff`;
the port neither reads nor ships that file and draws the same look itself
with `scroll`. The player turns it on in `camos/options.txt`, which the
Android app's Camo packs screen writes:

```
animate-gold=1              every still camouflage made for Gold scrolls
animate=<pack-id>           every still camouflage of that pack does
animate=<pack-id>/<camo-id> that one does
off=<pack-id>               the pack is not listed
```

**Whose they are.** `name`, `author` and `url` are the pack's credit: the game
shows the pack and its author in each camouflage's description, and the
Android app lists every pack with its author and link. `setup/camo-packs.json`
is what the project knows of the packs it has looked at -- name, author,
link, the names of the camouflages, and by `art` whose the artwork is -- and
the importers credit a mod by matching its file name there. No pack is
committed to this repository.

| pack | author | artwork | camos | here |
|---|---|---|---|---|
| MW3 Recomp Originals | the project | drawn by `tools/camo_gen.py` | 13 | generated at set-up |
| [Solid Colours](https://forum.plutonium.pw/topic/27407/release-solid-colours-camo-pack) | Sloth | his own: flat colours and a rainbow | 13 | hosted, fetched by setup and the app |
| [Bubble Gum](https://forum.plutonium.pw/topic/28668/release-bubble-gum-camo) | Sloth | his own, as far as the thread and the picture say | 1 | hosted, fetched by setup and the app |
| [Interstellar](https://forum.plutonium.pw/topic/34225/release-mwiii-interstellar-camo) | Sloth | after the Interstellar camouflage of Modern Warfare III (2023), which the thread's title names | 1 | import your own download |
| [Exclusion Zone](https://forum.plutonium.pw/topic/27811/release-mwr-exclusion-zone-camo) | Sloth | "Exclusion Zone Camo from MWR" (the thread): Modern Warfare Remastered's | 1 | import your own download |
| [Classic Camos](https://forum.plutonium.pw/topic/27905/release-classic-camos-pack) | Sloth | twelve camouflages of Call of Duty 4, Modern Warfare 2, Black Ops 1 to 3 and Ghosts, each named in the thread | 12 | import your own download |
| [Diamond](https://forum.plutonium.pw/topic/19998/release-diamond-camo-for-all-weapons-mw3) | jdhruv | the game's own weapon textures with a diamond pattern laid over them | 1 (33 weapons) | import your own download |

A pack whose art is its author's own is converted once and kept as an asset
of the `camo-packs` release of the public repository (`release` and `asset`
in `setup/camo-packs.json`, with its SHA-256;
[android-releases.md](android-releases.md#the-camo-packs-release)), from
where setup and the app fetch it. A port of another
Call of Duty's camouflage, or a pack built on the game's own textures, is
Activision's design: the project does not pass it on, and it gets in only as
the file a player downloaded themselves and imported.

**The project's own pack**, `mw3-recomp-originals`, is drawn by
`tools/camo_gen.py --pack` when the game is set up (`setup/setup.ps1`,
`android/fetch_deps.sh`): 2.7 MB of pictures that a script in the repository
reproduces exactly are not worth keeping in git, and generating them leaves
no doubt about where each texel came from. On Android it is an asset of the
app, copied into the game folder when the multiplayer starts.

**How it works** (`runtime/mw3_custom_camos.cpp`, which has the detail). The
camouflage menus do not read `mp/camoTable.csv` by row: each camouflage is
six items written out in the menu, with its name and row number constants of
their expressions, and the rows are a button group that already scrolls
within fourteen. When the menus are added to the UI the runtime copies Gold's
row once per camouflage in guest memory, changing those constants, and gives
every row the test that hides it outside the fourteen; the list then scrolls
through as many rows as there are (159 were tried on the PC build: 1,225
items in a menu that had 271; a thousand is the bound, the copies' names
having three digits). A copy's button still writes `gold` to the
class, since that is all the stats' enum can hold; the runtime notes which
copy it was in `saves/custom_camos.txt`
(`customClasses.0.weaponSetups.0=solid-colours/red`, and beside it the weapon
the class has there, for a camouflage with a picture a weapon), and answers
the menus' reads of that class with the copy's name, so the list, the class
summary and a copied class all show what was chosen. Choosing one of the
game's camouflages or another weapon removes the record: a write of one of
the game's own to a slot the stats hold Gold in, or a write of Gold. Nothing
else does. When the player signs in the menus look every class over and
write `none` throughout one that has no weapon; on a phone that comes before
the stats have arrived, when every class reads so, and taken for a choice it
removed every record at each start: a camouflage chosen in one run was plain
Gold in the next, while the log still named it at start-up. The log says
what the weapons wear whenever it changes (`camos: the weapon image shows
…`, Gold included), why a record went (`camos: <slot> was <camo> and is
"<value>" now, as the menu set it`), and which picture each of Gold's images
was given (`textures: the image of <key> (128x128) shows <file>`).

The pictures go through Gold's (above): its weapon image, preview and banner
are *switchable* replacements (`gpu::replace::Switchable`, `Select`), images
always made in a replacement's form -- of the title's own picture until
something is chosen -- so that a choice reaches them as an animation's next
frame does, with no image made anew. The preview follows the row with focus
(the menu's own lookup of that row's material, every frame), the banner the
class being looked at, and the weapon image the class chosen in the match.

**Limits.**

- Gold on a weapon is one image, so one pack camouflage is drawn at a time
  on this device: that of the class the player chose in the match (its
  primary weapon's, or its secondary's when the primary has none). Every
  Gold weapon in view is drawn with it, other players' and a real Gold
  included, until a class without one is chosen. In the menus each class
  and weapon keeps and shows its own.
- A camouflage drawn through Gold has Gold's material: a PC mod's flat
  camouflage (`weapon_camo_<x>`) comes out with Gold's shine. That material's
  colour map is black paint, where the PC game's Gold has a colour map a mod
  can replace (`detail_gold_col`): what a mod painted there is not drawn, and
  a camouflage shows by what it reflects, so it is darker indoors than under
  the sky, as Gold is.
- Other players see Gold: the choice is local and nothing but `gold` leaves
  the device.
- The class in use is learned from the class menu of the match
  (`custom<n>`), and kept in the same file; a match that starts on the class
  of the one before without the menu shows the last one chosen.
- A class copied or reset outside the menus' own scripts keeps Gold and
  loses the record.
- The private match's editor of the default classes (the `..._gamesettings`
  menus) keeps the game's list.
- The Android app opens `.zip` and `.iwd`; a `.rar` or `.7z` goes through
  setup on a PC, or is unpacked first.

## The image pool's block copy

MW2 keeps streamed images in a compacting pool. To relocate a block, the pool
(in the campaign build, `sub_823A5CB8`) allocates the new home, queues the move
(`sub_823A5780`) and points the image record at the new address. The move is
flushed through `T_Image_FlushMove` (`runtime/title.h`), which builds a surface
header over the two blocks and has the **GPU** copy them: one draw whose vertex
shader fetches the source and memory-exports it to the destination, with the
export stream constant in `c0.x`:

```
c0.x = 0x40000000 | (destination physical address / 4)
```

The translator drops memory exports ([shaders.md](shaders.md#exports)), so
`runtime/image_move.cpp` makes the copy on the CPU, at that draw:

1. A hook on `T_Image_FlushMove` records `{destination, source, size, stream
   constant}` before the title's own call writes the draw, so the parse never
   meets the draw first.
2. At every draw while copies are pending, the command processor calls
   `imagepool::AtDraw(c0.x, vertex shader hash)`. A match makes the copy there:
   every draw queued before it sees the old bytes, every one after the new.
   Once the copy shader is known, a matching `c0.x` under another shader is not
   a copy, because `c0` outlives the draw that set it.
3. `imagepool::AtFrameEnd` makes any copy whose frame the parse has left
   without meeting its draw, so a block is never left uncopied. With nothing
   consuming the command stream the copy is made immediately.

Before copying, `vk::renderer::BeforeStreamWrite` ends the current segment if
it samples the destination, so draws before the copy keep the old texture. The
copy uses `gpu::watch::WillWrite` to unprotect the range in one call and then
`vk::textures::MemoryWritten`.

The copy must be made at its draw. Made when the pool asks, it lands ahead of
draws the title has already queued that still name the block, and those draw
whatever the pool lays there next. Not made at all, the destination keeps
whatever it held before, since the image is not streamed again.

## Checking the path

- **Upload round trip.** Because block formats go across untouched, an image
  copied back off the device must return the uploaded bytes exactly.
  `upload-texture [--format=<n>] [--size=<w>x<h>]` does this for every format
  with a Vulkan equivalent; sizes that are not a multiple of the block size
  (`--size=17x9`) exercise the copy-region row length.
- **By eye.** A wrong tiling function produces a buffer of the right size full
  of plausible bytes, so only a picture confirms it. `MW2_DUMP_TEXTURES=<dir>`
  writes each texture as `<key>.png` (`gpu::DecodeToRgba`, which is also what
  a replacement's channel order is defined by) and, with diagnostics, each
  further mip level of every distinct shape bound as `<key>_level<n>.png`;
  `MW2_DUMP_TEXTURES_RAW=1`
  also writes the guest bytes for offline layout experiments. A reimplemented
  offline untiler must first reproduce a known-good texture before its output
  is trusted.

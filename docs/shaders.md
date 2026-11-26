# Shaders

MW2's shaders are Xenos microcode produced by Microsoft's offline compiler. The
title compiles nothing at run time: it hands the GPU microcode through the
command stream, so the runtime translates each program to SPIR-V as it is
loaded. When shaders are compiled and cached is covered in
[rendering.md](rendering.md); this page covers the microcode and the
translation.

| Piece | Where |
|---|---|
| Microcode field layouts | `runtime/gpu/vulkan/xenos_ucode.h` |
| Translator | `runtime/gpu/vulkan/shader_translator.{h,cpp}` |
| SPIR-V binary writer | `runtime/gpu/vulkan/spirv.{h,cpp}` |
| Resource interface | `runtime/gpu/vulkan/bindings.h` |
| Offline translator | `tools/translate_shader.cpp` (target `translate-shader`) |
| Disassembler | `tools/xenos_shader.py` (control flow), `tools/xenos_isa.py` (ALU and fetch) |
| Fastfile extractor | `tools/extract_shaders.py` |

## How shaders reach the GPU

The command processor (`runtime/gpu/command_processor.cpp`) loads a program
from two packets:

| Packet | Payload |
|---|---|
| `IM_LOAD_IMMEDIATE` (0x2B) | `{ type, start << 16 \| size, microcode... }`, type 0 vertex, 1 pixel |
| `IM_LOAD` (0x27) | `{ physical address \| type, start << 16 \| size }`; the microcode is in guest memory |

Most of the title's shaders arrive by `IM_LOAD`. `IM_LOAD_IMMEDIATE` states its
length twice (header count and the `size` field); a packet whose two lengths
disagree is a misparse of the command stream and is refused, because its
payload is command buffer rather than microcode.

A shader is identified by a hash of its microcode. An `IM_LOAD`'s microcode and
hash are kept by address, length and stage while the memory watch
(`gpu/memory_watch.h`) says nothing has written their pages, so a frame's
thousands of loads of a few hundred shaders read and hash each one once. With diagnostics compiled in
(the default; `-DMW2_DIAGNOSTICS=OFF` removes them, see
[building.md](building.md)), `MW2_DUMP_SHADERS=<dir>` writes each distinct
program as
`vertex_<hash>.ucode` / `pixel_<hash>.ucode`, big-endian as the guest holds it
(see [switches.md](switches.md)).

## Shaders in the fastfiles

`tools/extract_shaders.py` recovers the programs offline from the fastfiles,
for testing the translator against the whole corpus without running the game.

- A fastfile is `IWffu100` followed by one zlib stream.
- Compiled shaders sit in Microsoft's shader containers, tagged `0x102A1100`
  (pixel) and `0x102A1101` (vertex). The constant table inside confirms which.
- The zone is packed without alignment, so tags must be searched at every byte
  offset, not every dword.
- The container holds the constant table, debug record and name, not the
  program. D3D9's shader constructor `sub_820B8B90` copies `container[+0x04]`
  bytes of container, then `container[+0x08]` bytes of microcode from past the
  container. The microcode is stored raw.
- Between container and program lies the rest of the IW4 asset (pointer
  placeholders, the name string), of variable length. The extractor searches
  forward for a blob of exactly the declared length that decodes as a program
  *and* whose every full vertex fetch names a defined vertex format. All three
  conditions are required: control flow alone decodes from arbitrary data,
  since every 4-bit control-flow opcode is defined.

The `*packed` suffix in a container's debug name (`trivial_vertcol_simple.hlsl*packed`)
is part of the shader's name, not a storage format.

## Microcode encoding

**Control flow.** A program starts with a list of 48-bit control-flow
instructions packed two per three dwords: the first takes `dword0` and the low
half of `dword1`, the second the high half of `dword1` and `dword2`. The opcode
is bits 12..15 of each half.

| Opcode | Name | Opcode | Name |
|---|---|---|---|
| 0 | nop | 8 | loop_end |
| 1 | exec | 9 | cond_call |
| 2 | exec_end | 10 | return |
| 3 | cond_exec | 11 | cond_jmp |
| 4 | cond_exec_end | 12 | alloc |
| 5 | cond_exec_pred | 13 | cond_exec_pred_clean |
| 6 | cond_exec_pred_end | 14 | cond_exec_pred_clean_end |
| 7 | loop_start | 15 | mark_vs_fetch_done |

An exec's address is an *instruction* index; instructions are three dwords, so
instruction N starts at dword 3N.

**Instructions.** Each is three dwords, either an ALU pair (vector + scalar) or
a fetch. Nothing in the instruction says which: the exec's *sequence* word
carries two bits per instruction, bit 0 meaning fetch. A program therefore
cannot be decoded without walking its control flow first.

**Fetch opcodes** (`w0` bits 0..4): 0 `vfetch`, 1 `tfetch`, 16
`get_border_colour_frac`, 17 `get_computed_lod`, 18 `get_gradients`, 19
`get_weights`, 24 `set_lod`, 25 `set_gradients_h`, 26 `set_gradients_v`. Bit 19 of a vertex fetch
(`must_be_one`) is set on every real one; a fetch with it clear is not a vertex
fetch and is refused.

**Swizzles are component-relative.** Each two-bit source swizzle field is added
to its own position, so the identity swizzle is `0b11100100`, not zero.

**Relative constant addressing** has two independent parts. Bit 29 of `w1`
chooses the addressing register (a0 rather than aL) and says nothing about
whether addressing happens. Whether a source is addressed comes from
`const_0_rel_abs` (w1 bit 31) and `const_1_rel_abs` (w1 bit 30), selected by how
many earlier operands were constants, because the hardware carries two bits for
three possible constant operands (`Alu::SourceConstantIsAddressed`).

## The module interface

Every translated shader is built against one fixed interface
(`runtime/gpu/vulkan/bindings.h`), so a single pipeline layout serves every
shader and nothing is reflected at bind time.

```
set 0  nothing
set 1  binding 0  texture2D[65536]    every texture the cache holds, at its id + 2,
                  textureCube[65536]  declared once for each kind a shader samples:
                  texture3D[65536]    one array, each element read only as its kind
       binding 1  sampler[4096]       every sampler state
       elements 0, 1 and 2 of the image array are 1x1 stand-ins: 2D, cube, volume
set 2  binding 0  uint[]   the frame arena, as dwords
       binding 1  vec4[]   the same, as vec4
       binding 2  vec4[256]  a pixel shader's float constants, a uniform block at a
                  dynamic offset; only where they are one (below)
       binding 3  samplerBuffer   the slot's share of the arena as texels of four
       binding 4  usamplerBuffer  floats and of four dwords, for the stages that
                  read the arena so; only where one does (below)
no push constants
a draw table, in the arena at the dword the hardware draw passes as its base
vertex (vertexOffset, or firstVertex for a draw without indices), two words an
entry:
  0              where the draw's record is in the arena, in dwords
  1              the guest index the entry's 65,536 indices count from
or eight, at a multiple of four, where a vertex shader reads it as two texels:
  2   (6 words)  what the draw's vertex program reads first, as it lists them:
                 the first dword of a vertex fetch constant, the place of a
                 block of its float constants
an index, always 32 bits: (entry << 16) + (guest index - the entry's)
the draw's record:
  0   (5 words)  where each constant block is in the arena, in dwords:
                   0  the vertex constant table
                   1  vertex fetch constants (96 pairs of dwords, each padded to four;
                      a vertex fetch's first dword holds its endianness below the address,
                      or nothing there where the data was copied in host byte order)
                   2  256 boolean constants, packed bits (8 dwords)
                   3  32 loop constants
                   4  pixel float constants, 256 vec4 from SQ_PS_CONST's base
  5   (3)        unused: the next four are a vec4 of the arena
  8   (4)        vertex: float2 scale, float2 offset, window to clip space, loaded as one vec4
  12  (4)        pixel: alpha function, alpha reference, colour scale, gamma-target flag
  16  (4)        both: a bit per texture slot bound to a resolve's copy at the frame's
                 scale, the same at the shadow maps' scale, and the reciprocal of each
                 scale; read only by shaders built under MW2_SCALE or MW2_SHADOW_SCALE
  20  (1 a slot) pixel: per fetch slot up to the highest it samples, the image's
                 index (low 16 bits) and the sampler's (high 16)
  52  (1 a slot) vertex: the same for its own slots, only when it samples any; a
                 stage's own because the two may declare a slot as different kinds
or, where the pixel shaders read the arena as texels, from 20 on:
  20  (4 a slot) pixel: a texel for each slot it samples, in the slots' order:
                 the word above; the fetch constant's LOD bias as a float, with
                 its exponent adjust plus 32 in the low six bits (nought in
                 any bias); one over the width and over the height of level 0
                 as the title sized it. A slot sampled as a volume has a
                 second texel after it, which starts with one over the depth
  276 (1 a slot) vertex: its own slots' words, only when it samples any
```

- **Finding the record.** One hardware draw is a run of the title's draws
  ([rendering.md](rendering.md#the-recorder-thread)), so a vertex finds its
  own draw's record through the draw table. The vertex shader takes
  `BaseVertex`, where the table is, away from `VertexIndex`; the high sixteen
  bits of what is left are the entry, whose first word is the record's offset,
  and the low sixteen added to the entry's second word are the title's index,
  from which `r0.x` is made. It passes the record's offset to the pixel
  shader as a flat output at location 16, after the sixteen interpolators.
  The base vertex rather than the first instance, because a run of draws
  without indices made with one `vkCmdDrawMultiEXT` shares its first instance
  but not its first vertices.
- **One record a pass in the pixel shader.** A subgroup may hold pixels of
  two of the title's draws, so the flat input is not one value across it.
  Read as it came, every constant's address was a register a pixel rather
  than one scalar register, and the texture and sampler arrays had to be
  indexed `NonUniform`: on the AYN Thor (Adreno 740, Turnip) the pixel work of
  a frame went from about 4 ms to about 7. So the whole program is the body
  of a waterfall (`EmitBody`):

  ```
  loop { r = first(record); if (record == r) { program; break; } }
  ```

  `first` is `OpGroupNonUniformBroadcastFirst`. A pass runs the program for
  the invocations whose record is that of the first one still in the loop,
  and they leave; the rest go round. Inside a pass the record, every address
  made from it and every texture index are the same for the whole subgroup,
  and nearly every subgroup makes one pass. The loop goes round the program
  in all four of its forms -- straight-line, forward jumps as selections,
  backward jumps as loops, and the dispatcher, whose own loop is nested in
  it -- from after the
  interpolators are copied into the registers to after the deferred discard,
  which is an `OpKill` inside the pass. The record words are loaded inside
  the pass (below). A vertex shader has no waterfall: its record is a value
  a vertex, and its texture and sampler arrays are indexed `NonUniform`
  (`SPV_EXT_descriptor_indexing`).

  The four pixels of a quad come from one primitive, helper invocations
  included, so they share the flat value, make the same pass and the
  derivatives an implicit-LOD fetch takes are those of the whole quad. That
  rests on the driver giving helper invocations their primitive's flat
  value; one that did not would pick the wrong level of detail along that
  primitive's edges. The loop ends because the first invocation equals
  itself, which rests on the broadcast answering only with an invocation
  still in the loop.

  The waterfall needs subgroup ballot in the fragment stage. A device
  without it -- Turnip on the Adreno 650 of the Retroid Pocket Flip 2 offers
  subgroup operations to compute shaders only -- gets the form above the
  waterfall replaced (`shader::PixelRecord::Pixel`): each pixel reads the flat
  value as its record, and the texture and sampler arrays are indexed
  `NonUniform` as in a vertex shader. That costs the pixel work the waterfall
  saves and draws the same picture. A Mali has the ballot and is given this
  form too, until one has run the waterfall: Arm's compiler costs the two the
  same ([android.md](android.md#mali)), and `MW2_PIXEL_RECORDS=subgroup` is
  the waterfall there. The renderer picks the form once, when the device is
  made (`vk::pipeline::SubgroupPixelRecords`), and says which in the log.
- **A pixel shader's float constants as a uniform block**
  (`shader::PixelConstants::Uniform`). Each pixel reading its own record
  fetched every float constant it read from the arena's storage buffer, at
  the address the record gives: forty fetches a pixel in a lit surface's
  shader, and a quarter of the GPU's frame on the Adreno 650 (21.2 ms to
  15.4 on Modern Warfare 2's S.S.D.D.). So the constants are set 2's third
  binding instead, on a device without the waterfall and on every Adreno,
  with it or without (the Adreno 740 of the AYN Thor has the ballot, and the
  same level went from 8.7 to 8.3 ms): 256 vec4 of the arena as a uniform
  block with a dynamic offset, which a driver gives the shader as constants
  it need not fetch. Any other device with the ballot fetches them from the
  arena as before. The renderer binds set 2 again, at the draw's
  pixel constants, when a draw whose shader reads any is given others than
  the last (`DrawOp::kPixelConstants`); that is a state change, so the run
  of draws before it ends there
  ([rendering.md](rendering.md#the-recorder-thread)). The pixel constants
  are copied whole and kept while nothing in them is written, so most draws
  share the last one's: on S.S.D.D. one draw in thirty that joined a run
  starts one. The record keeps where the block is; such a shader does not
  read it. `vk::pipeline::UniformPixelConstants` picks the form with the
  device, `MW2_PIXEL_UNIFORMS=0|1` says otherwise, and the layout has the
  third binding only where it is used.
- **Each record word is loaded once**, where a driver would sink the load:
  in the narrowest block that dominates every read of it, ahead of the first
  (`RecordWord`, `PlaceRecordLoads`). A straight-line program is one block,
  so a word is loaded at its first read. In a program with the dispatcher a
  word read in one case is loaded in that case, and one read in several at
  the top of the iteration, in the block that switches. A driver merges two
  equal loads only where one dominates the other, which a straight-line
  program's repeated loads already were; the dispatcher's cases each loaded
  the words again. Loading every word in the entry block instead leaves the
  placement to the driver: RADV sinks a load to where its reads meet but not
  into a loop, so the words stayed live across the dispatcher's loop and it
  spilled scalar registers in 1,089 pixel shaders (356 unscaled), and a
  driver that does not sink would keep every word live across every program.
  On RADV (Renoir), over the game's 15,044 shaders, the 1,688 pixel shaders
  with the dispatcher come out with 6% fewer instructions and 17-20% fewer
  scalar loads; 77 of them spill a few scalar registers more than before
  (2 unscaled), with no fewer waves; the straight-line pixel shaders and the
  vertex shaders compile to the same code as before. Building all of them
  takes 4-6% fewer CPU instructions. The GPU time of a frame moves by under
  1% (S.S.D.D.: 720p -1.0%, 1080p and three times the title's size -0.4%),
  at the edge of the run-to-run spread.
- **Why no push constants.** RADV places push constants per draw by reading
  each bound shader's description, a cache miss on every draw, which was a
  third of the recorder thread's time; binding a texture set per draw was
  another tenth. Both are gone: the two sets are bound once a command buffer,
  and a draw is a pipeline when it changes, the dynamic state that changed,
  and the draw.
- **Modules are SPIR-V 1.3**, the first version with `BaseVertex` and the
  subgroup operations in the core. Before 1.4 an entry point lists only its
  Input and Output variables.

- Every constant is in the frame arena, copied there from the register file,
  and found by the offset the draw's record holds for its block. Binding five
  dynamic uniform blocks at every draw, as the runtime once did, cost a
  descriptor upload each time, and nearly every draw moves one of them.
- The hardware constant file holds 512 vec4; `SQ_VS_CONST` and `SQ_PS_CONST`
  give each stage a window into it (D3D9 gives vertex the low half, pixel the
  high half).
- A pixel program reads its window as one run of 256 vec4. A vertex program
  reads through the constant table: for each of its window's 16 blocks of 16
  vec4, the vec4 index of the block's copy in the arena. Constant `i` is
  `arena[table[i / 16] + i % 16]`, and a program loads `table[i / 16]` once
  for each block it names a constant of, placed as a record word is
  (`VertexConstant`, `PlaceRecordLoads`); only a constant indexed through
  a0/aL loads its block's word where it is read. A draw changes a block or
  so of the ten or so a vertex program reads (its world matrix), and the
  table lets it copy only the blocks that changed. D3D9 starts a window at
  a block of the file; one that does not is copied whole for the draw.
- **What a vertex loads.** With a load of the table's word at every constant
  and the window's base before it, two in five of a vertex's loads from the
  arena were the table: over a Team Player frame 66 loads a vertex, 30 of
  them the table, 15 the constants, 11 dwords of vertex data, 6 record
  words, 2 fetch constants and the draw table's 2 (`MW2_SHADER_STATS`
  counts them as the translator emits them). A driver joins equal loads but
  not these, whose addresses it cannot tell are equal. Now a vertex of that
  level loads 27 times, 2 of them the table: the window transform is one
  vec4 instead of four words, a vertex fetch one dword instead of two, and a
  packed format's components share their dword's load (the commonest world
  shader went from 93 loads emitted to 39, the next from 38 to 19). A dword
  of vertex data also goes through the two exchanges its endianness is made
  of rather than all four forms. On the Adreno 740 the opening of S.S.D.D.
  went from 10.1 to 9.1 ms of GPU a frame, 93.7 to 104.1 frames a second,
  and Team Player's heaviest views from 12.5 to 11.5 ms for the scene, 65 to
  71 frames a second. Fewer than half the loads for a twelfth of the time:
  the loads are not most of what a vertex costs. Where the renderer copies
  vertex data in host byte order the exchanges are not in the shader at all
  ([rendering.md](rendering.md#vertex-data-in-host-byte-order)): 106 frames
  a second and 9.0 ms in that opening.
- **The arena as texels, for a vertex shader on an Adreno 6xx**
  (`shader::VertexArena::Texels`). What Turnip compiles a vertex shader to
  (`MW2_SHADER_IR`) says what the loads cost there: its compiler has a load
  of several dwords only from the Adreno 7xx on, so on a 6xx every dword of
  the storage buffer is a fetch of its own -- an `isam` of a buffer texture,
  with an add for its address and a move of a zero for its second
  coordinate, three instructions -- and a vec4 is four of them. The
  commonest vertex shader of Team Player, the depth pass's, which is nothing
  but a position, was 207 instructions with 45 fetches in 21 registers; the
  lit world's 348 with 61 in 32. The loads also waited on each other four
  deep: the entry, the record, the table's word or the fetch constant, the
  constant or the vertex. So where the device says so
  (`vk::pipeline::VertexTexels`: an Adreno under Turnip, the 7xx too, which
  is measured below; or `MW2_VERTEX_TEXELS=0|1`)
  set 2 has the slot's share twice more, as texel buffer views of four
  floats and of four dwords, and a vertex shader:
  - fetches a vec4 -- a float constant, the window transform -- as one
    texel;
  - reads its draw table entry as two texels, eight dwords, the last six of
    which hold what it would have loaded through the record: the first
    dword of each vertex fetch constant it names and the place of each
    block of constants it reads, in the order it first asks
    (`Translation::vertexHeader`, `HeaderWord`; the renderer fills them from
    what it has just written for the draw, `WriteDrawTable`). A program
    that wants more than six finds the rest through the record as before,
    as does a constant indexed through a0/aL and a vertex shader's texture
    fetch;
  - fetches vertex data four dwords a texel, where the data is the
    renderer's copy in host byte order, the program is straight-line and
    the vertex is a whole number of texels long with nothing fetched before
    its start (`Translation::vertexFetchTexels`). The renderer then points
    that fetch constant at a multiple of four dwords
    ([rendering.md](rendering.md#the-frame-arena)).

  The depth pass's shader is 99 instructions with 11 fetches in 10
  registers, the lit world's 204 with 16 in 16, and a vertex's loads are
  two deep. What a vertex computes from what it loaded is the same
  instructions as before, so the picture is: of four fixed views of
  S.S.D.D., 64.3 to 99.7% of the pixels are the same to the bit with and
  without, against 65.3 to 99.7% between two runs without (what differs is
  what moves). On the Retroid Pocket Flip 2 the vertex shaders' share of
  Team Player's scene is 3.5 ms, where it was 6.4 with only the vec4 as
  texels and more with none (measured with every primitive put outside the
  clip volume, at 587 MHz; not with none), and the scene at its
  heavy views from 15.7 to 16.8 ms to 12.5 to 14.1
  ([android.md](android.md#modern-warfare-2-there-60-frames-a-second)).
  No other GPU has run this form, and without it a vertex shader's module
  is the bytes it was before, so no device's pipeline cache is spent.
- **The record as texels, for a pixel shader on an Adreno 6xx**
  (`shader::PixelArena::Texels`). The same chip, the other stage. What
  Turnip compiled the pixel shaders of Team Player to: a lit surface's is
  470 to 850 instructions with 26 to 44 fetches of single dwords of the
  storage buffer, for 7 to 13 textures. Four are the alpha test's words; for
  each texture three -- its word of the record and the two dwords of the
  fetch constant that hold the LOD bias and the exponent adjust, found
  through another word of the record -- and then the shifts and conversions
  that take those fields out, an `exp2`, a `getsize` of the image and two
  reciprocals for the half-texel offsets. With the two dwords and the size
  read as nothing (a test build: the wrong exposure, and 60 frames a second
  where there were 50) that was three milliseconds of a frame. So where the
  device says so (`vk::pipeline::PixelTexels`: an Adreno under Turnip,
  or `MW2_PIXEL_TEXELS=0|1`) the texel view of four dwords is bound to the
  pixel stage too, and a pixel shader:
  - fetches the alpha test's four words as the one texel they are;
  - fetches one texel of the record for each slot it samples, which the
    renderer fills at the draw (`TexturesFor`) with the slot's word, the
    constant's LOD bias already a float, the exponent adjust beside it
    (three integer operations make two to its power, exactly, where `exp2`
    of a converted field was the driver's), and one over the size of
    level 0, kept with each image by the texture cache
    (`vk::textures::Handle`). It reads no fetch constant, so a draw copies
    none for it, and asks no image its size. Under `MW2_SCALE` the size is
    the title's already, and such a shader does not read the record's
    scale words.

  A record then ends after the pixel shader's texels (four words a slot
  sampled, by rank, not thirty-two words of slots), and two draws'
  records are the same exactly when their bytes are, as before. Without
  this form a module is the bytes it was. The picture: the offset is
  `offset * (1 / size)` where it was `offset / size`, which the driver
  compiled to a reciprocal and a multiply as well; of four fixed views of
  S.S.D.D. 66.9 to 92.6% of the pixels are the same to the bit as a run
  without, against 65.6 to 94.6% between two runs without, and what
  differs is in the same places, the helicopters, the soldiers and the
  dust. On the Retroid Pocket Flip 2 Team Player's heavy views went from
  50 frames a second to 59.5 or more, the frame's GPU time from 18.5 ms to
  15.4 ([android.md](android.md#modern-warfare-2-there-60-frames-a-second)).
- The translator records the highest float constant a program reads
  (`Translation::constantsRead`) so the renderer copies only that much, unless
  the program addresses constants through a0/aL (`addressedConstants`), where
  the whole window must go across.
- A vertex shader writes `gl_Position` and all sixteen interpolators to
  locations 0..15 (zero until written), because a pixel-shader input with no
  vertex output behind it is invalid. A pixel shader reads the interpolators it
  uses and writes only the colour targets it exports (locations 0..3). The
  ones it uses are the low temporaries any instruction reads, a texture
  fetch's coordinates included: some are read by nothing else.
- Set 1 declares a slot as 2D, cube or 3D according to how the program fetches
  it (`Translation::textureKinds`); 1D fetches are sampled as 2D at v = 0.
  The three kinds' variables share binding 0, which Vulkan allows for
  variables whose types the binding's descriptor type admits: a sampled
  image's descriptor is the same whatever its view's kind, and ids are shared
  across kinds, so one array of 65536 serves them all where one of each kind
  would leave two thirds of the descriptors unreachable.

## Translation

### Control flow

Xenos jumps, calls and loops move the sequencer's program counter arbitrarily.
Rather than recover structured control flow, a program that has any of them
runs one loop whose body is a switch on the counter, one case per control-flow
instruction:

```
loop { switch (pc) { case 0: ...; pc = 1; ... default: break out; } }
```

Every case leaves `pc` set. Falling off the end selects the default, which
breaks out of the loop. All sixteen control-flow opcodes are handled this way.

SPIR-V structured-control-flow rules require two extra blocks: the switch's
merge block may not be the loop's continue target, and the default target may
not be the loop's merge block, so each gets a block of its own that only
branches.

A program with no jumps, calls or loops (`cond_jmp`, `cond_call`, `return`,
`loop_start`, `loop_end`) gets no loop and no switch: its counter can only step
through the instructions in order up to the first `*_end`, where the decoded
list ends, so they are emitted one after the other. That is nine in ten of the
game's pixel shaders and every vertex shader. The driver does not remove the
loop itself — RADV keeps the switch, and every register live across it — so
the straight-line form compiles to fewer registers and instructions and more
waves per SIMD, and compiles faster.

Sequencer state — pc, aL, loop and call stacks, the predicate, the address a
full vertex fetch computed for the mini fetches after it, the previous scalar
result for the `*_prev` opcodes, the `set_lod` value — lives in function
variables, because producer and consumer can be in different cases. Temporaries
are one array of 64 `vec4` function variables (an array because a destination
can be aL-relative). Drivers promote these to registers, so the translator
never builds SSA form.

Loops follow the hardware: `aL = iterator * step + start` from the innermost
loop constant (step and start are signed 8-bit), clamped to [-256, 256].

### Predication

Predication is not branching: a predicated instruction executes and only its
write is suppressed. Every store is a select under the condition in force —
the instruction's own predicate, a `cond_exec_pred*` block's predicate, or a
`cond_exec` boolean constant (uniform for the draw). The same applies to fetch
results and `set_lod`.

The `setp` family writes the predicate and a result together: the predicate
takes the comparison against zero, the destination 0 where it holds and 1 where
not (`setp_rstr` keeps the source, `setp_clr` writes FLT_MAX). Scalar
comparisons test one component against zero.

Kills are deferred: the discard is recorded and performed after the sequencer
finishes, because `OpKill` would terminate a block inside the dispatch switch.
A straight-line program defers them too, so both forms behave alike. The
difference is observable only to a shader that exports to memory after
killing.

### Exports

An export writes one register from both halves of the ALU pair under
complementary masks. Components neither half writes are still defined by the
encoding: literal 0 or literal 1 where the masks say so. So

```
max export62.w, r1, r1 + retain_prev export62.w
```

is `gl_Position = (0, 0, 0, 1)`, not a move. `max rN, rN` is the compiler's
move.

| Export | Handling |
|---|---|
| Vertex 0..15 | interpolators |
| Vertex 62 | position |
| Vertex point size | dropped; point size is fixed at 1 |
| Vertex eM0..eM5 (memory export) | dropped and counted (`droppedMemoryExports`) |
| Pixel 0..3 | colour targets |
| Pixel depth | translation fails |
| Pixel, anything else | dropped and counted |

Memory export has nowhere to land: set 2 is the frame arena, holding copies of
the vertex data each draw reads, not guest memory. The geometry such a program
draws is still drawn. The one memory export MW2 depends on, the image pool's
block copy, is performed on the CPU at that draw (see
[textures.md](textures.md#the-image-pools-block-copy)).

### Vertex fetch

Xenos has no vertex input stage. The sequencer writes the vertex index into
`r0.x` as a float, and the program computes its own fetch address, so the
pipeline declares no vertex input state and the shader does the same
computation:

- `r0.x` is the title's index, converted to float at entry: `gl_VertexIndex -
  gl_BaseVertex` names a draw table entry in its high sixteen bits, and its
  low sixteen are added to the guest index the entry holds.
- A full `vfetch` reads its fetch constant from the arena: one dword, the
  renderer's copy of the constant's first, with the address of the data in
  dwords above its low two bits and, in those, the endianness the register
  holds in its second dword (the register's own two are a type the program
  has no use for). The address is `(vf.x >> 2) + index * stride + offset`,
  read from the arena (set 2). The renderer copies what the draw reads into
  the arena and points the constant at the copy
  ([rendering.md](rendering.md)).
- Endianness is a run-time value (none, 8-in-16, 8-in-32, 16-in-32): a
  dword goes through the two exchanges the four are made of, each taken or
  not. Vertex data is not always 8-in-32: a mesh with 16-bit positions is
  8-in-16. A shader built for vertex data in host byte order
  (`shader::VertexOrder::Host`, where there is no shadow of guest memory)
  has none of this: the renderer made the exchange as it copied, and the
  dword is loaded as it lies
  ([rendering.md](rendering.md#vertex-data-in-host-byte-order)).
- A mini fetch has no index or fetch constant: it reuses the address of the
  preceding full fetch with its own offset and format. A full fetch whose
  destination selects nothing exists only to compute that address.
- The instruction's exponent adjust scales the result by a power of two.

Vertex formats decoded (component 0 in the least significant bits; packed
formats honour the signed and integer flags, normalising to [-1, 1] or [0, 1]
otherwise):

| Id | Format | Id | Format |
|---|---|---|---|
| 6 | 8_8_8_8 | 33 | 32 |
| 7 | 2_10_10_10 | 34 | 32_32 |
| 16 | 10_11_11 | 35 | 32_32_32_32 |
| 17 | 11_11_10 | 36 | 32_FLOAT |
| 25 | 16_16 | 37 | 32_32_FLOAT |
| 26 | 16_16_16_16 | 38 | 32_32_32_32_FLOAT |
| 31 | 16_16_FLOAT | 57 | 32_32_32_FLOAT |
| 32 | 16_16_16_16_FLOAT | | |

Format 0 is undefined; a fetch naming it or any other unlisted format fails
translation.

### Texture fetch

`tfetch` names a slot; everything about the texture comes from the fetch
constant ([textures.md](textures.md)). The translator:

- takes the coordinate components the fetch names from the source register
  (u for 1D, u/v for 2D, all three for 3D and cube);
- applies the fetch's half-texel offsets, plus a 1.5/1024-texel nudge on every
  axis (Xenia's value) so a point sample on a texel boundary lands where the
  console's fixed-point coordinates (8 fractional bits) put it. Offsets and
  unnormalised coordinates are in texels of the bound image's size, which
  under `MW2_SCALE` or `MW2_SHADOW_SCALE` is divided by the copy's scale for
  a slot bound to a resolve's copy ([rendering.md](rendering.md#resolution-scale));
  the shader asks the image, or, where a pixel shader reads the arena as
  texels, takes one over the size from the slot's texel of the record
  ([above](#the-module-interface));
- computes LOD from the fetch constant's LOD bias (dword 4 bits 12..21, signed,
  in 32nds, read from binding 1 at run time, or from the slot's texel), the
  instruction's own bias (in 16ths) and, when `use_register_lod` is set, the
  `set_lod` value;
- samples with implicit LOD plus bias in a pixel shader, explicit LOD in a
  vertex shader (which has no derivatives; `get_gradients` returns zero there);
- scales the result by the fetch constant's exponent adjust (dword 3 bits
  13..18, signed).

**Cube maps** sample a real cube. The `cube` ALU instruction produces
(T, S, 2·major axis, face); the fetch turns the face and the two coordinates
(in 1..2 after the 1.5 the title adds) back into a direction. Sampling a
direction rather than six array layers keeps the LOD continuous across face
edges. The `cube` operand arrives swizzled `.zzxy`/`.yxzz`; the hardware reads
XYZ from the first operand only.

`get_border_colour_frac` returns zero. `get_computed_lod`, `get_weights`,
`set_gradients_h` and `set_gradients_v` are not translated (the fetch opcode fails);
the instruction's filter overrides and register gradients are not applied, and
the sampler comes from the fetch constant alone.

### Fixed-function state in the shader

- **Alpha test.** The console's alpha test is fixed-function and Vulkan has
  none, so the pixel shader compares against the function and reference in
  the draw's record. The compare-function bits (001 less, 010 equal, 100
  greater) are a mask over the three comparison results.
- **Colour exponent bias.** The console scales a pixel shader's output by
  2^`RB_COLOR_INFO` exponent bias on the way into EDRAM; the title renders the
  world with a bias. Applied after the alpha test, which runs on the unbiased
  alpha.
- **Gamma targets.** Output to an `8_8_8_8_GAMMA` target goes through Xenos'
  piecewise-linear gamma curve (Xenia's `LinearToPWLGamma`); alpha is not
  converted.
- **Pre-transformed vertices.** With the viewport transform off, a program
  emits window coordinates (D3D9 XYZRHW); Vulkan would clip those away. The
  vertex shader applies `xy = xy * scale + offset * w` from the draw's record —
  the identity when the viewport transform is on, `2 * window / size - 1`
  otherwise. MW2's EDRAM clears depend on this.

## Validating a translation

```
cmake --build build --target translate-shader
./build/translate-shader [--compile] [--pixel-records] [--uniform-constants] [--host-order] [--scaled] vertex_<hash>.ucode out.spv
spirv-val --target-env vulkan1.0 out.spv
```

The shader type comes from the file name (`pixel`/`ps_` means pixel). Run both
checks: `spirv-val` checks the binary against the specification, `--compile`
hands it to the real driver against the actual pipeline layout
(`runtime/gpu/vulkan/pipeline.cpp`). A driver accepting a module is not
evidence that it is valid — drivers accept, for example, duplicate
`OpTypeImage` declarations and implicit-LOD sampling in a vertex shader, both
invalid SPIR-V.

At run time, a program that fails translation or that the driver refuses is
logged (`renderer: ... shader ... not usable`) and draws using it are skipped.

## Cross-GPU Control Flow Translation (Mali vs. Adreno)

Xenos control flow instructions permit arbitrary branches, subroutine calls, and loops. Early translator revisions lowered any non-linear program into a generalized state-machine loop containing an `OpSwitch` over program counter values:

```
loop { switch (pc) { case 0: ...; case 19: ...; default: break; } }
```

### The Mali Driver Optimizer Pathology
While desktop drivers (NVIDIA, AMD) and Qualcomm Adreno/Turnip simplify this construct in ~50–100 ms, ARM Mali drivers (Bifrost/Valhall on Dimensity SoCs) encounter a severe compiler optimizer pathology. The driver attempts loop unrolling and full CFG convergence analysis across all cases in `OpSwitch`, locking compilation in `vkCreateGraphicsPipelines` for **300 to 450 seconds per shader**.

### Structured Acyclic Forward Jumps
Static analysis of MW2's complete shader corpus revealed:
1. **203 of 207 shaders** are strictly linear straight-line code.
2. Exactly **4 shaders** contain branch instructions (`70A21AB18663D4DD`, `73B3571D97C99594`, `99724088FD84BC3D`, and `B43C5FC2AEE46DFE`).
3. **Zero shaders contain backwards loops or subroutine calls**. Every `CondJmp` instruction in MW2 is strictly forward (`target > self`).

Because forward jumps with no backward loops are acyclic DAGs, they map directly to structured `if/else` control flow. By structuring forward jumps via `OpSelectionMerge` / `OpBranchConditional` without emitting `OpLoopMerge`, Mali driver compile times drop from **>400,000 ms to <30 ms**.

For fallback compatibility where loops may occur, `OpLoopMerge` specifies loop control parameter `kLoopControlDontUnroll = 0x2` to forbid driver unrolling.

### Backward Jumps As Loops
A program whose only jumps go backward, each on a condition and each span inside or clear of every other, is emitted as nested `do { } while` loops (`Translator::EmitLoopSpan`), with no counter and no switch. Black Ops III's zombies draw a glow behind the weapon in the HUD with one (`A3D3B14EF13F3232`): two loops of 11 turns, 121 fetches a pixel. Through the dispatcher that was 3,200 turns of its loop for each pixel, and two quads of 40 by 34 pixels took 32 ms of an Adreno 650's frame (The Giant at 13 to 19 frames a second on a Retroid Pocket Flip 2) and 1.4 ms of an RTX 5080's; as loops they take 0.1 ms on the RTX 5080. A fetch inside takes its level of detail from the derivatives as the pixels' turns go together; inside the dispatcher's switch the same fetch got another level on an RTX 5080 (the glow came out brighter there than with the level stated). Programs that mix forward and backward jumps, overlap two forward jumps (the title's `if`/`else`) or use the sequencer's own loops and calls still go through the dispatcher: 9 of the 205 shaders of a match on The Giant, in a handful of HUD draws.

### Non-Blocking Runtime Draw Path
In-game draw calls encountering a pipeline miss must never block the presentation thread (`tid 25150`). On Android, uncompiled pipelines are queued to the background warmup scheduler (`warmup::g_warmupQueue`), and the current draw is skipped (`Skip("async pipeline compile")`) so frames continue presenting at locked 60 FPS while the background thread compiles.


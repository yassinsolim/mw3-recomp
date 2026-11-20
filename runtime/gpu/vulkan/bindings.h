#pragma once
// The interface every translated shader is built against. The translator
// declares its resources here, the pipeline layout is made from the same
// numbers, and the renderer fills them -- so one layout serves every shader
// and nothing is reflected at bind time.
//
// Nothing in it changes from one draw to the next but where the draw's record
// is (kRecord*): both sets are bound once a command buffer, and the layout has
// no push constants. Per draw, the driver is handed a pipeline when it changes,
// the dynamic state that changed, and the draw itself -- or nothing at all,
// when the draw goes into the hardware draw before it (kTableWords). A push
// constant range written at every draw was a third of the recorder thread's
// time in RADV, which re-reads every bound shader's description to place the
// values; a texture set bound at every draw was another tenth.
#include <cstdint>

namespace vk::bindings
{
    // Five blocks of constants, each copied from the register file into the
    // frame arena (set 2) and found there by the dword offset the draw's
    // record holds for it (kRecordOffsets). Set 0 has nothing in it.
    constexpr uint32_t kConstantSet = 0;
    constexpr uint32_t kVertexFloats = 0;     // the vertex program's constant table: a uvec4
                                              // whose x is SQ_VS_CONST's base, then for each of
                                              // the file's 32 blocks of 16 vec4 the vec4 index
                                              // of its copy in the arena (kArenaFloats)
    constexpr uint32_t kFetchConstants = 1;   // 96 vertex fetch constants, two dwords
                                              // each, padded to a uvec4
    constexpr uint32_t kBoolConstants = 2;    // 256 bits as 8 dwords
    constexpr uint32_t kLoopConstants = 3;    // 32 dwords
    constexpr uint32_t kPixelFloats = 4;      // 256 vec4 from SQ_PS_CONST's base
    constexpr uint32_t kConstantBlocks = 5;
    constexpr uint32_t kFloatConstants = 256;
    constexpr uint32_t kConstantFileBlocks = 32;  // 512 vec4, 16 a block
    constexpr uint32_t kConstantTableWords = 4 + kConstantFileBlocks;
    constexpr uint32_t kFetchSlots = 96;

    // Set 1: every texture the cache holds, and every sampler. One array of
    // sampled images, indexed by the texture's place in it, and one of
    // samplers; the draw's record says, for each fetch slot, which image and
    // which sampler (kRecordTextures). A shader declares the image array once
    // for each kind it samples a slot as -- 2D, a cube map, a volume -- all at
    // the same binding: Vulkan lets variables share a binding when each is of
    // a type its descriptor type allows, and a sampled image's descriptor is
    // the same whatever its view's kind. An element is only read as the kind
    // its view was made as: elements 0 to 2 are 1x1 stand-ins, one of each
    // kind in its number's place, for a slot with nothing bound or with a
    // texture of another kind, and every other element holds one texture. An
    // entry is written once, when the texture is made, and the set stays
    // bound.
    constexpr uint32_t kTextureSet = 1;
    constexpr uint32_t kTextureSlots = 32;
    constexpr uint32_t kImages = 0, kSamplers = 1;
    constexpr uint32_t kImageKinds = 3;          // 2D, cube, volume: the stand-ins
    constexpr uint32_t kImageCount = 65536;
    constexpr uint32_t kSamplerCount = 4096;
    // A slot's word in the record: the image's index, then the sampler's.
    constexpr uint32_t kHandleImageBits = 16;

    // Set 2: the frame arena as an array of dwords, where the constant blocks
    // are, and what the draw's vertex fetches read: a vertex fetch constant
    // holds a guest physical address; the renderer copies what the draw reads
    // into the arena and rewrites the constant to point at the copy. The
    // draw tables are there too, and the indices, which the GPU reads as the
    // index buffer.
    constexpr uint32_t kArenaSet = 2;
    constexpr uint32_t kArenaBinding = 0;
    // The same buffer as vec4, for the float constants.
    constexpr uint32_t kArenaFloats = 1;
    // On a device whose pixel shaders take their float constants as a uniform
    // block (shader::PixelConstants::Uniform): 256 vec4 of the same buffer, at
    // the dynamic offset of the draw's pixel constants. The one thing bound
    // again within a command buffer, when a draw's differ from the last's.
    constexpr uint32_t kPixelConstants = 2;

    // A draw table: what a hardware draw names as its base vertex
    // (vertexOffset, or firstVertex for a draw without indices), as a dword
    // offset into the arena. The GPU's cost is per hardware draw whatever its
    // size, so the title's draws that follow one another with the same state
    // are made as one, and the table is how each vertex of it still finds
    // its own draw: an entry is { where the draw's record is, a guest index },
    // and an index in the index buffer is
    //
    //     (entry << kTableIndexBits) + (the guest's index - the entry's guest index)
    //
    // always 32 bits wide. The vertex stage takes the base vertex away from
    // the vertex index, which leaves that; the high bits pick the entry, the
    // low bits added to the entry's guest index are the index the title's
    // draw named. A draw whose indices span more than 2^kTableIndexBits
    // vertices has an entry for each 2^kTableIndexBits of them, one after
    // the other, and its indices carry on into them by themselves. A draw
    // without indices has entries of its own and names the first as its
    // first vertex, so its vertices count up through them the same way.
    constexpr uint32_t kTableWords = 2;
    constexpr uint32_t kTableIndexBits = 16;

    // A draw's record: what is register state rather than shader state, in
    // the arena, at the dword its draw table entries hold. The vertex stage
    // hands it to the pixel stage as a flat value (location kRecordLocation).
    // One hardware draw is many records, so the vertex stage indexes its
    // texture arrays NonUniform; the pixel stage takes one record at a time
    // across the wave (a loop over the records it holds, docs/shaders.md), so
    // its record and texture handles are uniform within each pass of it --
    // or, on a device without subgroup ballot there, reads each pixel's own
    // and indexes NonUniform too (shader::PixelRecord).
    // The base vertex rather than the first instance, because a run of draws
    // without indices made with one vkCmdDrawMultiEXT shares its first
    // instance and not its first vertices.
    //
    // { uint x5 } the dword offset of each constant block, in the order above.
    constexpr uint32_t kRecordOffsets = 0;
    // Vertex: { float2 scale, float2 offset }, window coordinates to clip space
    // for a program drawn with the viewport transform off.
    constexpr uint32_t kRecordNdc = 5;
    // Pixel: { uint alpha function, float alpha reference, float colour scale,
    //          uint the target is the gamma form }.
    constexpr uint32_t kRecordPixel = 9;
    // Both, read only by shaders built for render targets larger than the
    // title's (MW2_SCALE, MW2_SHADOW_SCALE): { uint frame copies, uint shadow
    // copies, float frame factor, float shadow factor }. A bit per texture
    // slot bound to a resolve's copy, whose image is a whole number of times
    // the size the title gave it -- the frame's scale, or the shadow maps' --
    // and the reciprocal of each scale.
    constexpr uint32_t kRecordScaled = 13;
    // Pixel: a word per fetch slot, up to the highest the pixel shader
    // samples (kHandleImageBits).
    constexpr uint32_t kRecordTextures = 17;
    // Vertex: the same for the vertex shader's slots, present only when it
    // samples any. A stage's own, because the two may declare one slot as
    // different kinds, and an element of set 1 is read as one kind only.
    constexpr uint32_t kRecordVertexTextures = kRecordTextures + kTextureSlots;
    constexpr uint32_t kRecordWords = kRecordVertexTextures + kTextureSlots;
    // After the title's sixteen interpolators.
    constexpr uint32_t kRecordLocation = 16;
}

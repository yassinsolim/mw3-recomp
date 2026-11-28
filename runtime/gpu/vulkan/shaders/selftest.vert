#version 460
// The driver self-test's vertex shader (runtime/gpu/vulkan/selftest.cpp),
// built the way the renderer's are (shader_translator.cpp, the record's
// offset): no vertex attributes; the vertex index less the base vertex is a
// draw table entry (high half) and a vertex of that entry's draw (low half);
// the entry, at the base vertex in the arena, holds the dword offset of the
// draw's record, which is what goes on to the pixel stage, and where the
// draw's vertices are. The base vertex itself is never passed on: K11MCH1's
// Turnip v26.0.0 R7 and R8 crash in ir3's register allocator on a shader that
// both passes it on and loads by it, which the renderer never builds.
layout(set = 0, binding = 0) readonly buffer Arena { uint words[]; } arena;

layout(location = 0) out vec2 uv;
layout(location = 1) flat out uint record;

void main()
{
    uint encoded = uint(gl_VertexIndex - gl_BaseVertex);
    uint entry = uint(gl_BaseVertex) + (encoded >> 16) * 2u;
    record = arena.words[entry];
    uint at = arena.words[entry + 1u] + (encoded & 0xFFFFu) * 8u;
    gl_Position = vec4(uintBitsToFloat(arena.words[at]), uintBitsToFloat(arena.words[at + 1u]),
                       uintBitsToFloat(arena.words[at + 2u]), 1.0);
    uv = vec2(uintBitsToFloat(arena.words[at + 4u]), uintBitsToFloat(arena.words[at + 5u]));
}

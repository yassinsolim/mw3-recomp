#version 460
// The driver self-test's vertex shader (runtime/android/driver_selftest.cpp),
// built the way the renderer's are: no vertex attributes, a draw's record
// found by its base vertex, positions read from a storage buffer.
layout(set = 0, binding = 0) readonly buffer Arena { uint words[]; } arena;

layout(location = 0) out vec2 uv;
layout(location = 1) flat out uint record;

void main()
{
    record = uint(gl_BaseVertex);
    uint at = arena.words[record * 8u] + uint(gl_VertexIndex - gl_BaseVertex) * 8u;
    gl_Position = vec4(uintBitsToFloat(arena.words[at]), uintBitsToFloat(arena.words[at + 1u]),
                       uintBitsToFloat(arena.words[at + 2u]), 1.0);
    uv = vec2(uintBitsToFloat(arena.words[at + 4u]), uintBitsToFloat(arena.words[at + 5u]));
}

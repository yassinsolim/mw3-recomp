#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The driver self-test's pixel shader: the draw's record out of the storage
// buffer, its texture out of the one set that holds every texture, indexed
// non-uniformly, and the alpha test as a discard (bindings.h).
layout(set = 0, binding = 0) readonly buffer Arena { uint words[]; } arena;
layout(set = 1, binding = 0) uniform texture2D images[65536];
layout(set = 1, binding = 1) uniform sampler samplers[4096];

layout(location = 0) in vec2 uv;
layout(location = 1) flat in uint record;
layout(location = 0) out vec4 colour;

void main()
{
    uint base = record * 8u;
    uint image = arena.words[base + 1u];
    uint mode = arena.words[base + 2u];
    uint smp = arena.words[base + 3u];
    vec4 result = vec4(uintBitsToFloat(arena.words[base + 4u]), uintBitsToFloat(arena.words[base + 5u]),
                       uintBitsToFloat(arena.words[base + 6u]), uintBitsToFloat(arena.words[base + 7u]));
    if (mode != 0u)
        result *= texture(sampler2D(images[nonuniformEXT(image)], samplers[nonuniformEXT(smp)]), uv);
    if (mode == 2u && result.a < 0.5) discard;
    colour = result;
}

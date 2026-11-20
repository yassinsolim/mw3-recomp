#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Xenos shader microcode -> SPIR-V.
//
// The title compiles nothing at runtime: it hands the GPU microcode its offline
// compiler produced. A Vulkan backend has to translate that while the title
// runs, so this lives in the runtime rather than in a tool.
//
// What comes out has a fixed interface, which is what lets one pipeline layout
// serve every shader: a vertex shader writes gl_Position and its interpolators
// to locations 0..15; a pixel shader reads those and writes colours to 0..3.
namespace shader
{
    enum class Type { Vertex, Pixel };

    struct Translation
    {
        bool ok = false;
        std::string error;                  // why not, when not
        std::vector<uint32_t> spirv;

        // What the pipeline has to bind for this shader to run.
        uint32_t interpolatorMask = 0;      // interpolators read or written
        uint32_t textureMask = 0;           // texture fetch constants sampled
        // What each sampled slot is declared as: 0 a 2D image, 1 a cube map,
        // 2 a 3D image. A view of another kind is invalid there.
        uint8_t textureKinds[32]{};
        uint32_t vertexFetchMask = 0;       // fetch constant groups read
        // A group of six dwords is either one texture constant or three vertex
        // ones, so the group mask alone names slots the program never reads --
        // whose bits read as a vertex fetch of some enormous buffer. Exact slot:
        // bit `group` of `vertexFetchSubs[sub]`.
        uint32_t vertexFetchSubs[3]{};
        // The same slots numbered as the fetch window's pairs, group * 3 +
        // sub, a bit each: what a draw walks to rewrite them.
        uint32_t vertexFetchPairs[3]{};
        // A draw reads `index * stride + tail` dwords of what a fetch constant
        // points at, which is far less than the extent the constant declares.
        uint32_t vertexFetchStride[96]{}, vertexFetchTail[96]{};
        // Every pair of the fetch window the program reads at all, vertex or
        // texture, a bit each: a draw writes these into the arena and leaves
        // the rest of the window as it found it.
        uint32_t fetchPairsRead[3]{};
        // The float constants the program actually reads: one past the highest
        // index, or nothing if it reads none. A shader that reads four of them
        // was still being handed a copy of all 256 -- 4 KB a draw, twelve
        // megabytes a frame on the training level. `addressed` means it indexes
        // them through a0 or aL, where the index is a run-time value and the
        // whole window has to go across.
        uint32_t constantsRead = 0;
        bool addressedConstants = false;
        uint32_t colourMask = 0;            // colour targets a pixel shader writes
        bool writesDepth = false;
        bool writesMemory = false;          // uses the memory export path
        uint32_t instructionCount = 0;
        // Exports the hardware has no output for, dropped rather than failed.
        uint32_t droppedExports = 0;
        // Writes to guest memory, dropped for want of anywhere to put them.
        uint32_t droppedMemoryExports = 0;
    };

    // How a pixel shader reads its draw's record, when one group of pixels
    // the GPU runs together (a subgroup) may hold more than one draw:
    // Subgroup runs the program once for each record the subgroup holds, the
    // record one value across it, and needs subgroup ballot in the fragment
    // stage; Pixel reads each pixel's own record and indexes the textures
    // NonUniform, which is slower and runs on any device the renderer takes.
    enum class PixelRecord { Subgroup, Pixel };

    // Where a pixel shader finds its float constants. Arena: in the frame
    // arena, where its draw's record says, a fetch from a storage buffer for
    // every constant a pixel reads. Uniform: in a uniform block the renderer
    // binds at the draw's own, which a driver hands the shader as constants it
    // need not fetch; a hardware draw is then the title's draws that share
    // their pixel constants, not every one that changes no state. On an Adreno
    // 650 the fetches were a quarter of the frame's GPU time.
    enum class PixelConstants { Arena, Uniform };

    // `words` is the program in host byte order, `count` its length in dwords.
    // `scaled`: the render targets may be larger than the title's, and the
    // shader reads the record's bindings::kRecordScaled.
    Translation Translate(Type type, const uint32_t* words, size_t count, bool scaled = false,
                          PixelRecord record = PixelRecord::Subgroup,
                          PixelConstants constants = PixelConstants::Arena);

    std::string Describe(const Translation& result);
}

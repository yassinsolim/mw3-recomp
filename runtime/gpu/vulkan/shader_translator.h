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

    // What a program loads from the frame arena each time it runs, counted
    // as the loads are emitted (MW2_SHADER_STATS): the draw table's entry,
    // words of the draw's record, words of the vertex constant table, float
    // constants, fetch constants, dwords of vertex data, and the boolean and
    // loop constants.
    enum Load : uint32_t
    {
        kLoadDrawTable, kLoadRecord, kLoadConstantTable, kLoadConstant, kLoadFetchConstant,
        kLoadVertexData, kLoadOther, kLoadKinds
    };

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
        // For a pixel program that reads the arena as texels
        // (PixelArena::Texels): the texels its slots take in a draw's
        // record, one a slot and two for a volume
        // (bindings::kRecordPixelTexels).
        uint32_t textureTexels = 0;
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
        // The pairs whose data the program fetches as texels of four dwords
        // (VertexArena::Texels): the renderer points such a constant at a
        // multiple of four dwords.
        uint32_t vertexFetchTexels[3]{};
        // What the program reads from its draw table entry after the record
        // and the guest index (VertexArena::Texels, bindings::kHeaderWords),
        // in the entry's order: a fetch pair's number for the first dword of
        // that constant as the renderer rewrote it, or bindings::kHeaderBlock
        // and a block of the constant window for the place of its copy.
        uint8_t vertexHeader[6]{};
        uint32_t vertexHeaderWords = 0;
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
        // A pixel shader with a kill instruction of its own: it decides which
        // pixels there are even where nothing it computes is kept.
        bool kills = false;
        bool writesMemory = false;          // uses the memory export path
        uint32_t instructionCount = 0;
        // Exports the hardware has no output for, dropped rather than failed.
        uint32_t droppedExports = 0;
        // Writes to guest memory, dropped for want of anywhere to put them.
        uint32_t droppedMemoryExports = 0;
        // Arena loads emitted, by kind: exact for a straight-line program,
        // which runs each once; a pixel program's constants in a uniform
        // block are not among them.
        uint32_t loads[kLoadKinds]{};
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

    // The byte order of the vertex data a vertex shader loads from the
    // arena. Guest: as the console has it, and the shader exchanges the
    // bytes of each dword as its fetch constant's endianness says, ten
    // operations a dword -- what a shadow of guest memory needs, which holds
    // the guest's bytes. Host: the renderer exchanged them as it copied the
    // data, once a copy instead of once a vertex, and the shader loads the
    // dword as it lies.
    enum class VertexOrder { Guest, Host };

    // How a vertex shader reads the frame arena. Storage: all of it from the
    // storage buffer, a load a dword, and loads that wait on loads -- the
    // draw table's entry for the record, the record for where the constant
    // table and the fetch constants are, those for where a block of
    // constants and the vertex data are. Texels: through two texel buffer
    // views of the same memory as well, four floats and four dwords a
    // texel. A float constant is one fetch; the draw table's entry is two
    // texels that hold what the vertex loads next (bindings::kHeaderWords);
    // vertex data the renderer copied is fetched four dwords at a time. It
    // is for a GPU that reads a storage buffer one dword a fetch, three
    // instructions each (an Adreno 6xx: Turnip's ir3 has a load of several
    // only from the 7xx on): there the commonest vertex shader of Modern
    // Warfare 2's Team Player went from 207 instructions and 45 fetches a
    // vertex to 99 and 11, and what the vertex computes is not touched.
    enum class VertexArena { Storage, Texels };

    // How a pixel shader reads its draw's record and what it samples a slot
    // with. Storage: from the storage buffer, a load a dword -- the alpha
    // test's four words, and for each slot its word of the record and two
    // dwords of the fetch constant, found through another word of the
    // record -- and it asks each image its size, to turn the title's half
    // texels into coordinates. Texels: through the texel buffer view of
    // four dwords a texel. The alpha test's words are one fetch, and a slot
    // is one texel of the record that the renderer fills with the slot's
    // word, the constant's LOD bias and exponent adjust and one over the
    // image's size (bindings::kRecordPixelTexels), so a pixel neither loads
    // the fetch constants nor asks a size. For the GPU VertexArena::Texels
    // is for: a lit surface's pixel shader in Team Player ran 26 to 44 such
    // fetches for seven to thirteen textures. It applies to both stages:
    // a vertex shader that samples finds its own slots' words elsewhere in
    // the record (bindings::kRecordTexelVertexTextures).
    enum class PixelArena { Storage, Texels };

    // `words` is the program in host byte order, `count` its length in dwords.
    // `scaled`: the render targets may be larger than the title's, and the
    // shader reads the record's bindings::kRecordScaled.
    Translation Translate(Type type, const uint32_t* words, size_t count, bool scaled = false,
                          PixelRecord record = PixelRecord::Subgroup,
                          PixelConstants constants = PixelConstants::Arena,
                          VertexOrder order = VertexOrder::Guest,
                          VertexArena arena = VertexArena::Storage,
                          PixelArena pixels = PixelArena::Storage);

    std::string Describe(const Translation& result);
}

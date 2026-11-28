// What the driver self-test's vertex shader asks of a driver
// (runtime/gpu/vulkan/selftest.cpp, shaders/selftest.vert): the draw's record
// found as the renderer's own vertex shaders find it, through the draw table
// at the base vertex, and the record's offset -- a word loaded from the arena,
// never the base vertex itself -- passed on to the pixel stage. K11MCH1's
// Turnip v26.0.0 R7 and R8 crash in ir3's register allocator on a vertex
// shader that passes the base vertex on and loads by it, which the renderer
// never builds, and they run the games.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace
{
#include "shaders/selftest_shaders.inc"

    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        if (ok) return;
        g_failures++;
        std::printf("FAILED: %s\n", what.c_str());
    }

    constexpr uint32_t kOpVariable = 59, kOpLoad = 61, kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71,
                       kOpBitcast = 124, kOpISub = 130;
    constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationFlat = 14;
    constexpr uint32_t kBuiltInVertexIndex = 42, kBuiltInBaseVertex = 4424;
    constexpr uint32_t kStorageOutput = 3, kStorageStorageBuffer = 12, kStorageUniform = 2;

    struct Instruction
    {
        uint32_t op = 0;
        const uint32_t* operand = nullptr;
        uint32_t count = 0;  // operands
    };

    // Each result id's instruction, and the decorations, of a module.
    struct Module
    {
        bool valid = false;
        std::map<uint32_t, Instruction> results;
        std::map<uint32_t, std::map<uint32_t, uint32_t>> decorations;  // id: decoration -> first literal
        std::map<uint32_t, uint32_t> stores;                           // pointer -> value stored
    };

    template <size_t N>
    Module Read(const uint32_t (&words)[N])
    {
        Module m;
        if (N < 5 || words[0] != 0x07230203) return m;
        for (size_t at = 5; at < N;)
        {
            const uint32_t count = words[at] >> 16, op = words[at] & 0xFFFF;
            if (!count || at + count > N) return m;
            const Instruction i{ op, words + at + 1, count - 1 };
            if (op == kOpDecorate && i.count >= 2) m.decorations[i.operand[0]][i.operand[1]] = i.count >= 3 ? i.operand[2] : 0;
            else if (op == kOpStore && i.count >= 2) m.stores[i.operand[0]] = i.operand[1];
            else if (op == kOpVariable && i.count >= 3) m.results[i.operand[1]] = i;
            else if ((op == kOpLoad || op == kOpAccessChain || op == kOpISub || op == kOpBitcast) && i.count >= 2)
                m.results[i.operand[1]] = i;
            at += count;
        }
        m.valid = true;
        return m;
    }

    // The variable of a built-in, 0 if none.
    uint32_t BuiltIn(const Module& m, uint32_t which)
    {
        for (const auto& [id, decorations] : m.decorations)
        {
            const auto found = decorations.find(kDecorationBuiltIn);
            if (found != decorations.end() && found->second == which) return id;
        }
        return 0;
    }

    // The output variable at a location, 0 if none.
    uint32_t Output(const Module& m, uint32_t location)
    {
        for (const auto& [id, decorations] : m.decorations)
        {
            const auto found = decorations.find(kDecorationLocation);
            if (found == decorations.end() || found->second != location) continue;
            const auto variable = m.results.find(id);
            if (variable != m.results.end() && variable->second.op == kOpVariable &&
                variable->second.operand[2] == kStorageOutput)
                return id;
        }
        return 0;
    }

    // Whether [value] is a word loaded out of a buffer (an access chain into a
    // storage buffer or a block), and from which access chain.
    uint32_t LoadedFromBuffer(const Module& m, uint32_t value)
    {
        const auto load = m.results.find(value);
        if (load == m.results.end() || load->second.op != kOpLoad) return 0;
        const auto chain = m.results.find(load->second.operand[2]);
        if (chain == m.results.end() || chain->second.op != kOpAccessChain) return 0;
        const auto base = m.results.find(chain->second.operand[2]);
        if (base == m.results.end() || base->second.op != kOpVariable) return 0;
        const uint32_t storage = base->second.operand[2];
        return storage == kStorageStorageBuffer || storage == kStorageUniform ? chain->first : 0;
    }

    // Whether [value] is, or is computed straight from, a load of [variable].
    bool LoadOf(const Module& m, uint32_t value, uint32_t variable, int depth = 0)
    {
        const auto found = m.results.find(value);
        if (!variable || depth > 8 || found == m.results.end()) return false;
        const Instruction& i = found->second;
        if (i.op == kOpLoad) return i.operand[2] == variable;
        if (i.op == kOpBitcast) return LoadOf(m, i.operand[2], variable, depth + 1);
        if (i.op == kOpISub) return LoadOf(m, i.operand[2], variable, depth + 1) || LoadOf(m, i.operand[3], variable, depth + 1);
        return false;
    }
}

int main()
{
    const Module vertex = Read(kSelfTestVertex);
    Check(vertex.valid, "the vertex shader is SPIR-V");

    const uint32_t baseVertex = BuiltIn(vertex, kBuiltInBaseVertex);
    const uint32_t vertexIndex = BuiltIn(vertex, kBuiltInVertexIndex);
    Check(baseVertex && vertexIndex, "the vertex shader reads the base vertex and the vertex index, as the renderer's do");

    const uint32_t record = Output(vertex, 1);
    Check(record != 0, "the vertex shader hands the pixel stage a record at location 1");
    Check(vertex.decorations.count(record) && vertex.decorations.at(record).count(kDecorationFlat),
          "the record goes on flat");
    const auto stored = vertex.stores.find(record);
    Check(stored != vertex.stores.end(), "the record is written");
    if (stored != vertex.stores.end())
    {
        Check(LoadedFromBuffer(vertex, stored->second) != 0,
              "what goes on to the pixel stage is a word of the arena (the draw table's), as the renderer's");
        Check(!LoadOf(vertex, stored->second, baseVertex), "the base vertex itself is never passed on");
    }

    // Every vertex's index less the base vertex: the entry and the vertex.
    bool subtracts = false;
    for (const auto& [id, i] : vertex.results)
        if (i.op == kOpISub && (LoadOf(vertex, i.operand[2], vertexIndex) && LoadOf(vertex, i.operand[3], baseVertex)))
            subtracts = true;
    Check(subtracts, "the vertex index is taken less the base vertex, as the renderer's");

    std::printf("%s\n", g_failures ? "self-test shaders: FAILED" : "self-test shaders: ok");
    return g_failures ? 1 : 0;
}

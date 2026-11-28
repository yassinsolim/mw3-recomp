// The script compiler behind the mods folder.
//
// This console's build of the game cannot read a script's source: it loads
// `scriptfile` assets, which hold a script already compiled (its code, and
// beside it the names the code refers to). So a mod's .gsc is compiled here
// before the title sees it, by gsc-tool (xensik, GPL-3.0, THIRD_PARTY.md),
// which knows the instruction numbers and the numbers this build gives its
// built-in functions, methods, fields and stock script files.
//
// Only this file sees gsc-tool's headers. The build defines MW2_GSC when the
// library is there (Modern Warfare 3's two titles, and Advanced Warfare's
// multiplayer as title update 17 makes it; CMakeLists.txt).
#if MW2_GSC

#include "mods.h"

#include <xsk/stdinc.hpp>
#if defined(AW_TITLE)
#include <xsk/gsc/engine/s1_xb.hpp>
#include "aw_gsc_builtins.h"
#else
#include <xsk/gsc/engine/iw5_xb.hpp>
#endif

#include <deque>
#include <memory>
#include <mutex>

namespace
{
#if defined(AW_TITLE)
    // gsc-tool knows S1's Xbox 360 instructions and none of its built-in
    // functions' and methods' numbers; the ones this port's scripts call are
    // given to it here (aw_gsc_builtins.h has how each was found).
    class Context : public xsk::gsc::s1_xb::context
    {
    public:
        explicit Context(xsk::gsc::instance which) : xsk::gsc::s1_xb::context(which)
        {
            for (const auto& [id, name] : awgsc::kFunctions)
            {
                func_map_.insert({ id, name });
                func_map_rev_.insert({ name, id });
            }
            for (const auto& [id, name] : awgsc::kMethods)
            {
                meth_map_.insert({ id, name });
                meth_map_rev_.insert({ name, id });
            }
        }
    };
#else
    using Context = xsk::gsc::iw5_xb::context;
#endif

    // Recursive: the finder a compile calls for an include asks for script
    // numbers (IdOf) on its way.
    std::recursive_mutex g_lock;
    std::unique_ptr<Context> g_context;
    const mods::script::Finder* g_finder = nullptr;     // the compile in progress's
    // The compiler keeps a pointer into a compiled include's code while it
    // reads its function names, so each lives until the next Begin.
    std::deque<std::vector<uint8_t>> g_included;

    std::string WithoutExtension(std::string name)
    {
        for (char& c : name)
            if (c == '\\') c = '/';
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && name.find('/', dot) == std::string::npos) name.resize(dot);
        return name;
    }

    // gsc-tool's file callback: an empty code half means "this is source".
    std::pair<xsk::gsc::buffer, std::vector<uint8_t>> Read(const xsk::gsc::context*, const std::string& name)
    {
        mods::script::File file;
        if (!g_finder || !(*g_finder)(WithoutExtension(name), file))
            throw std::runtime_error("no such script in the mods or the game: " + name);
        if (file.bytecode.empty()) return { {}, std::move(file.text) };
        g_included.push_back(std::move(file.bytecode));
        return { { g_included.back().data(), g_included.back().size() }, std::move(file.stack) };
    }

    Context& TheContext()
    {
        if (!g_context)
        {
            g_context = std::make_unique<Context>(xsk::gsc::instance::server);
            g_context->init(xsk::gsc::build::prod, Read);
        }
        return *g_context;
    }
}

void mods::script::Begin()
{
    std::lock_guard held(g_lock);
    g_included.clear();
    TheContext().cleanup();
}

bool mods::script::Compile(const std::string& name, std::vector<uint8_t> text, const Finder& includes,
                           std::vector<uint8_t>& bytecode, std::vector<uint8_t>& stack, std::string& error)
{
    std::lock_guard held(g_lock);
    g_finder = &includes;
    try
    {
        Context& context = TheContext();
        const auto assembly = context.compiler().compile(name, text);
        const auto output = context.assembler().assemble(*assembly);
        const xsk::gsc::buffer& code = std::get<0>(output);
        const xsk::gsc::buffer& names = std::get<1>(output);
        bytecode.assign(code.data, code.data + code.size);
        stack.assign(names.data, names.data + names.size);
        g_finder = nullptr;
        return true;
    }
    catch (const std::exception& e)
    {
        g_finder = nullptr;
        error = e.what();
        return false;
    }
}

bool mods::script::Decompile(const std::vector<uint8_t>& bytecode, const std::vector<uint8_t>& stack,
                             std::vector<uint8_t>& text, std::string& error)
{
    std::lock_guard held(g_lock);
    try
    {
        Context& context = TheContext();
        const auto assembly = context.disassembler().disassemble(xsk::gsc::buffer{ bytecode.data(), bytecode.size() },
                                                                 xsk::gsc::buffer{ stack.data(), stack.size() });
        const auto program = context.decompiler().decompile(*assembly);
        text = context.printer().print(*program);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

std::string mods::script::NameOf(uint32_t id)
{
    std::lock_guard held(g_lock);
    const std::string name = TheContext().token_name(id);
    // What gsc-tool calls a number it has no name for.
    return name.starts_with("_id_") ? std::string() : name;
}

uint32_t mods::script::IdOf(const std::string& name)
{
    if (name.starts_with("_id_")) return 0;
    std::lock_guard held(g_lock);
    return TheContext().token_id(name);
}

#endif

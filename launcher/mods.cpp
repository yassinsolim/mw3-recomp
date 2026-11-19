#include "mods.h"
#include "disc.h"
#include "../runtime/install/install.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    bool EndsWith(const std::string& name, std::initializer_list<const char*> suffixes)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        for (const char* suffix : suffixes)
        {
            const size_t n = std::strlen(suffix);
            if (lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0) return true;
        }
        return false;
    }

    bool IsGameFile(const std::string& name) { return EndsWith(name, { ".ff", ".pak", ".bik" }); }
    bool IsExecutable(const std::string& name) { return EndsWith(name, { ".xex", ".exe", ".dll", ".so" }); }

    // Whole, or not at all: a copy that stops half way leaves nothing behind.
    bool CopyFile(const fs::path& from, const fs::path& to)
    {
        static char buffer[1 << 20];
        std::error_code ec;
        const fs::path partial = fs::path(to) += ".part";
        std::ifstream in(from, std::ios::binary);
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        bool ok = bool(in) && bool(out);
        while (ok && in)
        {
            in.read(buffer, sizeof buffer);
            out.write(buffer, in.gcount());
            ok = bool(out);
        }
        out.close();
        if (ok) fs::rename(partial, to, ec);
        if (!ok || ec)
        {
            fs::remove(partial, ec);
            return false;
        }
        return true;
    }

#ifdef MW3_TITLE
    // A script mod (docs/mods.md): a folder with scripts in it, which the
    // game reads from <game>/mods/<name>/ when mods.txt there names it.
    bool HasScripts(const fs::path& folder)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && EndsWith(install::Utf8(it->path().filename()), { ".gsc", ".gscbin" }))
                return true;
        return false;
    }

    std::vector<std::string> ReadList(const fs::path& list)
    {
        std::vector<std::string> lines;
        std::ifstream in(list);
        std::string line;
        while (in && std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            lines.push_back(line);
        }
        return lines;
    }

    bool WriteList(const fs::path& list, const std::vector<std::string>& lines)
    {
        std::ofstream out(list, std::ios::binary | std::ios::trunc);
        for (const std::string& line : lines) out << line << "\n";
        return bool(out);
    }

    // Turns a mod on (or, when `toggle` and it is on, off) in mods.txt,
    // keeping the other lines and their order; -> whether it is on now.
    bool Enable(const fs::path& modsFolder, const std::string& name, bool toggle)
    {
        const fs::path list = modsFolder / "mods.txt";
        std::vector<std::string> lines = ReadList(list);
        const auto at = std::find(lines.begin(), lines.end(), name);
        if (at != lines.end())
        {
            if (!toggle) return true;
            lines.erase(at);
            WriteList(list, lines);
            return false;
        }
        if (lines.empty())
            lines.push_back("# The mods that are on, one folder name a line; the first listed wins a file two have.");
        lines.push_back(name);
        WriteList(list, lines);
        return true;
    }
#endif
}

mods::Result mods::Install(const fs::path& folder, const fs::path& game)
{
    Result result;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return result;
    fs::create_directories(game, ec);

#ifdef MW3_TITLE
    const fs::path modsFolder = game / "mods";
    const std::string modName = install::Utf8(folder.filename());
    // A mod already in the game's mods folder: choosing it again turns it
    // off, or back on.
    if (fs::equivalent(folder.parent_path(), modsFolder, ec))
    {
        result.mod = modName;
        result.change = Enable(modsFolder, modName, true) ? Change::TurnedOn : Change::TurnedOff;
        return result;
    }
    const bool scripted = !modName.empty() && HasScripts(folder);
    if (scripted)
    {
        const fs::path to = modsFolder / folder.filename();
        fs::create_directories(to, ec);
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path relative = fs::relative(it->path(), folder, ec);
            const std::string name = install::Utf8(relative);
            if (it->is_directory(ec)) { fs::create_directories(to / relative, ec); continue; }
            if (!it->is_regular_file(ec)) continue;
            // The game's own kinds of file are for the game folder (below).
            if (IsGameFile(name) && !relative.has_parent_path()) continue;
            if (IsExecutable(name)) { result.skipped.push_back({name, "executables are never taken from a mod"}); continue; }
            if (CopyFile(it->path(), to / relative)) result.modFiles++;
            else result.skipped.push_back({name, "could not be written (disk full?)"});
        }
        result.mod = modName;
        Enable(modsFolder, modName, false);
        result.change = Change::Installed;
    }
#else
    const bool scripted = false;
#endif

    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(folder, ec)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    });
    for (const auto& entry : entries)
    {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = install::Utf8(entry.path().filename());
        const fs::path to = game / entry.path().filename();
        if (!IsGameFile(name))
        {
            // A script mod's other files went into its folder above.
            if (scripted) continue;
            result.skipped.push_back({name, IsExecutable(name) ? "executables are never taken from a mod"
                                                              : "needs the CoD Xe plugin, which this port does not run"});
            continue;
        }
        if (fs::is_regular_file(to, ec) && fs::file_size(to, ec) == fs::file_size(entry.path(), ec)) continue;
        if (!CopyFile(entry.path(), to))
        {
            result.skipped.push_back({name, "could not be written (disk full?)"});
            continue;
        }
        result.copied.push_back(name);
    }
    return result;
}

std::string mods::Report(const Result& result)
{
    std::string text;
    if (result.change == Change::TurnedOn)
        return "The mod \"" + result.mod + "\" is on. Choose its folder again to turn it off.";
    if (result.change == Change::TurnedOff)
        return "The mod \"" + result.mod + "\" is off. Its files stay in the mods folder; choose it again to turn it on.";
    if (result.change == Change::Installed)
    {
        char head[96];
        std::snprintf(head, sizeof head, "\" is installed and on: %zu file(s) in the game's mods folder.\n", result.modFiles);
        text = "The mod \"" + result.mod + head +
               "Its scripts run from the next match or mission. To turn it off, choose its folder inside mods.\n";
    }
    else if (result.copied.empty() && result.skipped.empty())
        return "That folder holds nothing to install: no .ff, .pak or .bik files, and no scripts.";
    if (!result.copied.empty())
    {
        char head[64];
        std::snprintf(head, sizeof head, "Installed %zu file(s) into the game folder:\n", result.copied.size());
        if (!text.empty()) text += "\n";
        text += head;
        for (const std::string& name : result.copied) text += name + "\n";
    }
    if (!result.skipped.empty())
    {
        if (!text.empty()) text += "\n";
        text += "Skipped:\n";
        for (const Skipped& skipped : result.skipped)
            text += skipped.name + " (" + skipped.reason + ")\n";
    }
    return text;
}

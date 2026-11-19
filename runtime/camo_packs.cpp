// Camouflage packs (camo_packs.h).
#include "camo_packs.h"

#include "log.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    std::string Utf8(const fs::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // As much of JSON as a pack.json holds: objects, arrays, strings and
    // numbers; true, false and null read as empty strings.
    struct Value
    {
        enum class Kind { Text, Number, List, Object } kind = Kind::Text;
        std::string text;
        double number = 0.0;
        std::vector<Value> list;
        std::map<std::string, Value> members;

        const Value* Member(const char* name, Kind of) const
        {
            const auto found = members.find(name);
            return found != members.end() && found->second.kind == of ? &found->second : nullptr;
        }
        std::string Text(const char* name) const
        {
            const Value* value = Member(name, Kind::Text);
            return value ? value->text : std::string();
        }
    };

    struct Parser
    {
        const std::string& in;
        size_t at = 0;
        // A pack.json nests three deep; this is what a malformed one may not pass.
        int depth = 0;

        void Space()
        {
            while (at < in.size() && (in[at] == ' ' || in[at] == '\t' || in[at] == '\r' || in[at] == '\n')) at++;
        }
        bool Take(char c)
        {
            Space();
            if (at >= in.size() || in[at] != c) return false;
            at++;
            return true;
        }
        static void AppendUtf8(std::string& out, uint32_t code)
        {
            if (code < 0x80) out += char(code);
            else if (code < 0x800) { out += char(0xC0 | code >> 6); out += char(0x80 | (code & 0x3F)); }
            else { out += char(0xE0 | code >> 12); out += char(0x80 | (code >> 6 & 0x3F)); out += char(0x80 | (code & 0x3F)); }
        }
        bool String(std::string& out)
        {
            if (!Take('"')) return false;
            for (; at < in.size(); at++)
            {
                const char c = in[at];
                if (c == '"') { at++; return true; }
                if (c != '\\') { out += c; continue; }
                if (++at >= in.size()) return false;
                switch (in[at])
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                    if (at + 4 >= in.size()) return false;
                    AppendUtf8(out, uint32_t(std::strtoul(in.substr(at + 1, 4).c_str(), nullptr, 16)));
                    at += 4;
                    break;
                default: out += in[at]; break;   // the quote, the solidus and the reverse solidus
                }
            }
            return false;
        }
        bool Read(Value& out)
        {
            Space();
            if (at >= in.size() || ++depth > 16) return false;
            bool ok = false;
            const char c = in[at];
            if (c == '"')
            {
                ok = String(out.text);
            }
            else if (c == '{')
            {
                at++;
                out.kind = Value::Kind::Object;
                ok = Take('}');
                while (!ok)
                {
                    std::string name;
                    if (!String(name) || !Take(':') || !Read(out.members[name])) break;
                    if (Take('}')) ok = true;
                    else if (!Take(',')) break;
                }
            }
            else if (c == '[')
            {
                at++;
                out.kind = Value::Kind::List;
                ok = Take(']');
                while (!ok)
                {
                    if (!Read(out.list.emplace_back())) break;
                    if (Take(']')) ok = true;
                    else if (!Take(',')) break;
                }
            }
            else if (c == '-' || (c >= '0' && c <= '9'))
            {
                char* end = nullptr;
                out.kind = Value::Kind::Number;
                out.number = std::strtod(in.c_str() + at, &end);
                ok = end != in.c_str() + at;
                at = size_t(end - in.c_str());
            }
            else
            {
                for (const char* word : { "true", "false", "null" })
                    if (in.compare(at, std::char_traits<char>::length(word), word) == 0)
                    {
                        at += std::char_traits<char>::length(word);
                        ok = true;
                    }
            }
            depth--;
            return ok;
        }
    };

    // A picture a pack names, when it is a file inside the pack.
    bool Picture(const fs::path& pack, const std::string& named, fs::path& out)
    {
        if (named.empty()) return false;
        const fs::path relative = fs::path(std::u8string(named.begin(), named.end())).lexically_normal();
        if (relative.is_absolute() || relative.has_root_name() ||
            (!relative.empty() && *relative.begin() == ".."))
            return false;
        std::error_code ec;
        out = pack / relative;
        return fs::is_regular_file(out, ec);
    }

    // The pictures of one camouflage entry under `single` or `several`.
    std::vector<fs::path> Pictures(const fs::path& pack, const Value& entry, const char* single,
                                   const char* several)
    {
        std::vector<fs::path> files;
        fs::path file;
        if (const Value* frames = entry.Member(several, Value::Kind::List))
        {
            for (const Value& frame : frames->list)
                if (frame.kind == Value::Kind::Text && Picture(pack, frame.text, file)) files.push_back(file);
        }
        else if (Picture(pack, entry.Text(single), file))
        {
            files.push_back(file);
        }
        return files;
    }
}

std::vector<camo::Camo> camo::Read(const fs::path& folder)
{
    std::vector<Camo> all;
    std::error_code ec;
    std::vector<fs::path> packs;
    for (fs::directory_iterator at(folder, ec), end; !ec && at != end; at.increment(ec))
        if (at->is_directory(ec)) packs.push_back(at->path());
    std::sort(packs.begin(), packs.end());

    for (const fs::path& pack : packs)
    {
        std::ifstream in(pack / "pack.json", std::ios::binary);
        if (!in) continue;
        const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Value root;
        Parser parser{ json };
        const Value* camos = parser.Read(root) && root.kind == Value::Kind::Object
                                 ? root.Member("camos", Value::Kind::List) : nullptr;
        if (!camos)
        {
            LOGW("camos: %s is not a pack: its pack.json does not parse or lists no camos",
                 Utf8(pack).c_str());
            continue;
        }
        const std::string id = Utf8(pack.filename());
        const std::string packName = root.Text("name").empty() ? id : root.Text("name");
        size_t listed = 0;
        for (const Value& entry : camos->list)
        {
            if (entry.kind != Value::Kind::Object) continue;
            Camo one;
            one.key = id + "/" + entry.Text("id");
            one.name = entry.Text("name").empty() ? entry.Text("id") : entry.Text("name");
            one.pack = packName;
            one.author = root.Text("author");
            one.frames = Pictures(pack, entry, "image", "frames");
            one.swatch = Pictures(pack, entry, "swatch", "swatch_frames");
            if (one.swatch.empty()) one.swatch = one.frames;
            if (const Value* fps = entry.Member("fps", Value::Kind::Number))
                one.fps = std::clamp(fps->number, 0.1, 120.0);
            if (entry.Text("id").empty() || one.frames.empty())
            {
                LOGW("camos: an entry of %s is left out: it has no id, or no picture that is a file"
                     " in the pack", Utf8(pack / "pack.json").c_str());
                continue;
            }
            if (all.size() == kMost)
            {
                LOGW("camos: only the first %zu camos are listed; %s has more", kMost, Utf8(pack).c_str());
                break;
            }
            all.push_back(std::move(one));
            listed++;
        }
        LOGI("camos: pack \"%s\" by %s, %zu camos (%s)", packName.c_str(),
             root.Text("author").empty() ? "an unnamed author" : root.Text("author").c_str(), listed,
             Utf8(pack).c_str());
    }
    return all;
}

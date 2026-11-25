// The commands waiting for the console's pump: see console_queue.h.
#include "console_queue.h"
#include "console.h"
#include "log.h"
#include "env.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace
{
    struct Entry
    {
        double at;
        std::string text;
        bool done = false;
    };

    std::mutex g_lock;
    std::vector<Entry> g_script;
    std::string g_place;                    // console::Place, the latest only
    bool g_parsed = false;
    std::chrono::steady_clock::time_point g_start;

    // A command may contain spaces; only ';' separates entries and only the first
    // ':' separates the time.
    void Parse()
    {
        g_parsed = true;
        g_start = std::chrono::steady_clock::now();
        const char* spec = env::Text("MW2_CONSOLE");
        if (!spec) return;

        std::string s = spec;
        size_t pos = 0;
        while (pos <= s.size())
        {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            std::string item = s.substr(pos, end - pos);
            pos = end + 1;

            size_t colon = item.find(':');
            if (colon == std::string::npos || colon == 0) continue;
            Entry e;
            e.at = std::strtod(item.substr(0, colon).c_str(), nullptr);
            e.text = item.substr(colon + 1);
            if (e.text.empty()) continue;
            g_script.push_back(std::move(e));
        }

        for (const Entry& e : g_script)
            LOGI("console: will run \"%s\" at %.2f s", e.text.c_str(), e.at);
    }

    double SinceStart()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    }
}

void console::RunNow(const char* text)
{
    std::lock_guard g(g_lock);
    if (!g_parsed) Parse();
    Entry e;
    e.at = 0.0;
    e.text = text;
    g_script.push_back(std::move(e));
}

void console::Place(const char* text)
{
    std::lock_guard g(g_lock);
    g_place = text;
}

bool console::Take(std::string& text, bool& placed)
{
    std::lock_guard g(g_lock);
    if (!g_parsed) Parse();
    if (g_script.empty() && g_place.empty()) return false;

    const double now = SinceStart();
    for (Entry& e : g_script)
    {
        if (e.done || now < e.at) continue;
        e.done = true;
        text = e.text;
        placed = false;
        return true;                        // one per pump; they queue anyway
    }
    if (g_place.empty()) return false;
    text = std::move(g_place);
    g_place.clear();
    placed = true;
    return true;
}

double console::Elapsed()
{
    std::lock_guard g(g_lock);
    return SinceStart();
}

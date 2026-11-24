#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest_memory.h"
#include "guest.h"
#include "log.h"
#include "stutters.h"
#include "diagnostics.h"
#include "env.h"
#include "kernel/kernel.h"
#include "kernel/volume.h"
#include "gpu/gpu.h"
#include "gpu/vulkan/pipeline.h"
#include "gpu/vulkan/presenter.h"
#include "gpu/vulkan/renderer.h"
#include "gpu/vulkan/shader_build.h"
#include "apu/audio.h"
#include "apu/xma.h"
#include "crash.h"
#include "online/service.h"
#include "engine.h"
#include "pacing_trace.h"
#include "player.h"
#include "watchpoint.h"
#include "install/install.h"
#include "platform.h"
#include "run.h"
#include "report.h"
#include "settings.h"
#if MW2_SPLIT_GAME
#include "android/game_library.h"
#endif

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <vector>

// A run that ends on a deadline and a run that ends on a fault are asking the
// same question, so they print the same reports.
static void ReportAll()
{
    report::Write();
    if constexpr (!diag::kOn) return;
    kernel::ReportUnimplemented();
    kernel::ReportMissingFiles();
    kernel::ReportMemory();
    gpu::Report();
    player::Report();
    engine::ReportPredicateWaits();
    pacing::Write();
    apu::xma::Report();
    apu::audio::Report();
}

void crash::OnFault() { ReportAll(); }

// One shutdown however it is asked for, and on a thread of its own: the caller
// may be the window thread, which stopping the presenter has to join.
void crash::RequestExit(const char* why)
{
    static std::atomic<bool> asked{ false };
    if (asked.exchange(true)) return;
    std::thread([why] {
        LOGI("--------------------------------------------------");
        LOGI("exiting: %s", why);
        crash::StopHangMonitor();
        stutters::Ending();
        gpu::Shutdown();
        ReportAll();
        install::StartNextTitle();
        std::fflush(nullptr);
        _exit(0);
    }).detach();
}

namespace
{
    // The guest never returns, so the reports at the end of main() are unreachable
    // and killing the process loses everything measured. MW2_RUN_SECONDS=<n>
    // reports and exits after n seconds.
    void StartDeadline()
    {
        const unsigned seconds = unsigned(diag::Number("MW2_RUN_SECONDS"));
        if (!seconds) return;
        LOGI("run deadline: reporting and exiting after %u seconds", seconds);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            // The guest threads are still running over state the reports walk;
            // unwinding them cleanly is not worth it for a measurement.
            crash::RequestExit("the run deadline was reached");
        }).detach();
    }

    // MW2_QUIT_AFTER_ARRIVAL=<n> ends the run n seconds after MW2_WALK_TO gets
    // where it was sent. A walk that takes four minutes and a measurement that
    // takes ten seconds should not be followed by two minutes of standing there.
    void StartArrivalDeadline()
    {
        if (!diag::Text("MW2_QUIT_AFTER_ARRIVAL")) return;
        const unsigned seconds = unsigned(diag::Number("MW2_QUIT_AFTER_ARRIVAL"));
        LOGI("run deadline: reporting and exiting %u seconds after arriving", seconds);
        // A walk that has stalled against a wall is as finished as one that
        // arrived -- it will take no more measurements. Ending there rather than
        // at the watchdog saves the rest of the run, which is most of it when
        // the route snags early.
        const double given = diag::Real("MW2_QUIT_IF_STUCK");
        const double stall = given > 0.0 ? given : 45.0;
        LOGI("run deadline: or %0.f seconds after the walk stops getting nearer", stall);
        std::thread([seconds, stall] {
            bool stuck = false;
            while (!player::Arrived())
            {
                if (player::SecondsSinceProgress() > stall) { stuck = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            crash::RequestExit(stuck ? "the walk stopped getting anywhere"
                                     : "the walk arrived and its measurement is done");
        }).detach();
    }

    bool LoadImage(const std::vector<uint8_t>& image)
    {
        if (uint64_t(image.size()) > PPC_IMAGE_SIZE)
            LOGW("image is %zu bytes, larger than PPC_IMAGE_SIZE %llu; truncating",
                 image.size(), (unsigned long long)PPC_IMAGE_SIZE);
        const size_t size = std::min<size_t>(image.size(), size_t(PPC_IMAGE_SIZE));
        std::memcpy(guest::Base() + PPC_IMAGE_BASE, image.data(), size);
        LOGI("loaded %zu bytes at guest %08X", size, uint32_t(PPC_IMAGE_BASE));
        return true;
    }

    // PPC_LOOKUP_FUNC indexes this table by (guest - PPC_CODE_BASE) * 2.
    size_t BuildFunctionTable()
    {
        size_t n = 0;
        // The recompiled code's own table: linked in, or the game library's.
#if MW2_SPLIT_GAME
        const PPCFuncMapping* mappings = game::Mappings();
#else
        const PPCFuncMapping* mappings = PPCFuncMappings;
#endif
        for (const PPCFuncMapping* m = mappings; m->guest != 0 || m->host != nullptr; ++m)
        {
            if (!m->host) continue;
            if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
            {
                LOGW("function %08zX outside code range, skipped", m->guest);
                continue;
            }
            *(PPCFunc**)(guest::Base() + PPC_IMAGE_BASE + PPC_IMAGE_SIZE +
                         (uint64_t(uint32_t(m->guest) - PPC_CODE_BASE) * 2)) = m->host;
            n++;
        }
        return n;
    }
}

int mw2::Run(int argc, char** argv)
{
    platform::Initialise();

    // Before any switch is read, the log's among them.
    const std::string kept = settings::Load(install::Folder(argc, argv) / ".env");

    // MW2_LOG_FILE=<path> sends the log there instead of the terminal. Every
    // line this runtime writes goes to stderr, so a plain `> file` redirection
    // catches nothing.
    // Its folder is made first: a run started with a new folder named lost its
    // whole log to the terminal.
#ifndef MW2_ANDROID
    if (const char* logPath = env::Text("MW2_LOG_FILE"))
    {
        std::error_code ignored;
        if (const auto folder = std::filesystem::path(logPath).parent_path(); !folder.empty())
            std::filesystem::create_directories(folder, ignored);
        // A title the other one started (from its menus) carries on in the
        // log of the run it belongs to.
        if (!std::freopen(logPath, env::Flag("MW2_LOG_APPEND") ? "a" : "w", stderr))
            std::fprintf(stdout, "could not open %s for the log\n", logPath);
        // A line at a time, as stderr is to a terminal: the file is read while
        // the run goes on, and a line held back is a line a watcher never sees.
        // Windows has no line buffering (and takes the request for an invalid
        // one), so there every write goes straight through.
        else
#ifdef _WIN32
            std::setvbuf(stderr, nullptr, _IONBF, 0);
#else
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
#endif
    }
#endif  // MW2_ANDROID

    LOGI("--- MW2 recompilation runtime (%s) ---", MW2_TITLE_NAME);
    if (!kept.empty()) LOGI("settings: %s", kept.c_str());
    // MW2_DRIVER_ENV="NAME=value;NAME=value": variables for the graphics
    // driver (Mesa's TU_DEBUG, IR3_SHADER_DEBUG, ...), set before Vulkan is.
    // .env holds only this runtime's own switches, and on a phone the app's
    // process environment is not otherwise reachable.
    if (const char* list = env::Text("MW2_DRIVER_ENV"))
    {
        std::string all = list;
        for (size_t at = 0; at < all.size();)
        {
            const size_t end = std::min(all.find(';', at), all.size());
            const std::string one = all.substr(at, end - at);
            if (const size_t equals = one.find('='); equals != std::string::npos && equals)
            {
#ifdef _WIN32
                _putenv_s(one.substr(0, equals).c_str(), one.substr(equals + 1).c_str());
#else
                setenv(one.substr(0, equals).c_str(), one.substr(equals + 1).c_str(), 1);
#endif
                LOGI("settings: driver %s", one.c_str());
            }
            at = end + 1;
        }
    }

    // Before any thread exists: a Flatpak's Steam is reached from its own
    // process namespace, which a process of more than one thread cannot join.
    // The parent that waited for the game ends as it did, and runs nothing of
    // the game's on the way out.
    if (int exitCode = 0; !online::EnterServiceNamespace(exitCode))
    {
        std::fflush(nullptr);
        _exit(exitCode);
    }

    // A player starts the executable with no arguments (runtime/install/); a
    // development run names the image, a flat PE or the XEX, and the game folder.
    install::Launch launch;
    const bool playerStart = install::PlayerStart(argc, argv);
    if (playerStart)
    {
        int exitCode = 0;
        if (!install::Prepare(argc, argv, launch, exitCode)) return exitCode;
        // The launcher compiles every shader of the game into this cache
        // before the game first runs (launcher/precompile.cpp), and the game
        // draws only with that done for the device it draws on.
        vk::pipeline::SetCachePath(vk::shaders::kCacheFile);
        vk::renderer::CheckCompiledShaders(launch.gameRoot.string());
    }
    else
    {
        crash::Step("image", "reading and decrypting the executable");
        if (!install::LoadImageFile(argv[1], launch.image, launch.xexHeader)) return 1;
        launch.gameRoot = (argc > 2) ? argv[2] : "mw2/game";
    }

    crash::Install();
    // Logged in before any other thread runs: Steam reads the app id from the
    // environment, which the service sets.
    crash::Step("online", "starting the network service");
    online::Get();

    crash::Step("guest-memory", "reserving the console's 4 GiB of address space and its 512 MB bank");
    if (!guest::Initialise()) return 1;
    if (!LoadImage(launch.image)) return 1;
    launch.image = {};
    watchpoint::Install();

    size_t mapped = BuildFunctionTable();
    LOGI("function table: %zu entries", mapped);

    crash::RegisterThread("main thread");
    // With the title's other threads, and not on the fastest core: that one
    // is the ring consumer's alone (gpu.cpp), and the threads started from
    // here take this thread's cores with them.
    platform::SetThreadAffinity(platform::ThreadAffinityRole::PerformanceCore);
    player::Record();
    crash::Step("kernel", "the console's kernel and the game's files");
    // A player who plays from the disc image has only the executables in the
    // game folder: the image is laid under it, named by the app (or by hand,
    // for a development run).
    if (const char* disc = env::Text("MW2_DISC_IMAGE"))
    {
        std::string error;
        if (!kernel::MountDisc(disc, error))
        {
            LOGE("the disc image %s cannot be read: %s", disc, error.c_str());
            return 1;
        }
    }
    kernel::Initialise(launch.gameRoot, launch.xexHeader);
    launch.xexHeader.clear();
    crash::Step("gpu", "bringing up the renderer");
    gpu::Initialise();
    // The device is known now, and nothing of the title has run: the launcher
    // compiles the shaders for it and starts the game again. Ended at once,
    // without writing this run's pipeline cache over the one the launcher is
    // about to make.
    if (std::string why, device; playerStart && !vk::renderer::CompiledShaders(why, device) &&
                                  install::CompileShaders(device, why))
    {
        std::fflush(nullptr);
        _exit(0);
    }
    vk::ShowWindow();
    apu::xma::Initialise();

    // PPC grows down; leave a zeroed backchain at the top.
    constexpr uint32_t kStackSize = 1u << 20;
    uint32_t stackBase = kernel::AllocateGuest(kStackSize, 0x1000);
    if (!stackBase) { LOGE("could not allocate guest stack"); return 1; }
    uint32_t stackTop = (stackBase + kStackSize - 0x100) & ~0xFu;

    // r13 is the Xbox 360 thread pointer (KPCR). The recompiled code makes ~1100
    // r13-relative accesses and reads its own thread id out of
    // KPCR.CurrentThread -> KTHREAD.ThreadId, so the block must be populated.
    uint32_t pcr = kernel::CreateThreadBlock(1);
    if (!pcr) { LOGE("could not allocate the thread block"); return 1; }

    PPCContext ctx{};
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.r1.u32 = stackTop;
    ctx.r13.u32 = pcr;
    ctx.fpscr.loadFromHost();

    uint32_t entry = T_ENTRY_POINT;   // the XEX header's ENTRY_POINT, from title.h
    PPCFunc* fn = kernel::GuestFunction(entry);
    if (!fn) { LOGE("no recompiled function at entry point %08X", entry); return 1; }

    LOGI("thread block (r13) at %08X", pcr);
    StartDeadline();
    StartArrivalDeadline();
    crash::Step("guest", "start-up is done; the title's own code runs from here");
    LOGI("entering guest at %08X (stack %08X..%08X)", entry, stackBase, stackTop);
    LOGI("--------------------------------------------------");
    fn(ctx, guest::Base());
    LOGI("--------------------------------------------------");
    LOGI("guest returned, r3 = %08X", ctx.r3.u32);

    gpu::Shutdown();
    ReportAll();
    return 0;
}

// A desktop build starts here; Android starts the same run from jni.cpp.
#ifndef MW2_ANDROID
int main(int argc, char** argv) { return mw2::Run(argc, argv); }
#endif

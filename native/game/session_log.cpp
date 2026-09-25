// Session log and hang catcher for play sessions launched from the Finder.
//
// A double-clicked app has no terminal, so everything it prints to stderr is
// lost -- including the runtime's own fatal messages, which exit without a
// crash report. When launchd is the parent (a Finder or `open` launch), or
// KNIFE_EDGE_SESSION_LOG is set, stderr goes to
// ~/Library/Logs/KnifeEdge/session-<time>.log with the overlay and pipeline
// traces on. Runs started from a shell keep stderr as it is.
//
// A wedge prints nothing at all, and the evidence that finds one is a sample
// of where every guest thread is sitting. So a watchdog watches the graphics
// heartbeat, and after 8 seconds without a new graphics task it runs
// /usr/bin/sample on this process into hang-<time>.txt beside the log: once per
// stall, noting when the game recovers. Opening the in-game menu can pause
// drawing too, so a hang file is a lead, not a verdict.

#include "session_log.h"

#include <spawn.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "pipeline_trace.h"

extern char** environ;

namespace {

std::filesystem::path log_dir;

std::string timestamp() {
    char out[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", std::localtime(&now));
    return out;
}

void watch_for_hangs() {
    constexpr int stall_seconds = 8;
    long long last = -1;
    int still = 0;
    bool stalled = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const long long now = knife_edge_gfx_tasks_total();
        if (now == 0) {
            continue;  // still at the launcher or booting: nothing drawn yet
        }
        if (now != last) {
            if (stalled) {
                std::fprintf(stderr, "[hang] recovered at %lld graphics tasks\n", now);
                stalled = false;
            }
            last = now;
            still = 0;
            continue;
        }
        if (++still != stall_seconds || stalled) {
            continue;
        }
        stalled = true;
        const std::filesystem::path out = log_dir / ("hang-" + timestamp() + ".txt");
        std::fprintf(stderr, "[hang] no graphics task for %d s after %lld; sampling threads to %s\n",
                     stall_seconds, now, out.c_str());
        const std::string pid = std::to_string(getpid());
        const std::string file = out.string();
        const char* argv[] = {"/usr/bin/sample", pid.c_str(), "3", "-file", file.c_str(), nullptr};
        pid_t child = 0;
        posix_spawn(&child, "/usr/bin/sample", nullptr, nullptr, const_cast<char* const*>(argv), environ);
    }
}

}  // namespace

extern "C" void knife_edge_session_log_start(void) {
    if (getppid() != 1 && std::getenv("KNIFE_EDGE_SESSION_LOG") == nullptr) {
        return;  // started from a shell: its stderr is already being read
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return;
    }
    log_dir = std::filesystem::path(home) / "Library/Logs/KnifeEdge";
    std::error_code error;
    std::filesystem::create_directories(log_dir, error);
    if (error) {
        return;
    }
    const std::string stamp = timestamp();
    const std::filesystem::path path = log_dir / ("session-" + stamp + ".log");
    if (std::freopen(path.c_str(), "w", stderr) == nullptr) {
        return;
    }
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    // Both traces read these once, on first use, so they must be set before any
    // guest code runs. Explicit settings from the environment still win.
    setenv("KNIFE_EDGE_TRACE_OVERLAYS", "1", 0);
    setenv("KNIFE_EDGE_TRACE_PIPELINE", "1", 0);
    std::fprintf(stderr, "[session] started %s, pid %d\n", stamp.c_str(), getpid());
    std::thread(watch_for_hangs).detach();
}

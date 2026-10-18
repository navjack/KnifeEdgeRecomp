// Session log and hang catcher for play sessions nobody is watching a terminal
// for.
//
// A double-clicked app has no terminal, so everything it prints to stderr is
// lost -- including the runtime's own fatal messages, which exit without a
// crash report. On macOS, when launchd is the parent (a Finder or `open`
// launch), and on every platform when KNIFE_EDGE_SESSION_LOG is set, stderr
// goes to session-<time>.log with the overlay and pipeline traces on:
//
//   macOS    ~/Library/Logs/KnifeEdge/
//   Linux    $XDG_STATE_HOME/KnifeEdgeRecompiled/logs/ (~/.local/state/...)
//   Windows  %LOCALAPPDATA%\KnifeEdgeRecompiled\logs\
//
// Runs started from a shell keep stderr as it is.
//
// A wedge prints nothing at all, and the evidence that finds one is a sample
// of where every guest thread is sitting. So a watchdog watches the graphics
// heartbeat, and after 8 seconds without a new graphics task it logs the stall
// once and notes when the game recovers. On macOS it also runs /usr/bin/sample
// on this process into hang-<time>.txt beside the log. Opening the in-game menu
// can pause drawing too, so a hang is a lead, not a verdict.

#include "session_log.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <spawn.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "pipeline_trace.h"

#if defined(__APPLE__)
extern char** environ;
#endif

namespace {

std::filesystem::path log_dir;

std::string timestamp() {
    char out[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", std::localtime(&now));
    return out;
}

int process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

std::filesystem::path env_path(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path default_log_dir() {
#if defined(__APPLE__)
    const std::filesystem::path home = env_path("HOME");
    return home.empty() ? home : home / "Library/Logs/KnifeEdge";
#elif defined(_WIN32)
    const std::filesystem::path local = env_path("LOCALAPPDATA");
    return local.empty() ? local : local / "KnifeEdgeRecompiled" / "logs";
#else
    const std::filesystem::path state = env_path("XDG_STATE_HOME");
    if (!state.empty()) {
        return state / "KnifeEdgeRecompiled" / "logs";
    }
    const std::filesystem::path home = env_path("HOME");
    return home.empty() ? home : home / ".local/state/KnifeEdgeRecompiled/logs";
#endif
}

// Whether nothing is reading stderr. Only macOS can tell cheaply: a Finder or
// `open` launch has launchd, pid 1, as its parent. Elsewhere the log is opt-in.
bool launched_without_terminal() {
#if defined(__APPLE__)
    return getppid() == 1;
#else
    return false;
#endif
}

// Sets an environment variable unless it is already set.
void set_default_env(const char* name, const char* value) {
    if (std::getenv(name) != nullptr) {
        return;
    }
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

void sample_threads() {
#if defined(__APPLE__)
    const std::filesystem::path out = log_dir / ("hang-" + timestamp() + ".txt");
    std::fprintf(stderr, "[hang] sampling threads to %s\n", out.string().c_str());
    const std::string pid = std::to_string(process_id());
    const std::string file = out.string();
    const char* argv[] = {"/usr/bin/sample", pid.c_str(), "3", "-file", file.c_str(), nullptr};
    pid_t child = 0;
    posix_spawn(&child, "/usr/bin/sample", nullptr, nullptr, const_cast<char* const*>(argv), environ);
#endif
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
        std::fprintf(stderr, "[hang] no graphics task for %d s after %lld\n", stall_seconds, now);
        sample_threads();
    }
}

}  // namespace

extern "C" void knife_edge_session_log_start(void) {
    if (!launched_without_terminal() && std::getenv("KNIFE_EDGE_SESSION_LOG") == nullptr) {
        return;  // started from a shell: its stderr is already being read
    }
    log_dir = default_log_dir();
    if (log_dir.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(log_dir, error);
    if (error) {
        return;
    }
    const std::string stamp = timestamp();
    const std::filesystem::path path = log_dir / ("session-" + stamp + ".log");
    if (std::freopen(path.string().c_str(), "w", stderr) == nullptr) {
        return;
    }
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    // Both traces read these once, on first use, so they must be set before any
    // guest code runs. Explicit settings from the environment still win.
    set_default_env("KNIFE_EDGE_TRACE_OVERLAYS", "1");
    set_default_env("KNIFE_EDGE_TRACE_PIPELINE", "1");
    std::fprintf(stderr, "[session] started %s, pid %d\n", stamp.c_str(), process_id());
    std::thread(watch_for_hangs).detach();
}

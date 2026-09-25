#define SDL_MAIN_HANDLED

#include "SDL.h"
#include "SDL_metal.h"
#include "SDL_syswm.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "game_config.hpp"
#include "game/pipeline_trace.h"
#include "game/clock.h"
#include "game/guest_waits.h"
#include "game/session_log.h"
#include "librecomp/game.hpp"
#include "nfd.h"
#include "recomp.h"
#include "macos_menu.h"
#include "recompinput/input_events.h"
#include "recompinput/input_mapping.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompinput/recompinput.h"
#include "recompui/config.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "game/overlay_bridge.h"
#include "rsp_contract.hpp"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"

extern "C" void knife_edge_register_generated_code();
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// recompui and recompinput resolve these globals by name.
SDL_Window* window = nullptr;
std::vector<recomp::GameEntry> supported_games;

namespace {

SDL_MetalView metal_view = nullptr;

std::mutex audio_mutex;
SDL_AudioDeviceID audio_device = 0;
SDL_AudioStream* audio_stream = nullptr;
uint32_t audio_device_frequency = 0;
std::vector<int16_t> audio_volume_buffer;
std::vector<uint8_t> audio_conversion_buffer;
std::atomic_uint32_t audio_frequency = 0;
std::atomic_uint64_t audio_buffers_queued = 0;
std::atomic_uint64_t audio_nonzero_samples_queued = 0;

std::mutex trace_mutex;
std::ofstream trace_file;
std::ofstream function_trace_file;
std::atomic_uint64_t vi_count = 0;
uint64_t stop_after_vi = 0;

// Guest words to sample once per VI, appended to the trace as extra columns.
// The point is to watch the game's own state evolve without a debugger: a
// pending-work counter that never drains, or a message queue whose validCount
// sticks at 1, says far more than a backtrace of a thread that is merely
// parked. Captured from the render-context callback, the one place the
// frontend is handed the RDRAM base.
std::vector<uint32_t> watch_addresses;
std::atomic<uint8_t*> watched_rdram{nullptr};

int32_t read_guest_word(uint8_t* rdram, uint32_t vram) {
    int32_t value{};
    std::memcpy(&value, rdram + (vram & 0x3FFFFFFFu), sizeof(value));
    return value;
}
std::filesystem::path autoboot_rom_path;
// --launcher keeps the launcher up instead of starting the game.
bool show_launcher = false;

// A scripted input segment holds its buttons and stick for guest VIs
// [start_vi, end_vi). Scripts are keyed to guest time so they reproduce
// independently of host scheduling, and they replace live input for player 1.
struct InputScriptSegment {
    uint64_t start_vi;
    uint64_t end_vi;
    uint16_t buttons;
    float stick_x;
    float stick_y;
};

struct ScriptedInput {
    uint16_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
};

std::vector<InputScriptSegment> input_script;
bool input_script_enabled = false;
std::mutex script_mutex;
ScriptedInput scripted_input;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "%s: %s\n", message, SDL_GetError());
    std::exit(EXIT_FAILURE);
}

void* create_gfx() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        fail("SDL initialization failed");
    }
    // After SDL_Init, because that is what creates the NSApplication.
    knife_edge_install_macos_menu(KNIFE_EDGE_DISPLAY_NAME);
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(void*) {
    const uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
    window = SDL_CreateWindow(KNIFE_EDGE_WINDOW_TITLE,
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              1280,
                              960,
                              flags);
    if (window == nullptr) {
        fail("SDL window creation failed");
    }

    SDL_SysWMinfo wm_info{};
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE) {
        fail("SDL native-window lookup failed");
    }

    // RT64 renders through Metal: plume expects the NSWindow and its CAMetalLayer.
    metal_view = SDL_Metal_CreateView(window);
    if (metal_view == nullptr) {
        fail("SDL Metal view creation failed");
    }
    return ultramodern::renderer::WindowHandle{
        .window = wm_info.info.cocoa.window,
        .view = SDL_Metal_GetLayer(metal_view),
    };
}

void update_gfx(void*) {
    recompinput::handle_events();
}

void poll_input() {
    recompinput::poll_inputs();
    if (!input_script_enabled) {
        return;
    }

    const uint64_t vi = vi_count.load(std::memory_order_relaxed);
    ScriptedInput next;
    // KNIFE_EDGE_SCRIPT_UNTIL_LEVEL: stop scripted input once a level starts,
    // so a script can mash through menus of any pacing without skipping the
    // level's opening transmission.
    static const bool until_level = std::getenv("KNIFE_EDGE_SCRIPT_UNTIL_LEVEL") != nullptr;
    const bool stopped = until_level && knife_edge_level_callback_installed.load();
    for (const InputScriptSegment& segment : stopped ? std::vector<InputScriptSegment>{} : input_script) {
        if (vi >= segment.start_vi && vi < segment.end_vi) {
            next.buttons |= segment.buttons;
            if (segment.stick_x != 0.0f) next.stick_x = segment.stick_x;
            if (segment.stick_y != 0.0f) next.stick_y = segment.stick_y;
        }
    }
    std::lock_guard lock{script_mutex};
    if (next.buttons != scripted_input.buttons) {
        std::fprintf(stderr, "[input-script] vi=%llu buttons=%04X stick=%.2f,%.2f\n",
                     static_cast<unsigned long long>(vi), next.buttons, next.stick_x, next.stick_y);
    }
    scripted_input = next;
}

bool get_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    if (input_script_enabled) {
        ScriptedInput snapshot;
        if (controller_num == 0) {
            std::lock_guard lock{script_mutex};
            snapshot = scripted_input;
        }
        *buttons_out = snapshot.buttons;
        *x_out = snapshot.stick_x;
        *y_out = snapshot.stick_y;
        return true;
    }
    return recompinput::profiles::get_n64_input(controller_num, buttons_out, x_out, y_out);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    // Single-player mode routes every host device to player 1. Reporting only
    // port 1 keeps the game from starting a 2-player session nobody joined.
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

void queue_samples(int16_t* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) {
        return;
    }
    // KNIFE_EDGE_DUMP_AUDIO=<file>: the guest's own samples, raw interleaved
    // stereo s16 at the AI rate, before volume or resampling. Used to measure
    // whether the music's tempo follows the game's step rate.
    static FILE* audio_dump = [] {
        const char* path = std::getenv("KNIFE_EDGE_DUMP_AUDIO");
        return path != nullptr ? std::fopen(path, "wb") : nullptr;
    }();
    if (audio_dump != nullptr) {
        std::fwrite(samples, sizeof(int16_t), sample_count, audio_dump);
    }
    const float volume = std::clamp(
        static_cast<float>(recompui::config::sound::get_main_volume() / 100.0), 0.0f, 1.0f);

    std::lock_guard lock{audio_mutex};
    if (audio_device == 0 || audio_stream == nullptr) {
        return;
    }
    audio_volume_buffer.resize(sample_count);
    uint64_t nonzero_samples = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        nonzero_samples += samples[i] != 0;
        audio_volume_buffer[i] = static_cast<int16_t>(static_cast<float>(samples[i]) * volume);
    }
    const int source_bytes = static_cast<int>(sample_count * sizeof(int16_t));
    if (SDL_AudioStreamPut(audio_stream, audio_volume_buffer.data(), source_bytes) != 0) {
        std::fprintf(stderr, "Unable to resample guest audio: %s\n", SDL_GetError());
        return;
    }
    const int converted_bytes = SDL_AudioStreamAvailable(audio_stream);
    if (converted_bytes > 0) {
        audio_conversion_buffer.resize(static_cast<size_t>(converted_bytes));
        const int received_bytes = SDL_AudioStreamGet(
            audio_stream, audio_conversion_buffer.data(), converted_bytes);
        if (received_bytes > 0) {
            SDL_QueueAudio(audio_device, audio_conversion_buffer.data(), received_bytes);
        }
    }
    audio_buffers_queued.fetch_add(1, std::memory_order_relaxed);
    audio_nonzero_samples_queued.fetch_add(nonzero_samples, std::memory_order_relaxed);

    // KNIFE_EDGE_AUDIO_LOG=<file>: one line per guest buffer -- host ms, the
    // guest sample position it starts at, its length, how much output was
    // already queued ahead of it (its latency), and the game step it was made
    // during. With KNIFE_EDGE_DUMP_AUDIO this ties every sound to a step.
    static FILE* audio_log = [] {
        const char* path = std::getenv("KNIFE_EDGE_AUDIO_LOG");
        FILE* file = path != nullptr ? std::fopen(path, "w") : nullptr;
        if (file != nullptr) {  // wall clock at host ms 0, to line up with screen captures
            std::fprintf(file, "# epoch_ms %.3f\n", std::chrono::duration<double, std::milli>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }
        return file;
    }();
    if (audio_log != nullptr) {
        static const auto start = std::chrono::steady_clock::now();
        static uint64_t position = 0;
        const double now_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const double queued_ms = SDL_GetQueuedAudioSize(audio_device) / (2.0 * sizeof(int16_t)) * 1000.0 /
                                 static_cast<double>(audio_device_frequency);
        std::fprintf(audio_log, "%.2f %llu %zu %.2f %lld\n", now_ms, static_cast<unsigned long long>(position),
                     sample_count / 2, queued_ms, knife_edge_gfx_tasks_total());
        position += sample_count / 2;
    }
}

size_t get_frames_remaining() {
    std::lock_guard lock{audio_mutex};
    if (audio_device == 0 || audio_device_frequency == 0) {
        return 0;
    }
    const uint64_t output_frames = SDL_GetQueuedAudioSize(audio_device) / (2 * sizeof(int16_t));
    const uint64_t guest_frequency = audio_frequency.load(std::memory_order_relaxed);
    return static_cast<size_t>((output_frames * guest_frequency + audio_device_frequency - 1) /
                               audio_device_frequency);
}

void set_frequency(uint32_t frequency) {
    constexpr uint32_t kHostOutputFrequency = 48000;
    audio_frequency.store(frequency, std::memory_order_relaxed);
    std::lock_guard lock{audio_mutex};
    if (audio_device == 0) {
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(kHostOutputFrequency);
        desired.format = AUDIO_S16LSB;
        desired.channels = 2;
        desired.samples = 0x100;
        SDL_AudioSpec obtained{};
        audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (audio_device == 0) {
            std::fprintf(stderr, "Unable to open host audio device: %s\n", SDL_GetError());
            return;
        }
        audio_device_frequency = static_cast<uint32_t>(obtained.freq);
        SDL_PauseAudioDevice(audio_device, 0);
    }
    if (audio_stream != nullptr) {
        SDL_FreeAudioStream(audio_stream);
    }
    audio_stream = SDL_NewAudioStream(
        AUDIO_S16LSB, 2, static_cast<int>(frequency),
        AUDIO_S16LSB, 2, static_cast<int>(audio_device_frequency));
    if (audio_stream == nullptr) {
        std::fprintf(stderr, "Unable to create %u Hz to %u Hz audio stream: %s\n",
                     frequency, audio_device_frequency, SDL_GetError());
    }
}

// KNIFE_EDGE_POKE=<vram>=<value>[,...]: rewrite guest words on every VI. A
// test harness only -- e.g. pinning the ship's health (0x8011D464) so an
// unattended run survives to a timeline marker. Never used in normal play.
const std::vector<std::pair<uint32_t, uint32_t>>& pokes() {
    static const std::vector<std::pair<uint32_t, uint32_t>> all = [] {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        if (const char* spec = std::getenv("KNIFE_EDGE_POKE")) {
            std::string text = spec;
            for (size_t at = 0; at < text.size();) {
                const size_t next = text.find(',', at);
                const std::string item = text.substr(at, next - at);
                const size_t eq = item.find('=');
                if (eq != std::string::npos) {
                    out.emplace_back(static_cast<uint32_t>(std::stoul(item.substr(0, eq), nullptr, 0)),
                                     static_cast<uint32_t>(std::stoul(item.substr(eq + 1), nullptr, 0)));
                }
                at = next == std::string::npos ? text.size() : next + 1;
            }
        }
        return out;
    }();
    return all;
}

void vi_callback() {
    knife_edge_clock_vi();
    if (!pokes().empty()) {
        if (uint8_t* const rdram = watched_rdram.load(std::memory_order_relaxed)) {
            for (const auto& [vram, value] : pokes()) {
                *reinterpret_cast<uint32_t*>(rdram + (vram - 0x80000000u)) = value;
            }
        }
    }
    recompinput::update_rumble();
    const uint64_t current_vi = vi_count.fetch_add(1) + 1;
    if (trace_file.is_open()) {
        std::lock_guard lock{trace_mutex};
        trace_file << current_vi << ',' << (ultramodern::is_game_started() ? 1 : 0)
                   << ',' << audio_frequency.load(std::memory_order_relaxed)
                   << ',' << audio_buffers_queued.load(std::memory_order_relaxed)
                   << ',' << audio_nonzero_samples_queued.load(std::memory_order_relaxed);
        uint8_t* const rdram = watched_rdram.load(std::memory_order_relaxed);
        for (const uint32_t address : watch_addresses) {
            trace_file << ',';
            if (rdram != nullptr) {
                trace_file << read_guest_word(rdram, address);
            }
        }
        trace_file << '\n';
        trace_file.flush();
    }
    if (stop_after_vi != 0 && current_vi >= stop_after_vi) {
        // Leave immediately rather than unwinding. recomp::start joins the
        // game thread and the event threads on the way out, but never the
        // guest OSThreads osCreateThread spawned, so they keep running while
        // it unmaps RDRAM underneath them -- every bounded run ended in a
        // SIGSEGV that had nothing to do with the game, and enough of them in
        // a row got macOS to start showing its "reopen windows?" dialog on
        // launch, which blocks the next run in SDL_CreateWindow.
        //
        // A bounded run is a deliberate stop, not a shutdown, so there is
        // nothing to unwind: flush what the run was asked to record and go.
        // Closing the window normally still takes the ordinary path.
        if (trace_file.is_open()) {
            std::lock_guard lock{trace_mutex};
            trace_file.flush();
        }
        if (function_trace_file.is_open()) {
            std::lock_guard lock{trace_mutex};
            function_trace_file.flush();
        }
        knife_edge_pipeline_totals();
        knife_edge_guest_waits_totals();
        knife_edge_rdp_gate_totals();
        knife_edge_cpu_cost_totals();
        std::fflush(nullptr);
        std::_Exit(EXIT_SUCCESS);
    }
}

std::string get_thread_name(const OSThread* thread) {
    return std::string("[Knife Edge] ") + std::to_string(thread != nullptr ? thread->id : -1);
}

// Script lines: "<start_vi> <end_vi> <buttons_hex> [stick_x stick_y]".
// Blank lines and lines starting with '#' are ignored.
bool load_input_script(const std::filesystem::path& path) {
    std::ifstream script_file{path};
    if (!script_file) {
        std::fprintf(stderr, "Unable to open input script '%s'\n", path.c_str());
        return false;
    }
    std::string line;
    size_t line_number = 0;
    while (std::getline(script_file, line)) {
        ++line_number;
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        unsigned long long start_vi = 0;
        unsigned long long end_vi = 0;
        unsigned int buttons = 0;
        float stick_x = 0.0f;
        float stick_y = 0.0f;
        const int fields = std::sscanf(line.c_str(), "%llu %llu %x %f %f",
                                       &start_vi, &end_vi, &buttons, &stick_x, &stick_y);
        if ((fields != 3 && fields != 5) || end_vi <= start_vi || buttons > 0xFFFF) {
            std::fprintf(stderr, "Invalid input script line %zu in '%s'\n", line_number, path.c_str());
            return false;
        }
        input_script.push_back(InputScriptSegment{
            .start_vi = start_vi,
            .end_vi = end_vi,
            .buttons = static_cast<uint16_t>(buttons),
            .stick_x = stick_x,
            .stick_y = stick_y,
        });
    }
    input_script_enabled = true;
    return true;
}

bool parse_runtime_options(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        const bool has_value = i + 1 < argc;
        if (argument == "--rom") {
            if (!has_value) {
                std::fprintf(stderr, "--rom requires a file path\n");
                return false;
            }
            autoboot_rom_path = argv[++i];
        }
        else if (argument == "--stop-after-vi") {
            if (!has_value) {
                std::fprintf(stderr, "--stop-after-vi requires a count\n");
                return false;
            }
            char* end = nullptr;
            const unsigned long long value = std::strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value == 0) {
                std::fprintf(stderr, "Invalid --stop-after-vi count: '%s'\n", argv[i]);
                return false;
            }
            stop_after_vi = static_cast<uint64_t>(value);
        }
        else if (argument == "--launcher") {
            show_launcher = true;
        }
        else if (argument == "--watch") {
            if (!has_value) {
                std::fprintf(stderr, "--watch requires a guest address\n");
                return false;
            }
            char* end = nullptr;
            const unsigned long long value = std::strtoull(argv[++i], &end, 16);
            if (end == argv[i] || *end != '\0') {
                std::fprintf(stderr, "Invalid --watch address: '%s'\n", argv[i]);
                return false;
            }
            watch_addresses.push_back(static_cast<uint32_t>(value));
        }
        else if (argument == "--trace-output") {
            if (!has_value) {
                std::fprintf(stderr, "--trace-output requires a file path\n");
                return false;
            }
            trace_file.open(argv[++i], std::ios::out | std::ios::trunc);
            if (!trace_file) {
                std::fprintf(stderr, "Unable to open trace output '%s'\n", argv[i]);
                return false;
            }
            trace_file << "vi,game_started,audio_frequency,audio_buffers,audio_nonzero_samples";
            for (const uint32_t address : watch_addresses) {
                trace_file << ",0x" << std::hex << address << std::dec;
            }
            trace_file << '\n';
        }
        else if (argument == "--function-trace-output") {
            if (!has_value) {
                std::fprintf(stderr, "--function-trace-output requires a file path\n");
                return false;
            }
            function_trace_file.open(argv[++i], std::ios::out | std::ios::trunc);
            if (!function_trace_file) {
                std::fprintf(stderr, "Unable to open function trace output '%s'\n", argv[i]);
                return false;
            }
            function_trace_file << "event,function\n";
        }
        else if (argument == "--input-script") {
            if (!has_value) {
                std::fprintf(stderr, "--input-script requires a file path\n");
                return false;
            }
            if (!load_input_script(argv[++i])) {
                return false;
            }
        }
    }
    return true;
}

void set_default_bindings() {
    // The stock recomp defaults are complete and playable, so this changes one
    // thing only: N64 Z fires the gun in a nose-gunner game, and the stock
    // mapping puts Z on the left trigger because that is where Z physically
    // sits on an N64 pad. Faithful to the hardware, wrong for the hands -- on
    // any modern pad the trigger you expect to shoot with is the right one.
    //
    // Binding Z to both triggers rather than moving it: nothing is taken away,
    // whichever the player reaches for works, and a left-handed or
    // single-trigger setup is not broken to fix an ergonomic default. R stays
    // on the right trigger as well, which is harmless -- the game does not use
    // R, and anyone who finds otherwise can rebind from Setup Controls.
    recompinput::set_default_mapping_for_controller(
        recompinput::GameInput::Z,
        {
            recompinput::InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true),
            recompinput::InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true),
        });
}

void init_config() {
    const std::filesystem::path config_folder = recompui::file::get_app_folder_path();
    const bool first_general_config = !std::filesystem::exists(config_folder / "general.json");

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = false;
    // Tab creation can reallocate recompui's config storage, so references
    // returned here are only used before the next tab is created.
    recompui::config::create_general_tab(general_options);
    recomp::config::Config& graphics = recompui::config::create_graphics_tab();
    // Dev Mode enables RT64's ImGui inspector (F1 in game).
    graphics.update_option_hidden(recompui::config::graphics::options::developer_mode, false);
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::finalize();

    // Sticks default to no deadzone. Only a brand-new configuration is changed,
    // so a deadzone the player chose is never overwritten.
    if (first_general_config) {
        recomp::config::Config& general = recompui::config::get_config(recompui::config::general::id);
        general.set_option_value(recompui::config::general::options::joystick_deadzone, 0.0);
        general.save_config();
    }

    // Authentic presentation: 4:3, the game's own refresh (no interpolated
    // frames), native resolution and no multisampling -- forced every launch,
    // over whatever graphics.json holds, and hidden so nothing drifts back.
    // Beyond being the chosen look, MSAA must stay off for correctness: RT64's
    // MSAA resolve (rt64_workload_queue.cpp, `overrideTarget->resize(...,
    // colorTarget->width, ...)`) dereferences a null colour target when the
    // selected framebuffer pair has none, which a skipped frame produces, and
    // this game skips frames whenever drawing runs long.
    namespace gfx = recompui::config::graphics::options;
    recomp::config::Config& graphics_config = recompui::config::get_config(recompui::config::graphics::id);
    graphics_config.set_option_value(gfx::ar_option, static_cast<uint32_t>(ultramodern::renderer::AspectRatio::Original));
    graphics_config.set_option_value(gfx::rr_option, static_cast<uint32_t>(ultramodern::renderer::RefreshRate::Original));
    graphics_config.set_option_value(gfx::msaa_option, static_cast<uint32_t>(ultramodern::renderer::Antialiasing::None));
    graphics_config.set_option_value(gfx::res_option, static_cast<uint32_t>(ultramodern::renderer::Resolution::Original));
    for (const std::string* option : {&gfx::ar_option, &gfx::rr_option, &gfx::rr_manual_value,
                                      &gfx::msaa_option, &gfx::res_option, &gfx::hr_option, &gfx::ds_option}) {
        graphics_config.update_option_hidden(*option, true);
    }
    graphics_config.save_config();
}

void register_launcher() {
    recompui::register_launcher_init_callback([](recompui::LauncherMenu* menu) {
        const recomp::GameEntry& game = supported_games[0];
        recompui::update_game_mod_id(game.mod_game_id);
        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            game.game_id, game.mod_game_id, game.display_name, game.thumbnail_bytes,
            recompui::GameOptionsMenuLayout::Center);
        options->add_start_game_or_load_rom_option();
        options->add_setup_controls_option();
        options->add_settings_option();
        options->add_exit_option();
    });

    // Skip the launcher once a valid ROM is installed. The frontend shows the
    // launcher every UI frame until the game starts and never hides it itself --
    // only its Start Game button does, with start_game followed by
    // hide_all_contexts. Starting the game any other way leaves the launcher
    // drawn over it. This runs on the UI thread each frame the launcher is up,
    // the same thread the button's callback runs on, and does the same thing
    // once. Without a ROM it does nothing, so the launcher can still load one.
    // Escape opens the in-game menu for settings and controls.
    recompui::register_launcher_update_callback([](recompui::LauncherMenu*) {
        static bool started = false;
        if (started || show_launcher) {
            return;
        }
        const recomp::GameEntry& game = supported_games[0];
        if (!recomp::is_rom_valid(game.game_id)) {
            return;
        }
        started = true;
        recompui::update_game_mod_id(game.mod_game_id);
        recomp::start_game(game.game_id, {});
        recompui::hide_all_contexts();
    });
}

}

extern "C" uint64_t knife_edge_vi_count() {
    return vi_count.load(std::memory_order_relaxed);
}

// The game does spin-wait, in several places. knife_edge_guest_poll() lives in
// native/game/guest_waits.cpp and is attached by the hooks
// tools/spin-scan/find_spin_loops.py generates; no runtime patch was needed,
// because ultramodern already exports wait_for_external_message_timed and
// check_running_queue.

extern "C" void knife_edge_trace_entry(const char* function_name) {
    if (function_trace_file.is_open()) {
        std::lock_guard lock{trace_mutex};
        function_trace_file << "entry," << function_name << '\n';
        function_trace_file.flush();
    }
}

extern "C" void knife_edge_trace_return(const char* function_name) {
    if (function_trace_file.is_open()) {
        std::lock_guard lock{trace_mutex};
        function_trace_file << "return," << function_name << '\n';
        function_trace_file.flush();
    }
}

// Once the game is running, SDL_QUIT opens a confirmation prompt instead of
// quitting -- which is unusable if rendering is broken, because the prompt is
// part of what is not being drawn. An app you cannot quit is broken no matter
// what else is wrong, so a second Cmd-Q means it: the first still asks, and
// pressing it again within a few seconds leaves immediately.
//
// It leaves the same way a bounded run does, with _Exit after a flush, because
// the ordinary teardown is the one that unmaps RDRAM out from under guest
// threads it never joined.
int SDLCALL quit_watch(void*, SDL_Event* event) {
    if (event == nullptr || event->type != SDL_QUIT) {
        return 0;
    }
    using namespace std::chrono;
    static steady_clock::time_point first_request{};
    const auto now = steady_clock::now();
    if (first_request != steady_clock::time_point{} &&
        now - first_request < seconds{5}) {
        std::fprintf(stderr, "Quit requested twice; exiting without unwinding.\n");
        if (trace_file.is_open()) {
            std::lock_guard lock{trace_mutex};
            trace_file.flush();
        }
        knife_edge_pipeline_totals();
        knife_edge_guest_waits_totals();
        knife_edge_rdp_gate_totals();
        knife_edge_cpu_cost_totals();
        std::fflush(nullptr);
        std::_Exit(EXIT_SUCCESS);
    }
    first_request = now;
    return 0;
}

int main(int argc, char** argv) {
    knife_edge_session_log_start();
    if (!parse_runtime_options(argc, argv)) {
        return EXIT_FAILURE;
    }

    recompui::programconfig::set_program_name(KNIFE_EDGE_PROGRAM_NAME);
    recompui::programconfig::set_program_id(KNIFE_EDGE_PROGRAM_ID);

    const std::filesystem::path app_folder = recompui::file::get_app_folder_path();
    std::error_code app_folder_error;
    std::filesystem::create_directories(app_folder, app_folder_error);
    if (app_folder_error) {
        std::fprintf(stderr, "Unable to create application data directory '%s': %s\n",
                     app_folder.c_str(), app_folder_error.message().c_str());
        return EXIT_FAILURE;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL audio initialization failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_AddEventWatch(quit_watch, nullptr);
    NFD_Init();

    const std::u8string controller_db_path =
        (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db_path.c_str())) < 0) {
        std::fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recomp::register_config_path(app_folder);
    supported_games.push_back(recomp::GameEntry{
        .rom_hash = KNIFE_EDGE_ROM_HASH,
        .internal_name = KNIFE_EDGE_INTERNAL_NAME,
        .display_name = KNIFE_EDGE_DISPLAY_NAME,
        .game_id = KNIFE_EDGE_GAME_ID,
        .mod_game_id = KNIFE_EDGE_MOD_GAME_ID,
        .save_type = KNIFE_EDGE_SAVE_TYPE,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        // Runs after librecomp's init(), so after the boot-time 1 MB
        // load_overlays whose overlay registrations this undoes.
        .on_init_callback = knife_edge_neutralize_boot_overlays,
    });
    if (!recomp::register_game(supported_games[0])) {
        std::fprintf(stderr, "Failed to register the game\n");
        return EXIT_FAILURE;
    }
    knife_edge_register_generated_code();

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompinput::players::set_single_player_mode(true);
    set_default_bindings();
    init_config();
    register_launcher();

    // --rom installs the ROM; the launcher update above then boots it.
    if (!autoboot_rom_path.empty()) {
        const recomp::RomValidationError result =
            recomp::select_rom(autoboot_rom_path, supported_games[0].game_id);
        if (result != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "ROM validation failed (%d)\n", static_cast<int>(result));
            return EXIT_FAILURE;
        }
    }

    recomp::Configuration config{
        .argc = argc,
        .argv = argv,
        .project_version = recomp::Version{0, 1, 0},
        .rsp_callbacks = {.get_rsp_microcode = knife_edge_get_rsp_microcode},
        .renderer_callbacks = {
            .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle,
                                        bool developer_mode) {
                watched_rdram.store(rdram, std::memory_order_relaxed);
                return recompui::renderer::create_render_context(
                    rdram, window_handle, ultramodern::renderer::PresentationMode::Console, developer_mode);
            },
        },
        .audio_callbacks = {
            .queue_samples = queue_samples,
            .get_frames_remaining = get_frames_remaining,
            .set_frequency = set_frequency,
        },
        .input_callbacks = {
            .poll_input = poll_input,
            .get_input = get_input,
            .set_rumble = recompinput::set_rumble,
            .get_connected_device_info = get_connected_device_info,
        },
        .gfx_callbacks = {
            .create_gfx = create_gfx,
            .create_window = create_window,
            .update_gfx = update_gfx,
        },
        .events_callbacks = {
            .vi_callback = vi_callback,
            .gfx_init_callback = nullptr,
        },
        .error_handling_callbacks = {.message_box = recompui::message_box},
        .threads_callbacks = {.get_game_thread_name = get_thread_name},
    };

    recomp::start(config);

    // Leave here rather than unwinding, for the same reason the bounded run
    // and the double-Cmd-Q path do: recomp::start never joins the guest
    // OSThreads it spawned, so they are still running when this function
    // starts freeing the audio device, tearing down the window and running
    // static destructors, and eventually when RDRAM is unmapped underneath
    // them. That race is what both of the crashes seen so far fell out of --
    // a SIGBUS in func_800D2930 reading guest address 1, and a heap-corruption
    // SIGTRAP raised after RmlUi had already reported its shutdown leaks. The
    // process is exiting and the kernel reclaims all of it anyway; the only
    // thing worth doing first is flushing what we have written.
    knife_edge_pipeline_totals();
    std::fflush(nullptr);
    std::_Exit(EXIT_SUCCESS);

    {
        std::lock_guard lock{audio_mutex};
        if (audio_stream != nullptr) {
            SDL_FreeAudioStream(audio_stream);
            audio_stream = nullptr;
        }
        if (audio_device != 0) {
            SDL_CloseAudioDevice(audio_device);
            audio_device = 0;
        }
    }
    if (metal_view != nullptr) {
        SDL_Metal_DestroyView(metal_view);
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
    NFD_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}

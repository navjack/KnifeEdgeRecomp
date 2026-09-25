// Where a submitted graphics task actually gets to.
//
// The game's boot thread builds a one-command display list in func_800D1390,
// posts it with func_800D1BB4, and then busy-waits in func_800D2B40 for the
// pending-task count at 0x8013C280 to fall back to zero. On a stalled boot
// that count sits at 1 forever, and a backtrace cannot say whether the task
// reached osSpTaskStartGo or died in the queue before it: every thread in the
// chain is parked in osRecvMesg either way.
//
// These hooks sit on the guest instructions immediately after each step, so a
// run says which step was the last one taken. Silent unless
// KNIFE_EDGE_TRACE_PIPELINE is set, and each site reports only its first few
// hits -- the interesting question is whether a step happens at all, and an
// unbounded log of a 60 Hz pipeline drowns it.
#include <atomic>
#include <cstring>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"

namespace {

constexpr int report_limit = 4;

bool tracing() {
    static const bool on = std::getenv("KNIFE_EDGE_TRACE_PIPELINE") != nullptr;
    return on;
}

constexpr int report_period = 60;  // one second of NTSC

// Every step that has reported at least once, so the summary can give real
// totals. A step reports its first few and then only every 60th, which means
// counting the printed lines undercounts it by up to sixtyfold -- a trap worth
// closing, because the periodic line and the total look alike at a glance.
std::mutex steps_lock;
std::vector<std::pair<std::string, std::atomic<int>*>>& steps() {
    static std::vector<std::pair<std::string, std::atomic<int>*>> all;
    return all;
}

// Most probes format their line into a stack buffer that dies on return, and
// the tail of that line carries per-call values anyway. Keep a copy of just the
// part before the first ':', which is the step's stable name.
std::string step_name(const char* step) {
    std::string name(step);
    const std::size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(0, colon);
}

void summarize() {
    if (!tracing()) {
        return;
    }
    std::lock_guard<std::mutex> held(steps_lock);
    std::fprintf(stderr, "\n[pipeline] totals (every step, not just the ones printed)\n");
    for (const auto& [step, seen] : steps()) {
        std::fprintf(stderr, "[pipeline]   %-44s %d\n", step.c_str(), seen->load());
    }
    std::fflush(stderr);
}

void report(const char* step, std::atomic<int>& seen) {
    if (!tracing()) {
        return;
    }
    const int count = seen.fetch_add(1) + 1;
    if (count == 1) {
        static const bool once = (std::atexit(summarize), true);
        (void)once;
        std::lock_guard<std::mutex> held(steps_lock);
        steps().emplace_back(step_name(step), &seen);
    }
    // After the first few, report periodically rather than stopping: whether a
    // step happens at all is one question, and whether it keeps happening at
    // frame rate is a different one. The number in parentheses is how many
    // times the step has run, not how many lines have been printed.
    if (count <= report_limit || count % report_period == 0) {
        std::fprintf(stderr, "[pipeline] %s (%d)\n", step, count);
        std::fflush(stderr);
    }
}


// --- Matrix dump -----------------------------------------------------------
//
// KNIFE_EDGE_DUMP_MTX=<i>,<j>,... names graphics tasks by index (0-based, gfx
// tasks only). For each, walk the display list the way F3DEX2 would -- segment
// assignments, G_DL calls and branches, G_ENDDL -- and decode every matrix it
// hands the RSP. The upper 3x3 of a modelview is a rotation times a scale, so
// equal row norms and zero dot products mean the math is sound and only the
// angle fed into it could be wrong; anything else means the math itself is.

uint32_t guest_w(uint8_t* rdram, uint32_t kseg0) {
    return static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kseg0))));
}

int32_t guest_h(uint8_t* rdram, uint32_t kseg0) {
    return MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kseg0)));
}

uint32_t guest_hu(uint8_t* rdram, uint32_t kseg0) {
    return MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kseg0)));
}

bool in_rdram(uint32_t phys, uint32_t len) {
    return phys < 0x800000u && phys + len <= 0x800000u;
}

const std::set<int>& dump_indices() {
    static const std::set<int> wanted = [] {
        std::set<int> out;
        const char* spec = std::getenv("KNIFE_EDGE_DUMP_MTX");
        while (spec != nullptr && *spec != '\0') {
            char* end = nullptr;
            const long value = std::strtol(spec, &end, 10);
            if (end == spec) {
                break;
            }
            out.insert(static_cast<int>(value));
            spec = (*end == ',') ? end + 1 : end;
        }
        return out;
    }();
    return wanted;
}

// Graphics tasks started so far. The display list for task N is built while
// this still reads N, so a probe in drawing code gates on it directly.
std::atomic<int> gfx_tasks_started{0};

// The guMtxF2L call site that ran last, set by a hook just before each jal.
std::atomic<uint32_t> f2l_site{0};

void dump_matrices(uint8_t* rdram, uint32_t display_list, int task_index) {
    uint32_t segment[16] = {};
    std::vector<uint32_t> returns;
    uint32_t pc = display_list & 0x00FFFFFFu;
    int found = 0;
    std::fprintf(stderr, "[mtx] gfx task %d, display list 0x%08X\n", task_index, display_list);
    for (int steps = 0; steps < 50000 && found < 12; steps++) {
        if (!in_rdram(pc, 8)) {
            std::fprintf(stderr, "[mtx]   walked off RDRAM at 0x%06X\n", pc);
            break;
        }
        const uint32_t w0 = guest_w(rdram, 0x80000000u | pc);
        const uint32_t w1 = guest_w(rdram, 0x80000000u | (pc + 4));
        pc += 8;
        const auto resolve = [&](uint32_t address) {
            return segment[(address >> 24) & 0xF] + (address & 0x00FFFFFFu);
        };
        switch (w0 >> 24) {
            case 0xDB:  // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {  // G_MW_SEGMENT
                    segment[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFFu;
                }
                break;
            case 0xDE:  // G_DL: 0 pushes a return, 1 is a plain branch
                if (((w0 >> 16) & 0xFF) == 0) {
                    returns.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // G_ENDDL
                if (returns.empty()) {
                    steps = 1 << 30;
                } else {
                    pc = returns.back();
                    returns.pop_back();
                }
                break;
            case 0xDA: {  // G_MTX; F3DEX2 stores the params XORed with G_MTX_PUSH
                const uint32_t m = resolve(w1);
                const uint32_t params = (w0 & 0xFF) ^ 1u;
                if (!in_rdram(m, 64)) {
                    std::fprintf(stderr, "[mtx]   G_MTX -> 0x%08X outside RDRAM\n", w1);
                    break;
                }
                float v[16];
                for (int i = 0; i < 16; i++) {
                    const uint32_t whole = guest_hu(rdram, 0x80000000u | (m + i * 2));
                    const uint32_t frac = guest_hu(rdram, 0x80000000u | (m + 32 + i * 2));
                    v[i] = static_cast<float>(static_cast<int32_t>((whole << 16) | frac)) / 65536.0f;
                }
                const auto dot = [&](int a, int b) {
                    return v[a * 4] * v[b * 4] + v[a * 4 + 1] * v[b * 4 + 1] + v[a * 4 + 2] * v[b * 4 + 2];
                };
                std::fprintf(stderr,
                             "[mtx]   #%d @0x%06X %s %s %s  |r0|=%.3f |r1|=%.3f |r2|=%.3f  "
                             "r0.r1=%+.3f r0.r2=%+.3f r1.r2=%+.3f\n",
                             found, m, (params & 4) ? "PROJ" : "MODL", (params & 2) ? "LOAD" : "MUL ",
                             (params & 1) ? "PUSH" : "    ", std::sqrt(dot(0, 0)), std::sqrt(dot(1, 1)),
                             std::sqrt(dot(2, 2)), dot(0, 1), dot(0, 2), dot(1, 2));
                for (int r = 0; r < 4; r++) {
                    std::fprintf(stderr, "[mtx]      %+10.4f %+10.4f %+10.4f %+10.4f\n",
                                 v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
                }
                found++;
                break;
            }
            default:
                break;
        }
    }
    std::fflush(stderr);
}

}  // namespace

extern "C" {

// func_800D1BB4 has raised the pending count and posted the task to the
// scheduler queue at 0x801423EC.
void knife_edge_pipeline_submitted(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    static std::atomic<int> seen{0};
    report("task posted to the scheduler queue", seen);
}

// A dispatcher thread has returned from osSpTaskStartGo, so the task reached
// the runtime and ultramodern::submit_rsp_task has routed it. Which way it was
// routed is the whole question: submit_rsp_task sends type 1 (M_GFXTASK) to
// the graphics thread and everything else to the SP task thread, and those two
// fail in completely different ways. ctx->r4 still holds the task pointer --
// osSpTaskStartGo_recomp only reads it.
void knife_edge_pipeline_started(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    const uint32_t task = static_cast<uint32_t>(ctx->r4);
    const int32_t type = MEM_W(0x0, ctx->r4);
    const int32_t data_ptr = MEM_W(0x30, ctx->r4);
    if (type == 1 && !dump_indices().empty()) {
        const int index = gfx_tasks_started.fetch_add(1);
        if (dump_indices().count(index) != 0) {
            dump_matrices(rdram, static_cast<uint32_t>(data_ptr), index);
        }
    }
    if (!tracing()) {
        return;
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail),
                  "osSpTaskStartGo returned: task 0x%08X type %d data_ptr 0x%08X",
                  task, type, static_cast<uint32_t>(data_ptr));
    report(detail, seen);
}

// A task-done thread is about to drop the pending count, which is what
// releases the busy-wait in func_800D2B40.
void knife_edge_pipeline_completed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    static std::atomic<int> seen{0};
    report("task reported complete", seen);
}

// The retrace receiver thread has returned from osRecvMesg on the VI queue.
// Its message is still on the stack at 0x44(sp); 0x29A is the retrace the
// frame callback chain hangs off, so this says whether ultramodern's retrace
// is reaching the game at all.
void knife_edge_pipeline_retrace(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    const int32_t message = MEM_W(0x44, ctx->r29);
    char detail[96];
    std::snprintf(detail, sizeof(detail), "retrace receiver woke: message 0x%X",
                  static_cast<uint32_t>(message));
    report(detail, seen);

    // Walk the event-client list the dispatcher is about to walk, once. Each
    // node is {next, queue, mask:s16}; the dispatcher sends to every node whose
    // mask overlaps the event id. Printing it says which clients are actually
    // reachable, which is not something a queue's own counters can show.
    static std::atomic<bool> walked{false};
    bool expected = false;
    if (!walked.compare_exchange_strong(expected, true)) {
        return;
    }
    const gpr head_slot = static_cast<gpr>(static_cast<int32_t>(0x80142A18));
    uint32_t node = static_cast<uint32_t>(MEM_W(0, head_slot));
    for (int i = 0; i < 16 && node != 0; i++) {
        const gpr at = static_cast<gpr>(static_cast<int32_t>(node));
        const uint32_t next = static_cast<uint32_t>(MEM_W(0, at));
        const uint32_t queue = static_cast<uint32_t>(MEM_W(4, at));
        const int16_t mask = static_cast<int16_t>(MEM_H(8, at));
        std::fprintf(stderr,
                     "[pipeline]   client %d: node 0x%08X queue 0x%08X mask %d\n",
                     i, node, queue, static_cast<int>(mask));
        node = next;
    }
    std::fflush(stderr);
}

// The frame-callback thread, just after osRecvMesg on its own queue returns.
// The message is a pointer to the event id, and D_800EBC00 holds the callback
// the overlay registered -- the one that sets the flag the title loop spins on.
void knife_edge_pipeline_callback(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    const uint32_t message = static_cast<uint32_t>(MEM_W(0x40, ctx->r29));
    // This sits one instruction ahead of `lh $v0, 0x0($t6)`, which dereferences
    // the message. Every message on this queue is an event pointer -- the
    // dispatcher always sends the event struct -- so anything that is not a
    // KSEG0 address faults on the very next instruction. Report those every
    // time rather than on the sampling schedule, because the run ends there.
    if (message < 0x80000000u || (message & 1u) != 0u) {
        std::fprintf(stderr,
                     "[pipeline] BAD MESSAGE on the frame-callback queue: "
                     "0x%08X (sp 0x%08X) -- about to be dereferenced\n",
                     message, static_cast<uint32_t>(ctx->r29));
        std::fflush(stderr);
        return;
    }
    const gpr slot = static_cast<gpr>(static_cast<int32_t>(0x800EBC00));
    const gpr msg = static_cast<gpr>(static_cast<int32_t>(message));
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "frame-callback thread woke: message 0x%08X event_id %d "
                  "callback 0x%08X sp 0x%08X",
                  message, static_cast<int>(MEM_H(0, msg)),
                  static_cast<uint32_t>(MEM_W(0, slot)),
                  static_cast<uint32_t>(ctx->r29));
    report(detail, seen);
}

// The overlay's registered frame callback, at entry. Its only argument is the
// count of graphics tasks still outstanding, and it skips the entire frame --
// display list, flag and all -- when that count is above zero. If the count is
// never zero when the callback runs, nothing is ever drawn.
void knife_edge_pipeline_frame(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail), "frame callback: pending=%d -> %s",
                  static_cast<int>(static_cast<int32_t>(ctx->r4)),
                  static_cast<int32_t>(ctx->r4) > 0 ? "SKIPPED" : "drawing");
    report(detail, seen);
}

// func_800D23FC, the event dispatcher, at the point where it is about to
// osSendMesg to one client. a0 is the queue and a1 the event pointer it sends
// as the message, so this shows exactly what each client is handed.
void knife_edge_pipeline_dispatch(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    char detail[112];
    std::snprintf(detail, sizeof(detail),
                  "dispatch: queue 0x%08X <- event 0x%08X",
                  static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5));
    report(detail, seen);
}

// Immediately after the frame callback returns. Printing sp here and at the
// receive says whether the callback's call tree balanced the stack: $sp is an
// ordinary register in the generated C, so a function that allocates a frame
// and is cut off before restoring it moves sp permanently and every later
// off($sp) in the caller reads the wrong word.
void knife_edge_pipeline_returned(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail), "frame callback returned: sp 0x%08X",
                  static_cast<uint32_t>(ctx->r29));
    report(detail, seen);
}

// One sample of $sp, tagged with the call it follows. Walking these across a
// function's call sites is how an unbalanced callee gets located: the tag whose
// sp differs from the one before it names the call that leaked.
void knife_edge_sp_probe(uint8_t* rdram, recomp_context* ctx, const char* tag) {
    (void)rdram;
    static std::atomic<int> seen{0};
    if (!tracing() || seen.load() > 40) {
        return;
    }
    seen.fetch_add(1);
    std::fprintf(stderr, "[pipeline] sp after %-16s 0x%08X\n", tag,
                 static_cast<uint32_t>(ctx->r29));
}

// func_800D1FE0, which zeroes the four event-client list heads at
// D_801423B0+0x668. Called once at boot it is initialisation; called again
// after a client has registered it silently unregisters every one of them,
// and the per-frame callback chain simply stops.
void knife_edge_pipeline_event_init(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int> seen{0};
    if (!tracing()) {
        return;
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail),
                  "event system init: a0=%d a1=%d (clears the client lists)",
                  static_cast<int>(ctx->r4), static_cast<int>(ctx->r5));
    report(detail, seen);
}

// Called from the native busy-wait once it has been spinning long enough that
// something is clearly wrong, so a stalled run says what it is still holding
// out for instead of just hanging.
void knife_edge_pipeline_stalled(int32_t pending, int seconds) {
    if (!tracing()) {
        return;
    }
    std::fprintf(stderr, "[pipeline] still waiting after %ds: %d task(s) outstanding\n",
                 seconds, pending);
    std::fflush(stderr);
}

}  // extern "C"

// The VI-limited run leaves through _Exit, which skips atexit on purpose: the
// runtime never joins its guest OSThreads, so a normal teardown faults. Call
// this from there instead, or the totals never print.
extern "C" void knife_edge_pipeline_totals(void) {
    summarize();
}

// Entry of func_800C58E8, the per-object "draw with transform" the frame
// callbacks call once per object. Logs every argument, on dumped frames only,
// so the one that moves by the measured rotation step can be picked out by eye
// rather than guessed from the disassembly. Stack arguments are the caller's
// outgoing words at 0x10($sp) and up, since $sp is not yet adjusted here.
extern "C" void knife_edge_object_probe(uint8_t* rdram, recomp_context* ctx) {
    if (dump_indices().empty() || dump_indices().count(gfx_tasks_started.load()) == 0) {
        return;
    }
    const auto as_float = [](uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    std::fprintf(stderr, "[obj] frame %d ra 0x%08X a0 0x%08X a1 0x%08X a2 %g a3 %g |",
                 gfx_tasks_started.load(), static_cast<uint32_t>(ctx->r31),
                 static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5),
                 as_float(static_cast<uint32_t>(ctx->r6)), as_float(static_cast<uint32_t>(ctx->r7)));
    for (int offset = 0x10; offset <= 0x2C; offset += 4) {
        const uint32_t word = static_cast<uint32_t>(MEM_W(offset, ctx->r29));
        std::fprintf(stderr, " %02X:%g", offset, as_float(word));
    }
    std::fprintf(stderr, "\n");
}

// Entry of func_800D6C20, the game's guMtxF2L: a0 is the float matrix, a1 the
// fixed-point Mtx the RSP will load. Logs the pair on dumped frames, so each
// Mtx address in a display list can be traced back to the float matrix that
// produced it -- which says whether a bad rotation was already in the floats
// or appeared in the conversion.
extern "C" void knife_edge_mtxf2l_probe(uint8_t* rdram, recomp_context* ctx) {
    if (dump_indices().empty() || dump_indices().count(gfx_tasks_started.load()) == 0) {
        return;
    }
    float m[16];
    for (int i = 0; i < 16; i++) {
        const uint32_t bits = static_cast<uint32_t>(MEM_W(i * 4, ctx->r4));
        std::memcpy(&m[i], &bits, sizeof(float));
    }
    std::fprintf(stderr,
                 "[f2l] frame %d site 0x%08X out 0x%06X from 0x%08X  r0 %+.4f %+.4f %+.4f  r1 %+.4f %+.4f %+.4f  "
                 "r2 %+.4f %+.4f %+.4f  t %+.2f %+.2f %+.2f\n",
                 gfx_tasks_started.load(), f2l_site.load(), static_cast<uint32_t>(ctx->r5) & 0x00FFFFFFu,
                 static_cast<uint32_t>(ctx->r4), m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                 m[12], m[13], m[14]);
}

// Placed just before each of guMtxF2L's eleven call sites, so the conversion
// probe can say which caller built the matrix it is looking at.
extern "C" void knife_edge_f2l_site(uint32_t site) {
    f2l_site.store(site);
}

// Entry of func_800C6868, which builds scale x rotation x translation from a
// rotation matrix its caller hands over in a2. Logs the model pointer (a0) and
// position (a3, then 0x10/0x14($sp)) on dumped frames; the model address names
// the caller that decided the rotation.
extern "C" void knife_edge_srt_probe(uint8_t* rdram, recomp_context* ctx) {
    if (dump_indices().empty() || dump_indices().count(gfx_tasks_started.load()) == 0) {
        return;
    }
    const auto as_float = [](uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    std::fprintf(stderr, "[srt] frame %d model 0x%08X a1 0x%08X rot@0x%08X pos %.2f %.2f %.2f\n",
                 gfx_tasks_started.load(), static_cast<uint32_t>(ctx->r4),
                 static_cast<uint32_t>(ctx->r5), static_cast<uint32_t>(ctx->r6),
                 as_float(static_cast<uint32_t>(ctx->r7)),
                 as_float(static_cast<uint32_t>(MEM_W(0x10, ctx->r29))),
                 as_float(static_cast<uint32_t>(MEM_W(0x14, ctx->r29))));
    // The first object is the spinner under investigation: dump the words
    // around its rotation matrix, as float and as raw hex, to find the field
    // that steps by the spin each frame.
    static int last_frame = -1;
    if (gfx_tasks_started.load() != last_frame) {
        last_frame = gfx_tasks_started.load();
        const gpr base = static_cast<gpr>(static_cast<int32_t>(static_cast<uint32_t>(ctx->r6) - 0xC0));
        for (int offset = 0; offset < 0x100; offset += 4) {
            const uint32_t word = static_cast<uint32_t>(MEM_W(offset, base));
            std::fprintf(stderr, "[struct] frame %d rot%+04d 0x%08X %g\n", last_frame,
                         offset - 0xC0, word, as_float(word));
        }
    }
}

// Entry of a float-matrix builder (guRotateF and relatives). Logs its
// arguments when the destination is the spinner's embedded rotation matrix,
// on dumped frames, so the angle it was built from can be read directly.
extern "C" void knife_edge_rot_probe(uint8_t* rdram, recomp_context* ctx, uint32_t fn) {
    const uint32_t dest = static_cast<uint32_t>(ctx->r4);
    // The spinner's embedded rotation, or the per-frame delta it is multiplied
    // by, which is built in a stack buffer at 0x80144B60.
    const bool spinner = dest >= 0x8011D600u && dest < 0x8011D700u;
    const bool delta = dest >= 0x80144B00u && dest < 0x80144C00u;
    if (!spinner && !delta) {
        return;
    }
    if (dump_indices().empty() || dump_indices().count(gfx_tasks_started.load()) == 0) {
        return;
    }
    const auto as_float = [](uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    std::fprintf(stderr, "[rot] frame %d fn 0x%08X dest 0x%08X a1 %g a2 %g a3 %g sp10 %g sp14 %g (a1 raw 0x%08X)\n",
                 gfx_tasks_started.load(), fn, dest, as_float(static_cast<uint32_t>(ctx->r5)),
                 as_float(static_cast<uint32_t>(ctx->r6)), as_float(static_cast<uint32_t>(ctx->r7)),
                 as_float(static_cast<uint32_t>(MEM_W(0x10, ctx->r29))),
                 as_float(static_cast<uint32_t>(MEM_W(0x14, ctx->r29))),
                 static_cast<uint32_t>(ctx->r5));
}

// guMtxCatF (func_800D6F30) writes its product through a2, and guMtxIdentF
// (func_800D6D20) through a0. Log either when it lands in the spinner's
// embedded rotation, on dumped frames, with the inputs' first rows.
extern "C" void knife_edge_cat_probe(uint8_t* rdram, recomp_context* ctx, uint32_t fn) {
    const uint32_t dest = static_cast<uint32_t>(fn == 0x800D6F30u ? ctx->r6 : ctx->r4);
    const bool spinner = dest >= 0x8011D600u && dest < 0x8011D700u;
    const bool delta = dest >= 0x80144B00u && dest < 0x80144C00u;
    if (!spinner && !delta) {
        return;
    }
    if (dump_indices().empty() || dump_indices().count(gfx_tasks_started.load()) == 0) {
        return;
    }
    const auto row = [&](gpr base) {
        float v[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t bits = static_cast<uint32_t>(MEM_W(i * 4, base));
            std::memcpy(&v[i], &bits, sizeof(float));
        }
        char out[64];
        std::snprintf(out, sizeof(out), "(%+.3f %+.3f %+.3f)", v[0], v[1], v[2]);
        return std::string(out);
    };
    if (fn == 0x800D6F30u) {
        std::fprintf(stderr, "[cat] frame %d dest 0x%08X a0 0x%08X %s  a1 0x%08X %s\n",
                     gfx_tasks_started.load(), dest, static_cast<uint32_t>(ctx->r4),
                     row(ctx->r4).c_str(), static_cast<uint32_t>(ctx->r5), row(ctx->r5).c_str());
    } else {
        std::fprintf(stderr, "[cat] frame %d ident 0x%08X\n", gfx_tasks_started.load(), dest);
    }
}

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
#include <cstdio>
#include <cstdlib>

#include "recomp.h"

namespace {

constexpr int report_limit = 4;

bool tracing() {
    static const bool on = std::getenv("KNIFE_EDGE_TRACE_PIPELINE") != nullptr;
    return on;
}

void report(const char* step, std::atomic<int>& seen) {
    if (!tracing()) {
        return;
    }
    const int count = seen.fetch_add(1) + 1;
    // After the first few, report periodically instead of stopping: whether a
    // step happens at all is one question, and whether it keeps happening at
    // frame rate is a different one. 300 is about five seconds of NTSC.
    if (count <= report_limit || count % 60 == 0) {
        std::fprintf(stderr, "[pipeline] %s (%d)\n", step, count);
        std::fflush(stderr);
    }
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
    if (!tracing()) {
        return;
    }
    const uint32_t task = static_cast<uint32_t>(ctx->r4);
    const int32_t type = MEM_W(0x0, ctx->r4);
    const int32_t data_ptr = MEM_W(0x30, ctx->r4);
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
    const gpr slot = static_cast<gpr>(static_cast<int32_t>(0x800EBC00));
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "frame-callback thread woke: message 0x%08X event_id %d "
                  "callback 0x%08X sp 0x%08X",
                  message, static_cast<int>(static_cast<int32_t>(ctx->r2)),
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

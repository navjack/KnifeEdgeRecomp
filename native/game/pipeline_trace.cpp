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
    if (count <= report_limit || count % 300 == 0) {
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

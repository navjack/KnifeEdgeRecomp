// Guest busy-waits, replaced natively so they pump the runtime's event queue.
//
// ultramodern delivers SP-done, DP-done, VI retrace and the rest through an
// external message queue that is drained only at the *start* of an os* call
// (see dequeue_external_messages in ultramodern's mesgqueue.cpp). A thread
// already parked inside osRecvMesg is past that point and does not drain it,
// so the queue is only serviced by a thread that makes a fresh os* call.
//
// That is fine on hardware, where an interrupt preempts anything. It is a
// deadlock here. The game's boot thread posts its first display list and then
// sits in func_800D2B40, a three-instruction spin on a pending-task count,
// which calls nothing at all; the five threads that would decrement that count
// are all blocked in osRecvMesg. Nobody drains the queue, so the SP and DP
// completions the graphics thread posted sit in it forever and boot stops at
// the first frame.
//
// Recompiling the spin faithfully cannot work, because faithfulness is the
// problem: the loop's correctness on hardware depends on being interruptible.
// So it is replaced with a loop that waits on the runtime's queue instead of
// burning a core on RDRAM -- same termination condition, and each iteration
// now delivers whatever the graphics thread has finished.
//
// If another spin-wait turns up, it belongs here too. The general fix is a
// poll hook inside the recompiler's loop emission, which this project's pinned
// N64ModernRuntime does not have; see patches/README.md.
#include <cstdint>

#include <ultramodern/ultramodern.hpp>

#include "recomp.h"
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstdlib>
#include <cstdio>
#include "guest_waits.h"
#include "pipeline_trace.h"

namespace {

// Graphics tasks submitted and not yet reported complete. func_800D1BB4
// raises it, and the task-done threads func_800D1730 and func_800D2930 lower
// it once the SP and DP completions arrive.
constexpr uint32_t pending_gfx_tasks_vram = 0x8013C280;

// Long enough that a stalled wait costs nothing, short enough to stay well
// inside a 16.7 ms frame.
constexpr uint32_t poll_interval_ms = 1;

std::mutex sites_lock;
std::map<uint32_t, long long>& sites() {
    static std::map<uint32_t, long long> all;
    return all;
}

}  // namespace

extern "C" void func_800D2B40(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    const gpr counter = static_cast<gpr>(static_cast<int32_t>(pending_gfx_tasks_vram));
    int polls = 0;
    int reported = 0;
    while (MEM_W(0, counter) != 0) {
        ultramodern::wait_for_external_message_timed(rdram, poll_interval_ms);
        // Delivering the message only marks the waiting thread runnable.
        // ultramodern schedules guest threads the way the N64 did -- one runs
        // at a time, and it keeps running until it calls into the OS -- so
        // without this the thread that was just woken never actually gets the
        // CPU back from this loop, and the count it would decrement never
        // moves. Every os* entry point ends with the same call.
        ultramodern::check_running_queue(rdram);
        // A wait this long is not a slow frame, it is a stall. Say so once a
        // second rather than hanging silently.
        polls++;
        const int seconds = polls / 1000;
        if (seconds > reported) {
            reported = seconds;
            knife_edge_pipeline_stalled(MEM_W(0, counter), seconds);
        }
    }
}

// Hooked into the body of a loop that waits on a flag without calling
// anything. tools/spin-scan/find_spin_loops.py lists them: a short backward
// branch whose body has no call, no store, and a load through a fixed global.
//
// The game uses one idiom throughout -- register a per-frame callback, then
// spin until it sets D_8011D1B4 -- and repeats it verbatim in every overlay.
// Nothing in such a loop reaches the runtime, so on its own it waits forever
// for a callback whose retrace message is still sitting in the external queue.
//
// One iteration's worth of work: deliver whatever is pending, then let the
// thread that was woken actually run. Both are what an os* call would have
// done, which is the whole reason the loop works on hardware and not here.
extern "C" void knife_edge_guest_poll(uint8_t* rdram, recomp_context* ctx) {
    knife_edge_guest_poll_site(rdram, ctx, 0);
}

// Same, but recording which loop it was. A loop that turns once per frame is
// waiting the way the hardware made it wait; one that turns many times per
// frame is running the game faster than a 93.75 MHz R4300 ever could, which is
// the shape of this game's well-known "too fast everywhere" problem. Telling
// those apart needs a per-loop count, not a total.
extern "C" void knife_edge_guest_poll_site(uint8_t* rdram, recomp_context* ctx,
                                           uint32_t site) {
    (void)ctx;
    {
        std::lock_guard<std::mutex> held(sites_lock);
        sites()[site] += 1;
    }
    ultramodern::wait_for_external_message_timed(rdram, poll_interval_ms);
    ultramodern::check_running_queue(rdram);
}

// Printed alongside the pipeline totals, so the counts can be read against the
// retrace count from the same run.
extern "C" void knife_edge_guest_waits_totals(void) {
    if (std::getenv("KNIFE_EDGE_TRACE_PIPELINE") == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> held(sites_lock);
    std::vector<std::pair<uint32_t, long long>> all(sites().begin(), sites().end());
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "\n[waits] poll counts by loop, busiest first\n");
    for (const auto& [site, count] : all) {
        std::fprintf(stderr, "[waits]   0x%08X  %lld\n", site, count);
    }
    std::fflush(stderr);
}

// --- Simulated RDP drawing time ---------------------------------------------
//
// Knife Edge advances only on retraces when no graphics task is outstanding:
// every frame callback opens with `bgtz pending_gfx_tasks, skip`. On hardware
// the RSP and RDP take real time to draw a scene, so retraces go by while a
// frame is still being drawn and the game skips them. Here RT64 finishes at
// once, so nothing is ever pending and the game steps on every retrace.
//
// KNIFE_EDGE_RDP_VIS=N stands in for that drawing time: after the game draws
// on a retrace, the next N-1 retraces see the pending count as 1, so the game's
// own skip check rations its steps the way the hardware did. Only the value the
// callback is handed changes, and only for the duration of that call -- the
// real count is put back straight after -- so the game's accounting of
// submitted and completed tasks is untouched, and the retrace itself stays at
// 60 Hz, which leaves audio alone.
namespace {

int rdp_retraces() {
    static const int value = [] {
        const char* text = std::getenv("KNIFE_EDGE_RDP_VIS");
        const int parsed = text != nullptr ? std::atoi(text) : 1;
        return parsed >= 1 ? parsed : 1;
    }();
    return value;
}

uint64_t retraces_seen = 0;
uint64_t last_drawn = 0;
bool forced_busy = false;
uint64_t steps_allowed = 0;
uint64_t steps_held = 0;

}  // namespace

// Just before func_800D2930 hands the retrace callback the pending count. The
// callback runs on this thread without yielding when it skips, so nothing else
// observes the temporary value.
extern "C" void knife_edge_rdp_gate_before(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    retraces_seen++;
    forced_busy = false;
    if (rdp_retraces() <= 1) {
        return;
    }
    const gpr counter = static_cast<gpr>(static_cast<int32_t>(pending_gfx_tasks_vram));
    if (MEM_W(0, counter) != 0) {
        return;  // really still drawing: the game skips on its own
    }
    if (retraces_seen - last_drawn < static_cast<uint64_t>(rdp_retraces())) {
        MEM_W(0, counter) = 1;
        forced_busy = true;
        steps_held++;
    } else {
        last_drawn = retraces_seen;
        steps_allowed++;
    }
}

// Straight after the callback returns: put the real count back.
extern "C" void knife_edge_rdp_gate_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (forced_busy) {
        MEM_W(0, static_cast<gpr>(static_cast<int32_t>(pending_gfx_tasks_vram))) = 0;
        forced_busy = false;
    }
}

extern "C" void knife_edge_rdp_gate_totals(void) {
    if (rdp_retraces() <= 1) {
        return;
    }
    std::fprintf(stderr, "[rdp] %d retraces per drawn frame: %llu retraces, %llu steps allowed, %llu held\n",
                 rdp_retraces(), static_cast<unsigned long long>(retraces_seen),
                 static_cast<unsigned long long>(steps_allowed),
                 static_cast<unsigned long long>(steps_held));
    std::fflush(stderr);
}

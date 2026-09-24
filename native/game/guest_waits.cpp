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
    (void)ctx;
    ultramodern::wait_for_external_message_timed(rdram, poll_interval_ms);
    ultramodern::check_running_queue(rdram);
}

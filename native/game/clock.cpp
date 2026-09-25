// osSetTime(OSTime), as libultra actually behaves.
//
// libultra keeps time in two words: osGetTime() returns
// __osCurrentTime + (osGetCount() - __osBaseCounter), and the VI manager, on
// every retrace, adds the count since the last retrace to __osCurrentTime and
// moves __osBaseCounter up to now. osSetTime(t) writes only __osCurrentTime.
// So straight after osSetTime(t), osGetTime() is t plus the time since the
// last retrace -- not t. (Star Fox 64's libultra decomp: os/gettime.c,
// os/settime.c, io/vimgr.c.)
//
// ultramodern's osSetTime makes osGetTime() exactly t at the call. Knife Edge
// notices: its transmission box holds each step until osGetTime() reaches
// 2,800,000 ticks (59.7 ms) after osSetTime(0). Measured from the retrace
// before the reset, as on hardware, that lands every step 3 retraces after the
// last (50 ms); measured from the call it drifts to 3.58 (59.7 ms), 19% slow.
// A real-hardware longplay shows the 3-retrace rhythm: the stage 1 opening
// transmission takes 36.3 s there, 43.4 s with ultramodern's version.
//
// This replaces the game's copy (func_800D7A20) and passes ultramodern the
// value libultra's osGetTime would then report: t plus the time since the
// last VI.

#include "clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" void osSetTime(uint64_t time);  // ultramodern

namespace {

std::atomic<int64_t> last_vi_ns{0};

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

extern "C" void knife_edge_clock_vi(void) {
    last_vi_ns.store(now_ns(), std::memory_order_relaxed);
}

extern "C" void func_800D7A20(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    // OSTime arrives as a register pair: high word in a0, low in a1.
    const uint64_t time = (static_cast<uint64_t>(static_cast<uint32_t>(ctx->r4)) << 32) |
                          static_cast<uint32_t>(ctx->r5);
    constexpr int64_t counts_per_ms = 46'875;  // CPU count register: 93.75 MHz / 2
    const int64_t vi = last_vi_ns.load(std::memory_order_relaxed);
    const int64_t now = now_ns();
    const int64_t since_vi = (vi > 0 && now > vi) ? (now - vi) * counts_per_ms / 1'000'000 : 0;
    osSetTime(time + static_cast<uint64_t>(since_vi));
}

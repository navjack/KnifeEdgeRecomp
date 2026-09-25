#pragma once

// Guest instruction counting, for pacing the game the way the console did.
//
// N64Recomp (patches/N64Recomp-instruction-counter.patch) emits
// RECOMP_COUNT_INSTR() beside every recompiled instruction. Defining it here,
// ahead of recomp.h, turns each into one increment. Guest threads run one at a
// time under ultramodern, so a plain counter is race-free for guest code.
//
// Spinning is not work: knife_edge_guest_poll_site books the instructions a
// wait loop burns between its own polls as idle, so work = total - idle.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t knife_edge_guest_instructions;
extern uint64_t knife_edge_idle_instructions;
#ifdef __cplusplus
}
#endif

#define RECOMP_COUNT_INSTR() (++knife_edge_guest_instructions)

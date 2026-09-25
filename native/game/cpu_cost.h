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
//
// The thread-local pair does the same per guest thread (each runs on its own
// host thread). The pacing model needs a step's own cost: while the step waits,
// ultramodern runs other guest threads, and on the shared counter their
// instructions -- the gameplay loop spinning on its stage flag -- land in the
// step's total.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t knife_edge_guest_instructions;
extern uint64_t knife_edge_idle_instructions;
// __thread, not C++ thread_local: on Darwin a thread_local is reached through
// a C++ wrapper function the generated C cannot link to.
extern __thread uint64_t knife_edge_thread_instructions;
#ifdef __cplusplus
}
#endif

#define RECOMP_COUNT_INSTR() (++knife_edge_guest_instructions, ++knife_edge_thread_instructions)

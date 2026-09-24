// RSP task adapter.
//
// Graphics tasks never reach this function. ultramodern routes M_GFXTASK to a
// separate queue consumed by the renderer (ultramodern/src/events.cpp), so RT64
// handles the display lists and identifies the microcode itself by hashing it.
// Knife Edge uses stock F3DEX2 fifo 2.05 and F3DLX2 2.05.Rej, both already in
// RT64's table, so nothing is needed here for rendering.
//
// Everything else -- audio, and anything else the game submits -- arrives here.
// Returning nullptr is fatal: librecomp prints one line and ultramodern calls
// ULTRAMODERN_QUICK_EXIT. That is the honest behaviour while no microcode has
// been recovered, but it means the log below is the only evidence of what the
// game actually asked for, so it records enough to go and find it.

#include "rsp_contract.hpp"

#include "ultramodern/ultra64.h"

#include <cstdio>
#include <set>

namespace {

// One line per distinct task, not per submission: the audio task recurs every
// frame and would otherwise bury everything else.
bool first_sighting(uint32_t type, uint32_t ucode) {
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    return seen.emplace(type, ucode).second;
}

}

extern "C" RspUcodeFunc* knife_edge_get_rsp_microcode(const OSTask* task) {
    const uint32_t type = task->t.type;

    if (first_sighting(type, task->t.ucode)) {
        const char* name = type == M_GFXTASK ? "M_GFXTASK (unexpected here)"
                         : type == M_AUDTASK ? "M_AUDTASK"
                         : "unknown";
        std::fprintf(stderr,
                     "[rsp] task type %u (%s): ucode 0x%08X data 0x%08X size 0x%X\n",
                     type, name, task->t.ucode, task->t.ucode_data,
                     task->t.ucode_size);
    }

    switch (type) {
        case M_AUDTASK:
            // TODO: the audio microcode lives in the block at ROM
            // 0x23DF0-0x27200 (895 COP2 words, no MIPS prologues). Find the
            // stock aspMain prologue in it, generate with RSPRecomp, and return
            // it here. The toml needs extra_indirect_branch_targets for the
            // 16-entry DMEM command table or half the commands will exit with
            // UnhandledJumpTarget.
            std::fprintf(stderr,
                         "[rsp] no audio microcode recovered yet; the process "
                         "will exit. See rsp/README.md.\n");
            return nullptr;

        default:
            return nullptr;
    }
}

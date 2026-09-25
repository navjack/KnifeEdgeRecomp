#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What one display list asks of the RCP, counted as F3DEX2 would process it.
typedef struct FrameCounts {
    double commands, rdp_commands, matrices, syncs;
    double vertices, vertices_lit, light_xfrms;
    double tris_drawn, tris_backface, tris_offscreen, tris_clipped, tris_degenerate;
    double px_1c, px_1c_z, px_2c, px_2c_z, px_fill, px_copy;  // by cycle type, with/without Z
    double spans, texture_bytes;
    double pipe_cycles, pixel_bytes, sync_cycles;  // for fitting the RDP's bandwidth offline
    double rsp_ms, rdp_ms;
} FrameCounts;

// Estimated milliseconds a real N64 would take to draw this display list: RSP
// and RDP overlap through the FIFO, so the slower of the two. See rdp_model.cpp.
double knife_edge_estimate_frame_ms(uint8_t* rdram, uint32_t display_list);

// The counts behind the last estimate.
const FrameCounts* knife_edge_last_frame_counts(void);

// Set by the pipeline probe each graphics task: how many retraces the frame
// just submitted would have kept the console busy. 0 = no model, fixed pacing.
void knife_edge_rdp_set_frame_retraces(int retraces);

// Host ms since the retrace callback last let the game step (guest_waits.cpp).
double knife_edge_ms_since_step_start(void);

// Guest instructions of real work (spinning excluded) since then: the step's
// own CPU cost when read as it submits its graphics task.
uint64_t knife_edge_step_work_so_far(void);

// Whether a step is running: from the retrace that let it run until it submits
// its graphics task (knife_edge_step_submitted).
int knife_edge_in_step(void);
void knife_edge_step_submitted(void);

#ifdef __cplusplus
}
#endif

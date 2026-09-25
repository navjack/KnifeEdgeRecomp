#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Estimated milliseconds a real N64 would take to draw this display list:
// RSP vertex/triangle work and RDP fill, the slower of the two. See rdp_model.cpp.
double knife_edge_estimate_frame_ms(uint8_t* rdram, uint32_t display_list);

// Set by the pipeline probe each graphics task: how many retraces the frame
// just submitted would have kept the console busy. 0 = no model, fixed pacing.
void knife_edge_rdp_set_frame_retraces(int retraces);

// Fraction of the last frame's on-screen triangles the RSP would back/front-cull.
double knife_edge_last_culled_fraction(void);

#ifdef __cplusplus
}
#endif

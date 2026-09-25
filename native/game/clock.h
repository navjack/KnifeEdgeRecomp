#pragma once

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// osSetTime, replaced natively with libultra's retrace-based semantics: see clock.cpp.
void func_800D7A20(uint8_t* rdram, recomp_context* ctx);

// Called on every VI from the frontend's vi_callback.
void knife_edge_clock_vi(void);

#ifdef __cplusplus
}
#endif

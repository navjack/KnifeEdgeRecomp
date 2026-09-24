#ifndef KNIFE_EDGE_PIPELINE_TRACE_H
#define KNIFE_EDGE_PIPELINE_TRACE_H

/* Included by the generated C, so this header stays plain C. */

#include <stdint.h>
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [[patches.hook]] targets that say how far a submitted graphics task gets.
 * See native/game/pipeline_trace.cpp; all three are silent unless
 * KNIFE_EDGE_TRACE_PIPELINE is set. */
void knife_edge_pipeline_submitted(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_started(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_completed(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_retrace(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_callback(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_event_init(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_dispatch(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_frame(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_returned(uint8_t* rdram, recomp_context* ctx);
void knife_edge_pipeline_totals(void);

/* Reported by the native busy-wait in guest_waits.cpp, not by a hook. */
void knife_edge_pipeline_stalled(int32_t pending, int seconds);

#ifdef __cplusplus
}
#endif

#endif /* KNIFE_EDGE_PIPELINE_TRACE_H */

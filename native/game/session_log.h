#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// For Finder launches (or KNIFE_EDGE_SESSION_LOG=1): send stderr to
// ~/Library/Logs/KnifeEdge/session-<time>.log with overlay and pipeline traces
// on, and sample every thread into hang-<time>.txt after 8 s without a new
// graphics task. Call first thing in main, before any guest code runs.
void knife_edge_session_log_start(void);

#ifdef __cplusplus
}
#endif

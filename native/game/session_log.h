#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// For Finder launches (or KNIFE_EDGE_SESSION_LOG=1 on any platform): send
// stderr to session-<time>.log in the platform's log directory with overlay and
// pipeline traces on, and flag 8 s without a new graphics task (sampling every
// thread into hang-<time>.txt on macOS). Call first thing in main, before any
// guest code runs.
void knife_edge_session_log_start(void);

#ifdef __cplusplus
}
#endif

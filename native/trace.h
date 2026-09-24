#pragma once

// Hooks callable from recompiled code. Bind one to a guest address with a
// [[patches.hook]] entry in config/recomp-symbols.toml, for example:
//
//   [[patches.hook]]
//   func = "func_800CD6B0"
//   before_vram = 0x800CD6B0
//   text = "knife_edge_trace_entry(\"boot\");"
//
// Keep this list short. The Cruis'n workspace accumulated two dozen bespoke
// trace hooks during debugging; add one only while you are chasing something,
// and delete it when the question is answered.

#ifdef __cplusplus
extern "C" {
#endif

void knife_edge_trace_entry(const char* function_name);
void knife_edge_trace_return(const char* function_name);
uint64_t knife_edge_vi_count(void);

#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() knife_edge_trace_entry(__func__);
#define TRACE_RETURN() knife_edge_trace_return(__func__);

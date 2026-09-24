#pragma once

#include "librecomp/rsp.hpp"

// The checked-in/generated RSP implementation in rsp/ must provide this
// adapter. Keeping the boundary explicit stops the native app from silently
// running with no RSP implementation at all.
extern "C" RspUcodeFunc* knife_edge_get_rsp_microcode(const OSTask* task);

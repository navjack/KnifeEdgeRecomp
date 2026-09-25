#pragma once

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// __osMotorAccess (osMotorStart/osMotorStop), replaced natively: see rumble.cpp.
void func_800DB190(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

// __osMotorAccess(OSPfs* pfs, s32 flag), the body of libultra's
// osMotorStart(pfs) (flag 1) and osMotorStop(pfs) (flag 0).
//
// The game's copy, func_800DB190, talks to the Rumble Pak over the serial
// interface and faults on the SI registers when recompiled. N64Recomp knows the
// name as reimplemented, but the runtime provides only osMotorStart_recomp and
// osMotorStop_recomp, so this stands in for it and forwards: the arguments are
// already where those expect them (pfs in a0), and they write the return value.

#include "rumble.h"

extern "C" void osMotorStart_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osMotorStop_recomp(uint8_t* rdram, recomp_context* ctx);

extern "C" void func_800DB190(uint8_t* rdram, recomp_context* ctx) {
    constexpr int32_t motor_start = 1;
    if (static_cast<int32_t>(ctx->r5) == motor_start) {
        osMotorStart_recomp(rdram, ctx);
    } else {
        osMotorStop_recomp(rdram, ctx);
    }
}

#ifndef KNIFE_EDGE_GUEST_WAITS_H
#define KNIFE_EDGE_GUEST_WAITS_H

/* Included by the generated C, so this header stays plain C. */

#include <stdint.h>
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native replacement for the game's graphics-task busy-wait. Listed under
 * [patches] ignored so the recompiler drops the guest body and every call site
 * lands here instead. See native/game/guest_waits.cpp. */
void func_800D2B40(uint8_t* rdram, recomp_context* ctx);

/* [[patches.hook]] target for the game's other wait loops, which spin on a
 * flag without calling anything. Listed by tools/spin-scan. */
void knife_edge_guest_poll(uint8_t* rdram, recomp_context* ctx);
void knife_edge_guest_poll_site(uint8_t* rdram, recomp_context* ctx,
                                uint32_t site);
void knife_edge_guest_waits_totals(void);
void knife_edge_rdp_gate_before(uint8_t* rdram, recomp_context* ctx);
void knife_edge_rdp_gate_after(uint8_t* rdram, recomp_context* ctx);
void knife_edge_rdp_gate_totals(void);

#ifdef __cplusplus
}
#endif

#endif /* KNIFE_EDGE_GUEST_WAITS_H */

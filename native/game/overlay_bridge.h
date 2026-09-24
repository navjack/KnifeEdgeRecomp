#ifndef KNIFE_EDGE_OVERLAY_BRIDGE_H
#define KNIFE_EDGE_OVERLAY_BRIDGE_H

/* Included by the generated C, so this header stays plain C. */

#include <stdint.h>

/* recomp.h typedefs recomp_context from an anonymous struct, so it cannot be
 * forward-declared; include it. It carries its own guard, and the generated C
 * includes it first in any case. */
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [[patches.hook]] target, injected into func_800D1D10 after its DMA completes.
 * librecomp never watches DMA, so nothing registers a runtime-loaded overlay
 * unless the port does it here. */
void knife_edge_overlay_dma(uint8_t* rdram, recomp_context* ctx);

/* GameEntry::on_init_callback. Undoes the overlays that librecomp's boot-time
 * 1 MB load registers at addresses the game never uses. */
void knife_edge_neutralize_boot_overlays(uint8_t* rdram, recomp_context* ctx);

/* Implemented in native/register_generated.cpp, which has recomp_overlays.inl
 * in scope. Size of the code section starting exactly at `rom`, else 0. */
uint32_t knife_edge_code_section_size(uint32_t rom);

#ifdef __cplusplus
}
#endif

#endif

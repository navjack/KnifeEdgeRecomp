// Registers Knife Edge's runtime-loaded code overlays with librecomp.
//
// librecomp does not watch DMA. Its only built-in registration is the boot-time
// `load_overlays(0x1000, entrypoint, 1MB)` in librecomp/src/recomp.cpp, so every
// overlay this game brings in at runtime would otherwise be unknown to
// `get_function`, which aborts the process when it cannot resolve an address.
//
// All four of the game's overlay loaders funnel through func_800D1D10, a
// blocking DMA helper (osCreateMesgQueue -> osInvalDCache -> osEPiStartDma ->
// osRecvMesg). One hook there therefore covers every load, code or data, with
// the source, destination and size already in hand.

#include "overlay_bridge.h"

#include "librecomp/overlays.hpp"
#include "recomp.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace {

std::mutex overlay_mutex;

// Everything this bridge has registered: loaded address -> section size.
// librecomp keeps its own `loaded_sections` but does not expose it, and the
// exact extents are needed to compute a safe unload range.
std::map<uint32_t, uint32_t> loaded_overlays;

bool tracing() {
    static const bool on = std::getenv("KNIFE_EDGE_TRACE_OVERLAYS") != nullptr;
    return on;
}

}

extern "C" void knife_edge_overlay_dma(uint8_t* rdram, recomp_context* ctx) {
    // func_800D1D10(rom, dest, size) spills its arguments to 0x50/0x54/0x58($sp)
    // before doing anything else. The hook sits after osRecvMesg has returned,
    // so the bytes are in RDRAM, and before the epilogue, so sp still addresses
    // the frame.
    const uint32_t rom = static_cast<uint32_t>(MEM_W(0x50, ctx->r29));
    const uint32_t dest = static_cast<uint32_t>(MEM_W(0x54, ctx->r29));
    const uint32_t size = static_cast<uint32_t>(MEM_W(0x58, ctx->r29));
    if (size == 0) {
        return;
    }

    std::lock_guard lock{overlay_mutex};

    // Expand the affected region until it fully contains every overlay already
    // registered that this transfer touches. unload_overlays() refuses to
    // partially unload a section and calls exit(), so the range handed to it has
    // to cover each overlapping section whole.
    //
    // Widening past the bytes actually written is correct here rather than
    // merely safe: the game's modules are mutually exclusive by construction.
    // ovl_02FC30 fills 0x8016D6F0..0x8017B0C0 and swaps at 0x8017B0C0, while
    // ovl_1501A0 fills 0x8016D6F0..0x801D21F0 and swaps at 0x801D21F0, so an
    // overlay overlapped by an incoming load cannot still be coherent.
    uint32_t lo = dest;
    uint32_t hi = dest + size;
    bool overlaps = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [addr, extent] : loaded_overlays) {
            if (addr >= hi || addr + extent <= lo) {
                continue;
            }
            overlaps = true;
            if (addr < lo) {
                lo = addr;
                changed = true;
            }
            if (addr + extent > hi) {
                hi = addr + extent;
                changed = true;
            }
        }
    }

    if (overlaps) {
        if (tracing()) {
            std::fprintf(stderr, "[ovl] unload 0x%08X..0x%08X\n", lo, hi);
        }
        unload_overlays(static_cast<int32_t>(lo), hi - lo);
        for (auto it = loaded_overlays.begin(); it != loaded_overlays.end();) {
            const bool inside = it->first >= lo && it->first + it->second <= hi;
            it = inside ? loaded_overlays.erase(it) : std::next(it);
        }
    }

    // Only a transfer that starts exactly at a code section registers one.
    // Data loads and the LZSS-compressed blobs fall through, having already had
    // their unload effect above -- a data descriptor can and does overwrite a
    // window that code was living in.
    const uint32_t section_size = knife_edge_code_section_size(rom);
    if (section_size == 0) {
        return;
    }
    if (section_size > size) {
        // load_overlays only registers sections fully contained in the transfer;
        // a short read would be silently skipped and surface much later as a
        // fatal get_function, so say so now.
        std::fprintf(stderr,
                     "[ovl] refusing partial load of section at rom 0x%08X: "
                     "section 0x%X bytes, transfer 0x%X\n",
                     rom, section_size, size);
        return;
    }

    load_overlays(rom, static_cast<int32_t>(dest), size);
    loaded_overlays[dest] = section_size;
    if (tracing()) {
        std::fprintf(stderr, "[ovl] load   rom=0x%08X dest=0x%08X size=0x%X\n",
                     rom, dest, section_size);
    }
}

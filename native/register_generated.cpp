#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"

#include "game/overlay_bridge.h"

#include <cstdio>

#include "recomp_overlays.inl"

// Provided by the generated code.
extern "C" gpr get_entrypoint_address();

namespace {

// librecomp's init() performs `load_overlays(0x1000, entrypoint, 1024 * 1024)`
// before the game runs a single instruction.
constexpr uint32_t kBootRomStart = 0x1000;
constexpr uint32_t kBootSize = 1024 * 1024;

}

extern "C" void knife_edge_register_generated_code() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}

extern "C" uint32_t knife_edge_code_section_size(uint32_t rom) {
    for (size_t i = 0; i < ARRLEN(section_table); i++) {
        if (section_table[i].rom_addr == rom) {
            return section_table[i].size;
        }
    }
    return 0;
}

extern "C" void knife_edge_neutralize_boot_overlays(uint8_t*, recomp_context*) {
    // The boot load registers every section fully contained in the first 1 MB of
    // ROM, placing each at `rom_addr - 0x1000 + entrypoint`. That is right for
    // the resident segment and meaningless for an overlay: six of Knife Edge's
    // overlays start inside that window and get registered at addresses the game
    // never loads them to. ovl_0BA450 lands at 0x8017B850, straddling the live
    // 0x8017B0C0 swap window, so the first real swap there would hit
    // unload_overlays' partial-unload check and exit the process.
    //
    // Undo exactly those. Each unload uses the section's own extent, so it is
    // always a whole-section unload and can never trip that check. Driving it
    // off section_table rather than a hardcoded list keeps it correct if the
    // overlay boundaries change.
    const uint32_t entrypoint = static_cast<uint32_t>(get_entrypoint_address());
    for (size_t i = 0; i < ARRLEN(section_table); i++) {
        const SectionTableEntry& section = section_table[i];
        if (section.rom_addr < kBootRomStart ||
            section.rom_addr + section.size > kBootRomStart + kBootSize) {
            continue;
        }
        const uint32_t registered_at = section.rom_addr - kBootRomStart + entrypoint;
        if (registered_at == section.ram_addr) {
            continue;  // the resident segment, which really does live there
        }
        std::fprintf(stderr,
                     "[ovl] boot-load placed rom 0x%08X at 0x%08X (links at 0x%08X); "
                     "deregistering\n",
                     section.rom_addr, registered_at, section.ram_addr);
        unload_overlays(static_cast<int32_t>(registered_at), section.size);
    }
}

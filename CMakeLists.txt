cmake_minimum_required(VERSION 3.20)
project(knife_edge LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(KNIFE_EDGE_ROOT "" CACHE PATH "Knife Edge workspace root")
if (NOT KNIFE_EDGE_ROOT)
    get_filename_component(KNIFE_EDGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
endif()
include("${KNIFE_EDGE_ROOT}/native/KnifeEdgeNative.cmake")

# SAVE_TYPE is None until the game's real save device is established from a
# code path rather than guessed; see docs/ROM_ANALYSIS.md.
knife_edge_add_native_app(
    GAME_ID knife_edge
    ROM_HASH 56605501a85c79af
    INTERNAL_NAME "KNIFE EDGE"
    DISPLAY_NAME "Knife Edge: Nose Gunner"
    WINDOW_TITLE "Knife Edge: Nose Gunner — Recompiled"
    PROGRAM_ID KnifeEdgeRecompiled
    PROGRAM_NAME "Knife Edge: Nose Gunner: Recompiled"
    SAVE_TYPE None)

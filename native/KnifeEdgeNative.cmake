# Builds the native app from the recompiler output in generated/, the title's
# RSP sources in rsp/, and the shared SDL/RT64 frontend in native/:
#
#   macOS    arm64 .app, RT64 on Metal
#   Linux    executable with its assets beside it, RT64 on Vulkan
#   Windows  executable with its assets and DLLs beside it, RT64 on D3D12/Vulkan
#
# Every required input is checked here so a configure failure names the missing
# artifact instead of producing a placeholder binary.
#
# KNIFE_EDGE_CHECK_ONLY builds without anything derived from the ROM: every
# library the app links is built and the port's own sources are compiled
# against them, but no executable is produced. It is as far as a machine
# without the ROM can take the build, and it is what CI runs.

function(knife_edge_add_native_app)
    set(options)
    set(one_value_args
        GAME_ID ROM_HASH INTERNAL_NAME DISPLAY_NAME WINDOW_TITLE
        PROGRAM_ID PROGRAM_NAME SAVE_TYPE)
    cmake_parse_arguments(KNIFE_EDGE "${options}" "${one_value_args}" "" ${ARGN})

    if (NOT APPLE AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
        message(FATAL_ERROR "The Knife Edge native app builds on macOS, Linux and Windows only")
    endif()
    # The compile options below are clang's. On Windows that means clang-cl;
    # cl.exe cannot take them, and has never been tried on the generated code.
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "The Knife Edge native app builds with clang (clang-cl on Windows), "
            "not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    foreach(required_argument
        GAME_ID ROM_HASH INTERNAL_NAME DISPLAY_NAME WINDOW_TITLE
        PROGRAM_ID PROGRAM_NAME SAVE_TYPE)
        if (NOT KNIFE_EDGE_${required_argument})
            message(FATAL_ERROR "knife_edge_add_native_app: ${required_argument} is required")
        endif()
    endforeach()

    if (NOT KNIFE_EDGE_ROOT)
        get_filename_component(KNIFE_EDGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
    endif()
    if (NOT EXISTS "${KNIFE_EDGE_ROOT}/native/KnifeEdgeNative.cmake")
        message(FATAL_ERROR "Set KNIFE_EDGE_ROOT to the Knife Edge workspace root")
    endif()

    set(GENERATED_DIR "${KNIFE_EDGE_ROOT}/generated")
    set(RSP_DIR "${KNIFE_EDGE_ROOT}/rsp")
    set(NATIVE_ROOT "${KNIFE_EDGE_ROOT}/native")
    set(FRONTEND_ROOT "${NATIVE_ROOT}/frontend")
    set(STACK_ROOT "${KNIFE_EDGE_ROOT}/upstream")

    if (NOT KNIFE_EDGE_CHECK_ONLY)
        foreach(required_file
            "${GENERATED_DIR}/funcs.h"
            "${GENERATED_DIR}/lookup.cpp"
            "${GENERATED_DIR}/recomp_overlays.inl")
            if (NOT EXISTS "${required_file}")
                message(FATAL_ERROR
                    "Missing ${required_file}. Run scripts/recompile.sh after supplying "
                    "complete game metadata in input/.")
            endif()
        endforeach()

        file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
            "${GENERATED_DIR}/*.c"
            "${GENERATED_DIR}/*.cpp")
        if (NOT GENERATED_SOURCES)
            message(FATAL_ERROR "Generated game sources are missing from ${GENERATED_DIR}")
        endif()

        # aspMain.cpp is RSPRecomp's translation of the ROM's audio microcode.
        if (NOT EXISTS "${RSP_DIR}/aspMain.cpp")
            message(FATAL_ERROR
                "Recompiled RSP microcode is missing: ${RSP_DIR}/aspMain.cpp. "
                "Run scripts/generate_rsp.sh. A native app must not run with an "
                "empty RSP implementation.")
        endif()
    endif()

    file(GLOB RSP_SOURCES CONFIGURE_DEPENDS
        "${RSP_DIR}/*.c"
        "${RSP_DIR}/*.cpp")
    list(FILTER RSP_SOURCES EXCLUDE REGEX "/aspMain\\.cpp$")
    if (NOT RSP_SOURCES)
        message(FATAL_ERROR "The RSP task adapter is missing from ${RSP_DIR}")
    endif()

    # Hand-written native code that replaces or hooks recompiled functions.
    file(GLOB TITLE_NATIVE_SOURCES CONFIGURE_DEPENDS "${NATIVE_ROOT}/game/*.cpp")

    configure_file(
        "${NATIVE_ROOT}/game_config.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/game_config.hpp"
        @ONLY)

    # clang-cl takes GCC-style options only behind /clang:, and spells a forced
    # include /FI.
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(NO_STRICT_ALIASING "/clang:-fno-strict-aliasing")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FIstdlib.h")
    else()
        set(NO_STRICT_ALIASING "-fno-strict-aliasing")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include stdlib.h")
    endif()

    set(RT64_STATIC TRUE CACHE BOOL "" FORCE)
    # ultramodern's Linux window handle is the SDL_Window itself, and RT64 makes
    # its Vulkan surface from it. macOS hands RT64 the CAMetalLayer and Windows
    # the HWND instead.
    if (CMAKE_SYSTEM_NAME MATCHES "Linux")
        set(RT64_SDL_WINDOW_VULKAN TRUE CACHE BOOL "" FORCE)
        # RT64 defines these for itself only, but they change plume's
        # RenderWindow, which recompui and the app assign to as well.
        add_compile_definitions(RT64_SDL_WINDOW_VULKAN PLUME_SDL_VULKAN_ENABLED)
    else()
        set(RT64_SDL_WINDOW_VULKAN FALSE CACHE BOOL "" FORCE)
    endif()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
    add_compile_definitions(HLSL_CPU)
    if (WIN32)
        add_compile_definitions(NOMINMAX)
    endif()

    set(RT64_ROOT "${STACK_ROOT}/rt64")
    set(RUNTIME_ROOT "${STACK_ROOT}/N64ModernRuntime")
    set(FRONTEND_LIBRARY_ROOT "${STACK_ROOT}/RecompFrontend")
    set(N64RECOMP_ROOT "${STACK_ROOT}/N64Recomp" CACHE PATH
        "N64Recomp checkout shared by generated code and the runtime")
    if (NOT EXISTS "${N64RECOMP_ROOT}/include/recomp.h")
        message(FATAL_ERROR "N64RECOMP_ROOT must point to this project's N64Recomp checkout")
    endif()

    # SDL2 is found before RT64 is added: on Linux, plume creates its Vulkan
    # surface through SDL and reuses this target rather than finding its own.
    if (WIN32)
        # The official VC development package. recompui and recompinput read its
        # headers from ${sdl2_SOURCE_DIR}, so the download has to be named sdl2.
        include(FetchContent)
        FetchContent_Declare(sdl2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip
            URL_HASH SHA256=af347939395a58b365846aaea27391e69f9ec9d4dd650d6ac40802159b418a6e)
        FetchContent_MakeAvailable(sdl2)
        find_package(SDL2 REQUIRED CONFIG PATHS "${sdl2_SOURCE_DIR}/cmake" NO_DEFAULT_PATH)
        # SDL2_LIBRARIES also names SDL2main; main.cpp defines SDL_MAIN_HANDLED.
        set(SDL2_LIBRARIES SDL2::SDL2)
    else()
        find_package(SDL2 REQUIRED)
    endif()
    if (NOT SDL2_INCLUDE_DIRS AND TARGET SDL2::SDL2)
        get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    endif()

    add_subdirectory("${RT64_ROOT}" "${CMAKE_BINARY_DIR}/rt64")
    add_subdirectory("${RUNTIME_ROOT}" "${CMAKE_BINARY_DIR}/N64ModernRuntime")

    # RT64's shader helpers read the DXC settings from the calling scope, and
    # RT64 only sets them inside its own directory; without these recompui emits
    # a malformed shader command. The DXC selection mirrors RT64's own.
    set(DXC_COMMON_OPTS "-I${RT64_ROOT}/src")
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
    set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
    set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3"
        "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing"
        "-fspv-extension=SPV_EXT_descriptor_indexing")
    set(DXC_ROOT "${RT64_ROOT}/src/contrib/dxc")
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
        set(DXC_ARCH arm64)
    else()
        set(DXC_ARCH x64)
    endif()
    if (APPLE)
        find_library(ZLIB_LIBRARY NAMES z)
        get_filename_component(ZLIB_PATH "${ZLIB_LIBRARY}" DIRECTORY)
        set(DXC "DYLD_LIBRARY_PATH=${DXC_ROOT}/lib/${DXC_ARCH}:${ZLIB_PATH}"
                "${DXC_ROOT}/bin/${DXC_ARCH}/dxc-macos")
    elseif (WIN32)
        set(DXC "${DXC_ROOT}/bin/x64/dxc.exe")
    else()
        set(DXC "LD_LIBRARY_PATH=${DXC_ROOT}/lib/${DXC_ARCH}"
                "${DXC_ROOT}/bin/${DXC_ARCH}/dxc-linux")
    endif()

    # recompui includes the host project's patches/ui_funcs.h beside the frontend checkout.
    foreach(frontend_header ui_funcs.h patch_helpers.h recompui_event_structs.h)
        configure_file("${FRONTEND_ROOT}/${frontend_header}"
                       "${STACK_ROOT}/patches/${frontend_header}" COPYONLY)
    endforeach()
    set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${RUNTIME_ROOT}")
    set(RECOMP_FRONTEND_RT64_PATH "${RT64_ROOT}")
    add_subdirectory("${FRONTEND_LIBRARY_ROOT}" "${CMAKE_BINARY_DIR}/RecompFrontend")

    set(RUNTIME_INCLUDES
        "${N64RECOMP_ROOT}/include"
        "${RUNTIME_ROOT}/librecomp/include"
        "${RUNTIME_ROOT}/ultramodern/include"
        "${RUNTIME_ROOT}/thirdparty"
        "${RUNTIME_ROOT}/thirdparty/sse2neon"
        "${RUNTIME_ROOT}/thirdparty/concurrentqueue")

    # The port's own code: everything that is compiled from this repository
    # rather than from the ROM.
    set(PORT_SOURCES
        "${NATIVE_ROOT}/main.cpp"
        ${RSP_SOURCES}
        ${TITLE_NATIVE_SOURCES})
    if (APPLE)
        list(APPEND PORT_SOURCES "${NATIVE_ROOT}/macos_menu.mm")
    endif()
    set(PORT_INCLUDES
        "${NATIVE_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        ${RUNTIME_INCLUDES}
        "${RT64_ROOT}/src"
        "${RT64_ROOT}/src/contrib"
        "${RT64_ROOT}/src/contrib/hlslpp/include"
        "${RT64_ROOT}/src/contrib/plume"
        "${RT64_ROOT}/src/contrib/nativefiledialog-extended/src/include"
        "${FRONTEND_LIBRARY_ROOT}/recompinput/include"
        "${FRONTEND_LIBRARY_ROOT}/recompui/include"
        ${SDL2_INCLUDE_DIRS})
    set(PORT_LIBRARIES recompui recompinput librecomp ultramodern rt64 nfd ${SDL2_LIBRARIES})

    if (KNIFE_EDGE_CHECK_ONLY)
        add_library(${KNIFE_EDGE_GAME_ID}_port OBJECT ${PORT_SOURCES})
        target_include_directories(${KNIFE_EDGE_GAME_ID}_port PRIVATE ${PORT_INCLUDES})
        target_compile_definitions(${KNIFE_EDGE_GAME_ID}_port PRIVATE HLSL_CPU)
        target_compile_options(${KNIFE_EDGE_GAME_ID}_port PRIVATE ${NO_STRICT_ALIASING})
        # Nothing is linked; this carries the libraries' usage requirements
        # (include directories, definitions) exactly as the app target gets them.
        target_link_libraries(${KNIFE_EDGE_GAME_ID}_port PRIVATE ${PORT_LIBRARIES})
        add_custom_target(${KNIFE_EDGE_GAME_ID}_check ALL)
        add_dependencies(${KNIFE_EDGE_GAME_ID}_check ${KNIFE_EDGE_GAME_ID}_port
            recompui recompinput librecomp ultramodern rt64 nfd)
        message(STATUS
            "KNIFE_EDGE_CHECK_ONLY: building the libraries and the port's own "
            "sources; no executable, nothing from the ROM")
        return()
    endif()

    add_library(${KNIFE_EDGE_GAME_ID}_code STATIC ${GENERATED_SOURCES})
    target_include_directories(${KNIFE_EDGE_GAME_ID}_code PRIVATE
        "${NATIVE_ROOT}" "${GENERATED_DIR}" ${RUNTIME_INCLUDES})
    target_compile_options(${KNIFE_EDGE_GAME_ID}_code PRIVATE
        ${NO_STRICT_ALIASING} -Wno-unused-variable -Wno-unused-function)
    # scripts/recompile.sh declares every runtime replacement the generated code
    # calls; funcs.h only declares some of them.
    if (EXISTS "${GENERATED_DIR}/recomp_decls.h")
        if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(${KNIFE_EDGE_GAME_ID}_code PRIVATE
                "/FI${GENERATED_DIR}/recomp_decls.h")
        else()
            target_compile_options(${KNIFE_EDGE_GAME_ID}_code PRIVATE
                -include "${GENERATED_DIR}/recomp_decls.h")
        endif()
    endif()

    set(APP_TARGET "${KNIFE_EDGE_GAME_ID}_recompiled")
    add_executable(${APP_TARGET}
        ${PORT_SOURCES}
        "${NATIVE_ROOT}/register_generated.cpp"
        "${GENERATED_DIR}/lookup.cpp"
        "${RSP_DIR}/aspMain.cpp")
    target_include_directories(${APP_TARGET} PRIVATE ${PORT_INCLUDES} "${GENERATED_DIR}")
    target_compile_definitions(${APP_TARGET} PRIVATE HLSL_CPU)
    target_compile_options(${APP_TARGET} PRIVATE ${NO_STRICT_ALIASING})
    target_link_libraries(${APP_TARGET} PRIVATE ${KNIFE_EDGE_GAME_ID}_code ${PORT_LIBRARIES})
    set_target_properties(${APP_TARGET} PROPERTIES OUTPUT_NAME "${KNIFE_EDGE_GAME_ID}")

    if (APPLE)
        target_link_options(${APP_TARGET} PRIVATE -Wl,-stack_size,0x1000000)
        set_target_properties(${APP_TARGET} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${KNIFE_EDGE_DISPLAY_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.navjack.${KNIFE_EDGE_GAME_ID}")

        # recompui resolves assets and the controller database from the bundle's
        # Resources directory on macOS. Nothing but Mach-O may land in
        # Contents/MacOS or codesign refuses to seal the bundle.
        set(ASSET_DESTINATION "$<TARGET_BUNDLE_CONTENT_DIR:${APP_TARGET}>/Resources")
    else()
        # recompui resolves assets and the controller database against the
        # working directory everywhere but macOS, and main.cpp makes that the
        # executable's directory, so they go beside the executable.
        set(ASSET_DESTINATION "$<TARGET_FILE_DIR:${APP_TARGET}>")
    endif()
    if (WIN32)
        # The macOS main thread gets 16 MB above; on Windows every std::thread
        # inherits the executable's reserve, so this covers the guest threads too.
        target_link_options(${APP_TARGET} PRIVATE /STACK:16777216)
        # RT64 compiles shaders at run time on D3D12, so DXC ships with the app.
        add_custom_command(TARGET ${APP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL2::SDL2>"
                "${DXC_ROOT}/bin/x64/dxcompiler.dll"
                "${DXC_ROOT}/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:${APP_TARGET}>")
    elseif (NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(${APP_TARGET} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    add_custom_command(TARGET ${APP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FRONTEND_ROOT}/assets"
            "${ASSET_DESTINATION}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FRONTEND_ROOT}/recompcontrollerdb.txt"
            "${ASSET_DESTINATION}/recompcontrollerdb.txt")
endfunction()

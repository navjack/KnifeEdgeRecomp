# Builds the native arm64 macOS app from the recompiler output in generated/,
# the title's RSP sources in rsp/, and the shared SDL/Metal/RT64 frontend in
# native/. Every required input is checked here so a configure failure names the
# missing artifact instead of producing a placeholder binary.

function(knife_edge_add_native_app)
    set(options)
    set(one_value_args
        GAME_ID ROM_HASH INTERNAL_NAME DISPLAY_NAME WINDOW_TITLE
        PROGRAM_ID PROGRAM_NAME SAVE_TYPE)
    cmake_parse_arguments(KNIFE_EDGE "${options}" "${one_value_args}" "" ${ARGN})

    if (NOT APPLE)
        message(FATAL_ERROR "The Knife Edge native app currently requires macOS")
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

    file(GLOB RSP_SOURCES CONFIGURE_DEPENDS
        "${RSP_DIR}/*.c"
        "${RSP_DIR}/*.cpp")
    if (NOT RSP_SOURCES)
        message(FATAL_ERROR
            "Title-specific RSP sources are missing from ${RSP_DIR}. "
            "A native app must not run with an empty RSP implementation.")
    endif()

    # Hand-written native code that replaces or hooks recompiled functions.
    file(GLOB TITLE_NATIVE_SOURCES CONFIGURE_DEPENDS "${NATIVE_ROOT}/game/*.cpp")

    configure_file(
        "${NATIVE_ROOT}/game_config.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/game_config.hpp"
        @ONLY)

    set(RT64_STATIC TRUE CACHE BOOL "" FORCE)
    set(RT64_SDL_WINDOW_VULKAN FALSE CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include stdlib.h")
    add_compile_definitions(HLSL_CPU)

    set(RT64_ROOT "${STACK_ROOT}/rt64")
    set(RUNTIME_ROOT "${STACK_ROOT}/N64ModernRuntime")
    set(FRONTEND_LIBRARY_ROOT "${STACK_ROOT}/RecompFrontend")
    set(N64RECOMP_ROOT "${STACK_ROOT}/N64Recomp" CACHE PATH
        "N64Recomp checkout shared by generated code and the runtime")
    if (NOT EXISTS "${N64RECOMP_ROOT}/include/recomp.h")
        message(FATAL_ERROR "N64RECOMP_ROOT must point to this project's N64Recomp checkout")
    endif()
    add_subdirectory("${RT64_ROOT}" "${CMAKE_BINARY_DIR}/rt64")
    add_subdirectory("${RUNTIME_ROOT}" "${CMAKE_BINARY_DIR}/N64ModernRuntime")

    # RecompFrontend's recompui/recompinput reference SDL2_INCLUDE_DIRS on Apple,
    # so discovery has to happen before its subdirectory is added.
    find_package(SDL2 REQUIRED)
    if (NOT SDL2_INCLUDE_DIRS AND TARGET SDL2::SDL2)
        get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    endif()

    # RT64's shader helpers read the DXC settings from the calling scope, and
    # RT64 only sets them inside its own directory; without these recompui emits
    # a malformed shader command.
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
    find_library(ZLIB_LIBRARY NAMES z)
    get_filename_component(ZLIB_PATH "${ZLIB_LIBRARY}" DIRECTORY)
    set(DXC "DYLD_LIBRARY_PATH=${RT64_ROOT}/src/contrib/dxc/lib/arm64:${ZLIB_PATH}"
            "${RT64_ROOT}/src/contrib/dxc/bin/arm64/dxc-macos")

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

    add_library(${KNIFE_EDGE_GAME_ID}_code STATIC ${GENERATED_SOURCES})
    target_include_directories(${KNIFE_EDGE_GAME_ID}_code PRIVATE
        "${NATIVE_ROOT}" "${GENERATED_DIR}" ${RUNTIME_INCLUDES})
    target_compile_options(${KNIFE_EDGE_GAME_ID}_code PRIVATE
        -fno-strict-aliasing -Wno-unused-variable -Wno-unused-function)
    # scripts/recompile.sh declares every runtime replacement the generated code
    # calls; funcs.h only declares some of them.
    if (EXISTS "${GENERATED_DIR}/recomp_decls.h")
        target_compile_options(${KNIFE_EDGE_GAME_ID}_code PRIVATE
            -include "${GENERATED_DIR}/recomp_decls.h")
    endif()

    set(APP_TARGET "${KNIFE_EDGE_GAME_ID}_recompiled")
    add_executable(${APP_TARGET} MACOSX_BUNDLE
        "${NATIVE_ROOT}/main.cpp"
        "${NATIVE_ROOT}/macos_menu.mm"
        "${NATIVE_ROOT}/register_generated.cpp"
        "${GENERATED_DIR}/lookup.cpp"
        ${RSP_SOURCES}
        ${TITLE_NATIVE_SOURCES})
    target_include_directories(${APP_TARGET} PRIVATE
        "${NATIVE_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${GENERATED_DIR}"
        ${RUNTIME_INCLUDES}
        "${RT64_ROOT}/src"
        "${RT64_ROOT}/src/contrib"
        "${RT64_ROOT}/src/contrib/hlslpp/include"
        "${RT64_ROOT}/src/contrib/plume"
        "${RT64_ROOT}/src/contrib/nativefiledialog-extended/src/include"
        "${FRONTEND_LIBRARY_ROOT}/recompinput/include"
        "${FRONTEND_LIBRARY_ROOT}/recompui/include"
        ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(${APP_TARGET} PRIVATE HLSL_CPU)
    target_compile_options(${APP_TARGET} PRIVATE -fno-strict-aliasing)
    target_link_libraries(${APP_TARGET} PRIVATE
        ${KNIFE_EDGE_GAME_ID}_code
        recompui recompinput librecomp ultramodern rt64 nfd
        ${SDL2_LIBRARIES})
    target_link_options(${APP_TARGET} PRIVATE -Wl,-stack_size,0x1000000)
    set_target_properties(${APP_TARGET} PROPERTIES
        OUTPUT_NAME "${KNIFE_EDGE_GAME_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "${KNIFE_EDGE_DISPLAY_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.navjack.${KNIFE_EDGE_GAME_ID}")

    # recompui resolves assets and the controller database from the bundle's
    # Resources directory on macOS. Nothing but Mach-O may land in Contents/MacOS
    # or codesign refuses to seal the bundle.
    add_custom_command(TARGET ${APP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FRONTEND_ROOT}/assets"
            "$<TARGET_BUNDLE_CONTENT_DIR:${APP_TARGET}>/Resources/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FRONTEND_ROOT}/recompcontrollerdb.txt"
            "$<TARGET_BUNDLE_CONTENT_DIR:${APP_TARGET}>/Resources/recompcontrollerdb.txt")
endfunction()

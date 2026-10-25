cmake_minimum_required(VERSION 3.24)
project(tdkr VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)   # (render/metalfx.mm)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Linux uses its packages; Windows and macOS build every library from source.
if(WIN32 OR APPLE)
    set(TDKR_PORTABLE_DEFAULT ON)
else()
    set(TDKR_PORTABLE_DEFAULT OFF)
endif()
option(TDKR_VENDORED "Build SDL3, SDL3_ttf, zlib, Lua 5.1, shaderc and GLM from pinned sources" ${TDKR_PORTABLE_DEFAULT})
option(TDKR_SHADERCROSS "Direct3D 12 and Metal through SDL_shadercross (else Vulkan only)" ${TDKR_PORTABLE_DEFAULT})
include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

if(MSVC)
    set(TDKR_WARNINGS /W4 /utf-8)
else()
    set(TDKR_WARNINGS -Wall -Wextra)
endif()

# Engine core: file system, asset decoders and the game-shader compiler (no rendering).
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/assets/*.cpp src/shader/*.cpp src/script/*.cpp src/swf/*.cpp)
add_library(tdkrcore STATIC ${CORE_SOURCES})
target_include_directories(tdkrcore PUBLIC src)
target_link_libraries(tdkrcore PUBLIC tdkr::zlib tdkr::shaderc tdkr::lua tdkr::glm)
target_compile_options(tdkrcore PRIVATE ${TDKR_WARNINGS})

# The renderer's own shaders: GLSL -> SPIR-V at build time, with the build machine's glslc
# (cross builds too); other GPU formats come from the SPIR-V at run time.
find_program(GLSLC glslc)
if(NOT GLSLC)
    if(TARGET glslc_exe AND NOT CMAKE_CROSSCOMPILING)
        set(GLSLC $<TARGET_FILE:glslc_exe>)
    else()
        message(FATAL_ERROR "glslc not found: install shaderc (or the Vulkan SDK) on the build machine")
    endif()
endif()
file(GLOB SHADER_SOURCES RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS src/render/shaders/*.vert src/render/shaders/*.frag
     src/render/shaders/*.comp)
set(SHADER_OUTPUTS)
foreach(src ${SHADER_SOURCES})
    get_filename_component(name ${src} NAME)
    set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLC} -O ${CMAKE_SOURCE_DIR}/${src} -o ${out}
        DEPENDS ${src} VERBATIM)
    list(APPEND SHADER_OUTPUTS ${out})
endforeach()
# HLSL ones (the FidelityFX passes, around third_party/fidelityfx's MIT sources): NAME.STAGE.hlsl
# -> NAME.STAGE.spv, separate textures and samplers folded into combined ones as SDL's GPU API
# binds them.
file(GLOB HLSL_SOURCES RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS src/render/shaders/*.hlsl)
file(GLOB_RECURSE FFX_HEADERS CONFIGURE_DEPENDS third_party/fidelityfx/*.h)
foreach(src ${HLSL_SOURCES})
    get_filename_component(name ${src} NAME)
    string(REGEX REPLACE "\\.hlsl$" "" base ${name})
    string(REGEX MATCH "[a-z]+$" stage ${base})
    set(out ${CMAKE_BINARY_DIR}/shaders/${base}.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLC} -x hlsl -fshader-stage=${stage} -fentry-point=main -fauto-combined-image-sampler -O
                -I${CMAKE_SOURCE_DIR}/third_party/fidelityfx/gpu ${CMAKE_SOURCE_DIR}/${src} -o ${out}
        DEPENDS ${src} ${FFX_HEADERS} VERBATIM)
    list(APPEND SHADER_OUTPUTS ${out})
endforeach()
# FSR 3's upscaler passes as they ship (third_party/fidelityfx/shaders), as compute shaders in
# SDL's layout: its textures (t) in set 0, storage images (u) in set 1, constants (b) in set 2.
# Render-size motion vectors, reversed depth; accumulate also with sharpening (then RCAS); the
# pyramids' downsampling (SPD) through group memory, not subgroup operations (Vulkan 1.1).
set(FFX_REGISTERS)
foreach(i RANGE 15)
    list(APPEND FFX_REGISTERS t${i} 0 ${i} u${i} 1 ${i})
endforeach()
foreach(i RANGE 3)
    list(APPEND FFX_REGISTERS b${i} 2 ${i})
endforeach()
foreach(pass prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity luma_instability
             accumulate accumulate_sharpen rcas debug_view)
    string(REGEX REPLACE "_sharpen$" "" source ${pass})
    set(sharpen 0)
    if(pass STREQUAL "accumulate_sharpen")
        set(sharpen 1)
    endif()
    set(src third_party/fidelityfx/shaders/ffx_fsr3upscaler_${source}_pass.hlsl)
    set(out ${CMAKE_BINARY_DIR}/shaders/fsr3_${pass}.comp.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLC} -x hlsl -fshader-stage=comp -fentry-point=CS -fauto-combined-image-sampler -fhlsl-offsets -O
                -I${CMAKE_SOURCE_DIR}/third_party/fidelityfx/gpu -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_HALF=0
                -DFFX_SPD_NO_WAVE_OPERATIONS=1
                -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1 -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1
                -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen} -fresource-set-binding ${FFX_REGISTERS}
                ${CMAKE_SOURCE_DIR}/${src} -o ${out}
        DEPENDS ${src} ${FFX_HEADERS} VERBATIM)
    list(APPEND SHADER_OUTPUTS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

# The game.
file(GLOB VIEWER_SOURCES CONFIGURE_DEPENDS src/main.cpp src/app/*.cpp src/render/*.cpp src/game/*.cpp src/audio/*.cpp)
add_executable(tdkr ${VIEWER_SOURCES})
set_target_properties(tdkr PROPERTIES WIN32_EXECUTABLE ON)   # no console window of its own
target_link_libraries(tdkr PRIVATE tdkrcore tdkr::sdl3 tdkr::sdl3_ttf)
target_compile_options(tdkr PRIVATE ${TDKR_WARNINGS})
if(TDKR_SHADERCROSS)
    target_link_libraries(tdkr PRIVATE tdkr::shadercross)
    target_compile_definitions(tdkr PRIVATE TDKR_SHADERCROSS=1)
endif()
if(TDKR_NATIVE_GPU)
    target_compile_definitions(tdkr PRIVATE TDKR_NATIVE_GPU=1)
endif()
# The patched SDL as a shared library: the game finds it beside itself (and, installed, in
# ../lib), before the system's.
if(NOT TDKR_VENDORED AND TDKR_PATCHED_SDL)
    set_target_properties(tdkr PROPERTIES BUILD_RPATH "$<TARGET_FILE_DIR:SDL3::SDL3-shared>"
                                          INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

# NVIDIA DLSS (render/dlss.cpp): with the DLSS SDK (github.com/NVIDIA/DLSS; its licence is
# NVIDIA's own and the SDK is not part of this repository). -DTDKR_DLSS_SDK=DIR, by default
# ../work/ext/DLSS when it is there. The DLSS runtime goes beside the game.
set(TDKR_DLSS_SDK "" CACHE PATH "The NVIDIA DLSS SDK (github.com/NVIDIA/DLSS) for DLSS; empty: without")
set(dlss_sdk "${TDKR_DLSS_SDK}")
if(NOT dlss_sdk AND EXISTS ${CMAKE_SOURCE_DIR}/../work/ext/DLSS/include/nvsdk_ngx.h)
    set(dlss_sdk ${CMAKE_SOURCE_DIR}/../work/ext/DLSS)
endif()
if(dlss_sdk AND TDKR_NATIVE_GPU AND NOT APPLE)
    find_path(TDKR_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS $ENV{VULKAN_SDK}/include)
    if(WIN32)
        set(dlss_lib ${dlss_sdk}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib)
        file(GLOB dlss_runtime ${dlss_sdk}/lib/Windows_x86_64/rel/nvngx_dlss.dll)
    else()
        set(dlss_lib ${dlss_sdk}/lib/Linux_x86_64/libnvsdk_ngx.a)
        file(GLOB dlss_runtime ${dlss_sdk}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*)
    endif()
    if(EXISTS ${dlss_lib} AND dlss_runtime AND TDKR_VULKAN_INCLUDE_DIR)
        message(STATUS "DLSS: ${dlss_sdk}")
        target_include_directories(tdkr PRIVATE ${dlss_sdk}/include ${TDKR_VULKAN_INCLUDE_DIR})
        target_link_libraries(tdkr PRIVATE ${dlss_lib} ${CMAKE_DL_LIBS})
        target_compile_definitions(tdkr PRIVATE TDKR_DLSS=1)
        add_custom_command(TARGET tdkr POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dlss_runtime} $<TARGET_FILE_DIR:tdkr> VERBATIM)
        set(TDKR_DLSS_RUNTIME ${dlss_runtime})
    else()
        message(STATUS "DLSS: no NGX library, DLSS runtime or Vulkan headers in ${dlss_sdk}; without DLSS")
    endif()
endif()

# Apple MetalFX (render/metalfx.mm), with the patched SDL's Metal objects.
if(APPLE AND TDKR_NATIVE_GPU)
    target_sources(tdkr PRIVATE src/render/metalfx.mm)
    target_compile_options(tdkr PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(tdkr PRIVATE TDKR_METALFX=1)
    target_link_libraries(tdkr PRIVATE "-framework Metal" "-framework MetalFX" "-weak_framework MetalFX")
endif()
add_dependencies(tdkr shaders)
# The shaders sit beside the executable (SDL_GetBasePath()/shaders).
add_custom_command(TARGET tdkr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:tdkr>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_OUTPUTS} $<TARGET_FILE_DIR:tdkr>/shaders
    VERBATIM)

# Command-line checks of the decoders against the Python reference tools.
add_executable(tdkr-check tools/check.cpp tools/swf_check.cpp)
target_link_libraries(tdkr-check PRIVATE tdkrcore)
if(TDKR_SHADERCROSS)
    target_link_libraries(tdkr-check PRIVATE tdkr::shadercross tdkr::sdl3)
    target_compile_definitions(tdkr-check PRIVATE TDKR_SHADERCROSS=1)
endif()

# Installation and packages: the game, its shaders and the checks. The game's own data
# is never part of them; players point --data at their copy.
if(WIN32 OR APPLE)
    set(TDKR_INSTALL_BIN .)
else()
    set(TDKR_INSTALL_BIN ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS tdkr tdkr-check RUNTIME DESTINATION ${TDKR_INSTALL_BIN} COMPONENT game)
if(TDKR_DLSS_RUNTIME)
    install(FILES ${TDKR_DLSS_RUNTIME} DESTINATION ${TDKR_INSTALL_BIN} COMPONENT game)
endif()
if(NOT TDKR_VENDORED AND TDKR_PATCHED_SDL)
    install(TARGETS SDL3-shared LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT game NAMELINK_SKIP)
endif()
install(FILES ${SHADER_OUTPUTS} DESTINATION ${TDKR_INSTALL_BIN}/shaders COMPONENT game)
install(FILES ${CMAKE_SOURCE_DIR}/../README.md DESTINATION ${TDKR_INSTALL_BIN} COMPONENT game OPTIONAL)
# Vendored libraries add install rules of their own (headers, static libraries); the
# packages hold only the game.
set(CPACK_COMPONENTS_ALL game)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_PACKAGE_NAME tdkr)
set(CPACK_PACKAGE_VENDOR "The Dark Knight Rises reimplementation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A native reimplementation of The Dark Knight Rises (Gameloft) that runs the original game's files")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)

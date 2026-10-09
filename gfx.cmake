# Vulkan renderer, font atlas and the gfx_demo test program.
# Included from the top-level CMakeLists.txt when PYJUKE_GFX is ON.
#
# This file is also run in CMake script mode (cmake -P gfx.cmake) by the build
# to turn a compiled .spv file into a C++ header; that part is at the top.

if(CMAKE_SCRIPT_MODE_FILE)
    # Arguments: -DSPV=<input.spv> -DOUT=<output.h> -DNAME=<array name>
    file(READ "${SPV}" spv_hex HEX)
    string(LENGTH "${spv_hex}" hex_length)
    math(EXPR word_count "${hex_length} / 8")
    # SPIR-V is a stream of little-endian 32-bit words as written by the
    # compiler on this machine; regroup bytes into words (b3 b2 b1 b0).
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u," words "${spv_hex}")
    string(REGEX REPLACE "(([^,]*,){8})" "\\1\n    " words "${words}")
    file(WRITE "${OUT}"
        "// Generated from ${SPV} by gfx.cmake; do not edit.\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        "#include <cstddef>\n"
        "inline constexpr uint32_t ${NAME}[] = {\n    ${words}\n};\n"
        "inline constexpr size_t ${NAME}_size = ${word_count} * sizeof(uint32_t);\n")
    return()
endif()

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(Freetype REQUIRED)

# Shader compiler: glslangValidator (preferred) or glslc, found on PATH or in
# the Vulkan SDK.
find_program(PYJUKE_GLSLANG glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")
find_program(PYJUKE_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")
if(PYJUKE_GLSLANG)
    set(pyjuke_shader_compile ${PYJUKE_GLSLANG} -V)
elseif(PYJUKE_GLSLC)
    set(pyjuke_shader_compile ${PYJUKE_GLSLC})
else()
    message(FATAL_ERROR "Neither glslangValidator nor glslc was found; install the Vulkan SDK or glslang.")
endif()

set(pyjuke_shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${pyjuke_shader_out})

set(pyjuke_shader_headers)
foreach(stage vert frag)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.${stage})
    set(spv ${pyjuke_shader_out}/ui.${stage}.spv)
    set(hdr ${pyjuke_shader_out}/ui_${stage}_spv.h)
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${pyjuke_shader_compile} -o ${spv} ${src}
        COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DOUT=${hdr} -DNAME=ui_${stage}_spv
                -P ${CMAKE_CURRENT_SOURCE_DIR}/gfx.cmake
        DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/gfx.cmake
        COMMENT "Compiling and embedding shaders/ui.${stage}"
        VERBATIM)
    list(APPEND pyjuke_shader_headers ${hdr})
endforeach()
add_custom_target(pyjuke_shaders DEPENDS ${pyjuke_shader_headers})

add_library(pyjuke_gfx STATIC
    vulkan_support.cpp
    font.cpp
    renderer.cpp
    ${pyjuke_shader_headers})
add_dependencies(pyjuke_gfx pyjuke_shaders)
target_include_directories(pyjuke_gfx
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${pyjuke_shader_out})
target_link_libraries(pyjuke_gfx PUBLIC Vulkan::Vulkan glfw Freetype::Freetype)
target_compile_options(pyjuke_gfx PRIVATE ${PYJUKE_WARNINGS})
# GLFW should not pull in an OpenGL header; we use Vulkan only.
target_compile_definitions(pyjuke_gfx PUBLIC GLFW_INCLUDE_NONE)

add_executable(gfx_demo gfx_demo.cpp)
target_link_libraries(gfx_demo PRIVATE pyjuke_gfx)
target_compile_options(gfx_demo PRIVATE ${PYJUKE_WARNINGS})

# Widget event routing (no window or renderer needed to run).
add_executable(ui_tests tests/test_ui.cpp ui.cpp)
target_link_libraries(ui_tests PRIVATE pyjuke_gfx)
target_compile_options(ui_tests PRIVATE ${PYJUKE_WARNINGS})
add_test(NAME ui_tests COMMAND ui_tests)

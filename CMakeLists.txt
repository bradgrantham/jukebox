cmake_minimum_required(VERSION 3.20)

project(pyjuke CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Applied per-target to our own sources; not to the minimp3 implementation TU.
if(MSVC)
    set(PYJUKE_WARNINGS /W4 /WX)
else()
    set(PYJUKE_WARNINGS -Wall -Wextra -pedantic -Werror)
endif()

option(PYJUKE_CORE "Build audio, library, ID3, player logic and tests" ON)
option(PYJUKE_GFX "Build Vulkan renderer and font code" ON)

enable_testing()

if(PYJUKE_CORE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/core.cmake)
endif()

if(PYJUKE_GFX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/gfx.cmake)
endif()

if(PYJUKE_CORE AND PYJUKE_GFX)
    add_executable(pyjuke main.cpp ui.cpp)
    target_compile_options(pyjuke PRIVATE ${PYJUKE_WARNINGS})
    target_link_libraries(pyjuke PRIVATE pyjuke_core pyjuke_gfx)
endif()

# core.cmake - audio, library, ID3, player logic, config, UPnP output, CLI and tests.
# Included from CMakeLists.txt when PYJUKE_CORE is ON.

find_package(PkgConfig REQUIRED)
pkg_check_modules(AO REQUIRED IMPORTED_TARGET ao)
find_package(Threads REQUIRED)

# Third-party decoder: compiled on its own, without our warning flags.
add_library(pyjuke_minimp3 OBJECT minimp3_impl.cpp)
target_include_directories(pyjuke_minimp3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(pyjuke_minimp3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(pyjuke_core STATIC
    audio.cpp
    config.cpp
    id3.cpp
    library.cpp
    net.cpp
    player.cpp
    remote_renderer.cpp
    track_server.cpp
    upnp.cpp
    $<TARGET_OBJECTS:pyjuke_minimp3>
)
target_include_directories(pyjuke_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyjuke_core PRIVATE ${PYJUKE_WARNINGS})
target_link_libraries(pyjuke_core PUBLIC PkgConfig::AO Threads::Threads)

add_executable(pyjuke-cli cli.cpp)
target_compile_options(pyjuke-cli PRIVATE ${PYJUKE_WARNINGS})
target_link_libraries(pyjuke-cli PRIVATE pyjuke_core)

add_executable(pyjuke_tests tests/test_main.cpp tests/test_network.cpp)
target_compile_options(pyjuke_tests PRIVATE ${PYJUKE_WARNINGS})
target_link_libraries(pyjuke_tests PRIVATE pyjuke_core)
add_test(NAME pyjuke_tests COMMAND pyjuke_tests)

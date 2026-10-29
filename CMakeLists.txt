cmake_minimum_required(VERSION 4.3)
project(flux VERSION 0.4.0 LANGUAGES CXX) # x-release-please-version

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ckdl
    SYSTEM
    GIT_REPOSITORY https://github.com/tjol/ckdl.git
    GIT_TAG        c9c33fe64446287215e80705545139d92a48f829
)
FetchContent_MakeAvailable(ckdl)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET wlroots-0.20 wayland-server xkbcommon)

find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
set(GEN_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${GEN_PROTO_DIR}")

function(generate_protocol proto)
    get_filename_component(proto_name "${proto}" NAME_WLE)
    set(xml_path "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${proto_name}.xml")
    set(header_out "${GEN_PROTO_DIR}/${proto_name}-protocol.h")

    add_custom_command(
        OUTPUT "${header_out}"
        COMMAND ${WAYLAND_SCANNER} server-header "${xml_path}" "${header_out}"
        DEPENDS "${xml_path}"
        COMMENT "Generating ${proto_name}-protocol.h"
        VERBATIM
    )

    target_sources(flux PRIVATE "${header_out}")
endfunction()

add_executable(flux
        src/main.cpp
        src/compositor.cpp
        src/scene.cpp
        src/output.cpp
        src/input/input.cpp
        src/input/seat.cpp
        src/input/cursor.cpp
        src/input/keyboard.cpp
        src/input/pointer.cpp
        src/window.cpp
        src/popup.cpp
        src/layer.cpp
        src/config.cpp
        src/log.cpp
)

target_include_directories(flux PRIVATE include "${GEN_PROTO_DIR}")
target_link_libraries(flux PRIVATE PkgConfig::DEPS kdlpp)
target_compile_definitions(flux PRIVATE FLUX_VERSION="${PROJECT_VERSION}")

generate_protocol(wlr-layer-shell-unstable-v1)

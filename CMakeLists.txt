cmake_minimum_required(VERSION 4.3)
project(frost VERSION 0.10.1 LANGUAGES CXX) # x-release-please-version

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ckdl
    SYSTEM
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/tjol/ckdl.git
    GIT_TAG        c9c33fe64446287215e80705545139d92a48f829
)
FetchContent_MakeAvailable(ckdl)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    SYSTEM
    EXCLUDE_FROM_ALL
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS 3.12
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET wlroots-0.20 wayland-server xkbcommon libinput xcb pixman-1 libcanberra)

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

    target_sources(frost PRIVATE "${header_out}")
endfunction()

add_executable(frost
        src/main.cpp
        src/compositor.cpp
        src/bell.cpp
        src/scene.cpp
        src/output/output.cpp
        src/output/manager.cpp
        src/lock.cpp
        src/input/input.cpp
        src/input/seat.cpp
        src/input/cursor.cpp
        src/input/keyboard.cpp
        src/input/pointer.cpp
        src/input/grab.cpp
        src/window/window.cpp
        src/window/xdg.cpp
        src/window/xwayland.cpp
        src/layout/layout.cpp
        src/layout/master.cpp
        src/layout/dwindle.cpp
        src/layout/monocle.cpp
        src/workspace.cpp
        src/popup.cpp
        src/layer.cpp
        src/config.cpp
        src/log.cpp
        src/animation.cpp
        src/ipc/server.cpp
)

target_include_directories(frost PRIVATE include "${GEN_PROTO_DIR}")
target_link_libraries(frost PRIVATE PkgConfig::DEPS kdlpp nlohmann_json::nlohmann_json)
target_compile_definitions(frost PRIVATE FROST_VERSION="${PROJECT_VERSION}")
target_compile_options(frost PRIVATE -Wall -Wextra -Wno-c23-extensions)

generate_protocol(wlr-layer-shell-unstable-v1)
generate_protocol(wlr-output-power-management-unstable-v1)

add_executable(frostctl src/ipc/client.cpp)
target_link_libraries(frostctl PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(frostctl PRIVATE -Wall -Wextra)

include(GNUInstallDirs)

install(TARGETS frost frostctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/frost.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
install(FILES resources/frost-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)

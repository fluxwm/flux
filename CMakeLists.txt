cmake_minimum_required(VERSION 4.3)
project(flux LANGUAGES CXX)

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

add_executable(flux
        src/main.cpp
        src/server.cpp
        src/output.cpp
        src/keyboard.cpp
        src/window.cpp
        src/popup.cpp
        src/config.cpp
)

target_include_directories(flux PRIVATE include)
target_link_libraries(flux PRIVATE PkgConfig::DEPS kdlpp)

cmake_minimum_required(VERSION 3.21)

project(raum
    VERSION 0.1.0
    DESCRIPTION "A 3D desktop: a Wayland compositor on wlroots, drawn by the fitzel engine"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# --- The engine ------------------------------------------------------------------
# fitzel is built from its source tree as a subproject. Its dependencies are the
# sources fitzel's own Linux build already fetched, so configuring raum does not
# download the world a second time (FETCHCONTENT_SOURCE_DIR_<NAME> per package).
set(FITZEL_DIR "$ENV{HOME}/Documents/devel/fitzel" CACHE PATH "The fitzel source tree")
set(FITZEL_DEPS_DIR "${FITZEL_DIR}/build/linux/_deps" CACHE PATH
    "Where fitzel's build keeps its fetched dependency sources")
if(NOT EXISTS "${FITZEL_DIR}/engine/CMakeLists.txt")
    message(FATAL_ERROR "fitzel not found at ${FITZEL_DIR} (set -DFITZEL_DIR=...)")
endif()
foreach(dep glfw glm glad stb imgui imguizmo imcolortextedit gamecontrollerdb tinyexr
            miniaudio cgltf assimp lua nlohmann_json jolt)
    string(TOUPPER "${dep}" DEP)
    if(EXISTS "${FITZEL_DEPS_DIR}/${dep}-src")
        set(FETCHCONTENT_SOURCE_DIR_${DEP} "${FITZEL_DEPS_DIR}/${dep}-src" CACHE PATH "")
    endif()
endforeach()
set(FITZEL_BUILD_SANDBOX OFF CACHE BOOL "" FORCE)
set(FITZEL_VULKAN OFF CACHE BOOL "" FORCE)   # raum draws through OpenGL only
set(FITZEL_VST3 OFF CACHE BOOL "" FORCE)
add_subdirectory("${FITZEL_DIR}" fitzel EXCLUDE_FROM_ALL)

# --- System libraries ------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.19)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-server wayland-client)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

# --- Protocol code ---------------------------------------------------------------
# Server headers the wlroots headers include by name, and the one client protocol
# raum speaks to the desktop it runs nested in (asking it to pass Super through).
set(PROTO_DIR "${CMAKE_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${PROTO_DIR}")
set(PROTO_SOURCES)
function(raum_protocol xml base kind)
    set(header "${PROTO_DIR}/${base}-protocol.h")
    set(code   "${PROTO_DIR}/${base}-protocol.c")
    if(kind STREQUAL "server")
        set(hdrcmd server-header)
    else()
        set(hdrcmd client-header)
    endif()
    add_custom_command(OUTPUT "${header}"
        COMMAND "${WAYLAND_SCANNER}" ${hdrcmd} "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${code}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
        DEPENDS "${xml}" VERBATIM)
    set(PROTO_SOURCES ${PROTO_SOURCES} "${header}" "${code}" PARENT_SCOPE)
endfunction()
set(WP "${WAYLAND_PROTOCOLS_DIR}")
raum_protocol("${WP}/stable/xdg-shell/xdg-shell.xml" xdg-shell server)
raum_protocol("${WP}/staging/cursor-shape/cursor-shape-v1.xml" cursor-shape-v1 server)
raum_protocol("${WP}/stable/tablet/tablet-v2.xml" tablet-v2 server)
raum_protocol("${WP}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
              pointer-constraints-unstable-v1 server)
raum_protocol("${WP}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml"
              keyboard-shortcuts-inhibit-unstable-v1-client client)
# raum's own: a client drawing its world into a room (src/Stage.hpp). fitzel's
# player speaks the other side, from a copy in fitzel/sandbox/protocols.
raum_protocol("${CMAKE_CURRENT_SOURCE_DIR}/protocols/raum-stage-v1.xml" raum-stage-v1 server)
# raum's own window on the host desktop (HostWindow) speaks xdg-shell as a
# client: only the header -- the interface tables are the server's above.
add_custom_command(OUTPUT "${PROTO_DIR}/xdg-shell-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" client-header "${WP}/stable/xdg-shell/xdg-shell.xml"
            "${PROTO_DIR}/xdg-shell-client-protocol.h"
    DEPENDS "${WP}/stable/xdg-shell/xdg-shell.xml" VERBATIM)
list(APPEND PROTO_SOURCES "${PROTO_DIR}/xdg-shell-client-protocol.h")
add_library(raum_protocols STATIC ${PROTO_SOURCES})
target_include_directories(raum_protocols PUBLIC "${PROTO_DIR}")
target_link_libraries(raum_protocols PUBLIC PkgConfig::WAYLAND)

# --- raum ------------------------------------------------------------------------
add_executable(raum
    src/main.cpp
    src/Server.cpp
    src/Output.cpp
    src/InputHandling.cpp
    src/View.cpp
    src/XwaylandView.cpp
    src/Gpu.cpp
    src/SurfaceTextures.cpp
    src/Stage.cpp
    src/Desk.cpp
    src/DeskRenderer.cpp
    src/Hud.cpp
    src/Apps.cpp
    src/HostShortcuts.cpp
    src/HostWindow.cpp
    src/Sfx.cpp
    src/HoloIcons.cpp
    src/RingMenu.cpp
    src/State.cpp
)
target_compile_definitions(raum PRIVATE WLR_USE_UNSTABLE RAUM_VERSION="${PROJECT_VERSION}"
    RAUM_FITZEL_DIR="${FITZEL_DIR}")
target_include_directories(raum PRIVATE src)
target_link_libraries(raum PRIVATE
    fitzel
    glad_gl_core_43          # the engine's loader: one copy, filled by the device
    stb_headers
    nlohmann_json::nlohmann_json
    raum_protocols
    PkgConfig::WLROOTS PkgConfig::WAYLAND PkgConfig::XKB PkgConfig::EGL
    PkgConfig::PIXMAN PkgConfig::LIBDRM)
target_compile_options(raum PRIVATE -Wall -Wextra -Wno-missing-field-initializers
    -Wno-unused-parameter)

# No install() rules: the engine's dependencies bring their own, and an install
# would copy assimp's and GLFW's headers along. scripts/install.sh copies the
# three files raum needs instead.

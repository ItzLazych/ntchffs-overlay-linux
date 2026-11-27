cmake_minimum_required(VERSION 3.24)
project(ntchffs-overlay VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOBS REQUIRED libobs)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(XFIXES REQUIRED IMPORTED_TARGET xfixes)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Svg
    WaylandClient
    WaylandScannerTools
)
find_package(Qt6 QUIET OPTIONAL_COMPONENTS WaylandClientPrivate)

find_library(OBS_LIBRARY NAMES obs HINTS ${LIBOBS_LIBRARY_DIRS} /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 REQUIRED)
find_library(OBS_FRONTEND_LIBRARY NAMES obs-frontend-api HINTS ${LIBOBS_LIBRARY_DIRS} /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 REQUIRED)

pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
set(_xdg_shell_candidates "")
if(WAYLAND_PROTOCOLS_FOUND)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir wayland-protocols
        OUTPUT_VARIABLE _wl_proto_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_wl_proto_dir)
        list(APPEND _xdg_shell_candidates "${_wl_proto_dir}/stable/xdg-shell/xdg-shell.xml")
    endif()
endif()
list(APPEND _xdg_shell_candidates
    /usr/share/qt6/wayland/protocols/xdg-shell/xdg-shell.xml
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    /usr/local/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
)
set(XDG_SHELL_XML_PATH "")
foreach(_cand IN LISTS _xdg_shell_candidates)
    if(EXISTS "${_cand}")
        set(XDG_SHELL_XML_PATH "${_cand}")
        break()
    endif()
endforeach()
if(NOT XDG_SHELL_XML_PATH)
    message(FATAL_ERROR "Could not locate xdg-shell.xml (install wayland-protocols or qt6-wayland dev files)")
endif()

set(PLUGIN_SOURCES
    src/plugin/plugin-support.c
    src/plugin/plugin-main.cpp
    src/plugin/overlay-settings-storage.cpp
    src/plugin/overlay-smart-replay.cpp
    src/plugin/overlay-clip-save.cpp
    src/common/overlay-string-utils.cpp
    src/common/overlay-svg-rasterize.cpp
    src/common/overlay-gif-decode.cpp
    src/common/overlay-icon-frames.cpp
    src/common/overlay-mp4-box-reader.cpp
    src/window/overlay-wayland-layer.cpp
    src/window/overlay-window-manager.cpp
    src/window/overlay-window-c.cpp
    src/renderer/overlay-layout.cpp
    src/renderer/overlay-state.cpp
    src/renderer/overlay-gallery.cpp
    src/renderer/overlay-renderer.cpp
    src/renderer/overlay-renderer-input.cpp
    src/export/overlay-rebuild-preset.cpp
    src/export/overlay-export-queue.cpp
    src/trimmer/overlay-trimmer.cpp
    src/player/overlay-video-player.cpp
    src/indicators/overlay-indicators.cpp
    src/indicators/overlay-indicators-c.cpp
    src/toasts/overlay-toasts.cpp
    src/toasts/overlay-toasts-c.cpp
)

add_library(ntchffs-overlay MODULE ${PLUGIN_SOURCES})

set_target_properties(ntchffs-overlay PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(Qt6WaylandClient_VERSION VERSION_GREATER_EQUAL "6.8.0")
    qt6_generate_wayland_protocol_client_sources(ntchffs-overlay
        PRIVATE_CODE
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            ${XDG_SHELL_XML_PATH}
    )
else()
    qt6_generate_wayland_protocol_client_sources(ntchffs-overlay
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            ${XDG_SHELL_XML_PATH}
    )
endif()

target_include_directories(ntchffs-overlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin
    ${CMAKE_CURRENT_SOURCE_DIR}/src/window
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/export
    ${CMAKE_CURRENT_SOURCE_DIR}/src/trimmer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
    ${CMAKE_CURRENT_SOURCE_DIR}/src/indicators
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toasts
    ${CMAKE_CURRENT_BINARY_DIR}
    ${LIBOBS_INCLUDE_DIRS}
    /usr/include/obs
)

target_compile_definitions(ntchffs-overlay PRIVATE
    HAVE_OBSCONFIG_H
    PLUGIN_NAME="ntchffs-overlay"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_compile_options(ntchffs-overlay PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>
)

target_link_libraries(ntchffs-overlay PRIVATE
    ${OBS_LIBRARY}
    ${OBS_FRONTEND_LIBRARY}
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::WaylandClient
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::PULSE_SIMPLE
    PkgConfig::X11
    PkgConfig::XEXT
    PkgConfig::XFIXES
    pthread
)

if(TARGET Qt6::WaylandClientPrivate)
    target_link_libraries(ntchffs-overlay PRIVATE Qt6::WaylandClientPrivate)
elseif(DEFINED Qt6WaylandClient_PRIVATE_INCLUDE_DIRS)
    target_include_directories(ntchffs-overlay PRIVATE ${Qt6WaylandClient_PRIVATE_INCLUDE_DIRS})
endif()

cmake_minimum_required(VERSION 3.21)
project(vantapaper VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# QRhi lives in Qt's "rhi" headers, exposed through the GuiPrivate component.
find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate ShaderTools Network Concurrent)
find_package(LayerShellQt REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# libultrahdr (libuhdr) for decoding UltraHDR gain-map JPEGs.
find_library(UHDR_LIB NAMES uhdr ultrahdr REQUIRED)

# libavif for decoding AVIF (true HDR: PQ/HLG, 10/12-bit).
pkg_check_modules(Avif REQUIRED IMPORTED_TARGET libavif)

# lcms2 to read embedded ICC profiles (Lightroom tags HDR via ICC, not CICP).
pkg_check_modules(Lcms2 REQUIRED IMPORTED_TARGET lcms2)

# libjxl for JPEG-XL (HDR PQ/HLG and SDR).
pkg_check_modules(Jxl REQUIRED IMPORTED_TARGET libjxl)

# libheif for HEIC/HEIF (HDR PQ/HLG, 10/12-bit).
pkg_check_modules(Heif REQUIRED IMPORTED_TARGET libheif)

# FFmpeg for video wallpapers: demux + decode (NVDEC via cuda hwaccel, sw fallback).
pkg_check_modules(FFmpeg REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

# --- Generate the wp-color-management-v1 client protocol glue ---
set(CM_XML "${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml")
set(CM_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wl-protocols")
file(MAKE_DIRECTORY "${CM_GEN_DIR}")
set(CM_HEADER "${CM_GEN_DIR}/color-management-v1-client-protocol.h")
set(CM_SOURCE "${CM_GEN_DIR}/color-management-v1.c")
add_custom_command(
    OUTPUT "${CM_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${CM_XML}" "${CM_HEADER}"
    DEPENDS "${CM_XML}"
    VERBATIM)
add_custom_command(
    OUTPUT "${CM_SOURCE}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${CM_XML}" "${CM_SOURCE}"
    DEPENDS "${CM_XML}"
    VERBATIM)

add_executable(vantapaper
    src/main.cpp
    src/rhiwindow.cpp
    src/cm_tagging.cpp
    src/hdr_image.cpp
    src/playlist.cpp
    src/daemon.cpp
    src/video_source.cpp
    src/state_frame.cpp
    "${CM_SOURCE}"
    "${CM_HEADER}"
)

target_include_directories(vantapaper PRIVATE "${CM_GEN_DIR}")

target_link_libraries(vantapaper PRIVATE
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Network
    Qt6::Concurrent
    LayerShellQt::Interface
    PkgConfig::WaylandClient
    PkgConfig::Avif
    PkgConfig::Lcms2
    PkgConfig::Jxl
    PkgConfig::Heif
    PkgConfig::FFmpeg
    ${UHDR_LIB}
)

# Compile the GLSL to .qsb and embed under :/shaders/<name>.qsb
# BASE strips the "shaders/" dir from the resource alias so paths stay clean.
qt6_add_shaders(vantapaper "vantapaper_shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    PREFIX "/shaders"
    FILES
        shaders/fullscreen.vert
        shaders/image.frag
        shaders/video_convert.frag
)

include(GNUInstallDirs)
install(TARGETS vantapaper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

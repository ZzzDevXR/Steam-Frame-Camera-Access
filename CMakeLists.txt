cmake_minimum_required(VERSION 3.16)
project(frame_camera_access VERSION 0.1.0 LANGUAGES C CXX)

option(FCA_CLIENT_ONLY "Build only the client library" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "frame-camera-access targets Linux (the Steam Frame)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(FCA_WARNINGS -Wall -Wextra -Wshadow)

add_library(frame_camera_access SHARED native/client/fca_client.c)
target_include_directories(frame_camera_access PUBLIC native/include PRIVATE native/common)
target_compile_options(frame_camera_access PRIVATE ${FCA_WARNINGS})
set_target_properties(frame_camera_access PROPERTIES C_VISIBILITY_PRESET hidden)
target_link_options(frame_camera_access PRIVATE -Wl,--no-undefined -Wl,-z,defs)

if(FCA_CLIENT_ONLY)
  return()
endif()

add_executable(fca_probe native/tools/fca_probe.c)
target_link_libraries(fca_probe PRIVATE frame_camera_access)
target_compile_options(fca_probe PRIVATE ${FCA_WARNINGS})
set_target_properties(fca_probe PROPERTIES BUILD_RPATH "$ORIGIN")

add_executable(frame-camera-accessd
  native/daemon/main.cpp
  native/daemon/shared_ring.cpp
  native/daemon/server.cpp
  native/daemon/synthetic_source.cpp
  native/daemon/frame_dma_source.cpp
  native/daemon/access.cpp
  native/daemon/control.cpp)
target_include_directories(frame-camera-accessd PRIVATE native/include native/common native/daemon)
target_compile_options(frame-camera-accessd PRIVATE ${FCA_WARNINGS})

set(FCA_OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory with SteamVR's libopenvr_api.so")
set(FCA_OPENVR_RUNTIME_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "libopenvr_api.so location at run time")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FCA_CAIRO QUIET IMPORTED_TARGET cairo)
  pkg_check_modules(FCA_VULKAN QUIET IMPORTED_TARGET vulkan)
  pkg_check_modules(FCA_FREETYPE QUIET IMPORTED_TARGET freetype2)
endif()
if(FCA_CAIRO_FOUND AND FCA_VULKAN_FOUND AND FCA_FREETYPE_FOUND AND EXISTS "${FCA_OPENVR_LIB_DIR}/libopenvr_api.so")
  add_executable(frame-camera-access-prompt
    native/prompt/main.cpp
    native/prompt/control_client.cpp
    native/prompt/doodle_assets.cpp
    native/prompt/doodle_style.cpp
    native/prompt/ui.cpp
    native/prompt/vr_prompt.cpp
    native/prompt/third_party/frameeyeosc/vk_texture.cpp)
  target_include_directories(frame-camera-access-prompt PRIVATE
    native/prompt native/prompt/third_party/frameeyeosc native/prompt/third_party/openvr)
  target_link_directories(frame-camera-access-prompt PRIVATE "${FCA_OPENVR_LIB_DIR}")
  target_link_libraries(frame-camera-access-prompt PRIVATE PkgConfig::FCA_CAIRO PkgConfig::FCA_FREETYPE PkgConfig::FCA_VULKAN
                        openvr_api)
  set_target_properties(frame-camera-access-prompt PROPERTIES BUILD_RPATH "${FCA_OPENVR_RUNTIME_DIR}"
                        INSTALL_RPATH "${FCA_OPENVR_RUNTIME_DIR}")
  target_compile_options(frame-camera-access-prompt PRIVATE -Wall -Wextra)
  set(FCA_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/native/prompt/assets")
  file(GLOB FCA_ASSETS CONFIGURE_DEPENDS "${FCA_ASSET_DIR}/*.ttf" "${FCA_ASSET_DIR}/*.png")
  set_source_files_properties(native/prompt/doodle_assets.cpp PROPERTIES
    COMPILE_DEFINITIONS "DOODLE_ASSET_DIR=\"${FCA_ASSET_DIR}\"" OBJECT_DEPENDS "${FCA_ASSETS}")
else()
  message(STATUS "frame-camera-access-prompt skipped (needs cairo, freetype2, vulkan and ${FCA_OPENVR_LIB_DIR}/libopenvr_api.so)")
endif()

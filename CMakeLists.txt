cmake_minimum_required(VERSION 3.20)

project(VHMD_DevKit VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT on MSVC so the driver DLL does not depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Only HmdDriverFactory is exported (Linux/macOS; MSVC hides by default).
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VHMD_BUILD_TESTS "Build unit tests" ON)

# --- OpenVR SDK (headers only) ----------------------------------------------
# Pinned version. third_party/openvr is used when present (git submodule),
# otherwise the tag is fetched at configure time.
set(VHMD_OPENVR_TAG "v2.15.6" CACHE STRING "OpenVR SDK tag")
set(VHMD_OPENVR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr" CACHE PATH "OpenVR SDK checkout")
# Linux only, used when no system vulkan/vulkan.h is found.
set(VHMD_VULKAN_HEADERS_TAG "v1.3.296" CACHE STRING "Vulkan-Headers tag")
if(NOT EXISTS "${VHMD_OPENVR_DIR}/headers/openvr_driver.h")
  include(FetchContent)
  message(STATUS "OpenVR not found in ${VHMD_OPENVR_DIR}; fetching ${VHMD_OPENVR_TAG}")
  FetchContent_Declare(openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG ${VHMD_OPENVR_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-build) # headers only: never add_subdirectory() it
  FetchContent_MakeAvailable(openvr)
  set(VHMD_OPENVR_DIR "${openvr_SOURCE_DIR}")
endif()
message(STATUS "OpenVR SDK: ${VHMD_OPENVR_DIR}")

# --- Platform folder name used by SteamVR -----------------------------------
if(WIN32)
  set(VHMD_PLATFORM_DIR "win64")
elseif(APPLE)
  set(VHMD_PLATFORM_DIR "osx64")
else()
  set(VHMD_PLATFORM_DIR "linux64")
endif()

# Staged driver folder (what vrpathreg registers): build/driver/vhmd
set(VHMD_STAGE_DIR "${CMAKE_BINARY_DIR}/driver/vhmd" CACHE PATH "Staged SteamVR driver folder")

if(MSVC)
  set(VHMD_WARNINGS /W4 /permissive- /utf-8 /external:W0)
else()
  set(VHMD_WARNINGS -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(driver)

if(VHMD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

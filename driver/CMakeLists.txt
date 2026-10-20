# --- Core library: no OpenVR dependency, unit tested -------------------------
add_library(vhmd_core STATIC
  src/math/pose_math.cpp
  src/protocol/parser.cpp
  src/protocol/osc.cpp
  src/config/roles.cpp
  src/config/driver_config.cpp
  src/state/device_state.cpp
  src/state/router.cpp
  src/net/udp_socket.cpp
  src/net/udp_receiver.cpp
  src/util/log.cpp
  src/util/precise_sleep.cpp
  src/util/thread_cpu.cpp
  src/visuals/device_visuals.cpp
)
# Driver version reported in status / logs: the project version from the top-level CMakeLists.txt.
target_compile_definitions(vhmd_core PUBLIC VHMD_VERSION="${PROJECT_VERSION}")
target_include_directories(vhmd_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(vhmd_core SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party) # <nlohmann/json.hpp>
target_compile_options(vhmd_core PRIVATE ${VHMD_WARNINGS})
if(WIN32)
  target_link_libraries(vhmd_core PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(vhmd_core PUBLIC Threads::Threads)
endif()

# --- Hand pose generator from the OpenVR samples (compiled as-is) -----------
set(VHMD_HANDSIM_DIR ${VHMD_OPENVR_DIR}/samples/drivers/drivers/handskeletonsimulation/src)
add_library(vhmd_handsim STATIC ${VHMD_HANDSIM_DIR}/hand_simulation.cpp)
target_include_directories(vhmd_handsim SYSTEM PUBLIC
  ${VHMD_HANDSIM_DIR}
  ${VHMD_OPENVR_DIR}/headers
  ${VHMD_OPENVR_DIR}/samples/drivers/utils/vrmath)

# --- The SteamVR driver ------------------------------------------------------
add_library(driver_vhmd SHARED
  src/main.cpp
  src/device_provider.cpp
  src/device_base.cpp
  src/hmd_device.cpp
  src/controller_device.cpp
  src/tracker_device.cpp
)
target_include_directories(driver_vhmd SYSTEM PRIVATE ${VHMD_OPENVR_DIR}/headers)
target_link_libraries(driver_vhmd PRIVATE vhmd_core vhmd_handsim)
target_compile_options(driver_vhmd PRIVATE ${VHMD_WARNINGS})
set_target_properties(driver_vhmd PROPERTIES
  PREFIX ""  # driver_vhmd.dll / driver_vhmd.so, as SteamVR expects
  # $<1:...> stops multi-config generators from appending /Release etc.
  RUNTIME_OUTPUT_DIRECTORY "$<1:${VHMD_STAGE_DIR}/bin/${VHMD_PLATFORM_DIR}>"
  LIBRARY_OUTPUT_DIRECTORY "$<1:${VHMD_STAGE_DIR}/bin/${VHMD_PLATFORM_DIR}>"
  PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>")

if(WIN32)
  # Direct-mode HMD output on Windows (display.mode = "direct"): D3D11 shared
  # textures, optional preview window, shared-memory frames.
  target_sources(driver_vhmd PRIVATE src/windows/direct_mode.cpp src/windows/stream_encoder.cpp)
  # stream_encoder: hardware video encoding for VHMD Link (Media Foundation).
  target_link_libraries(driver_vhmd PRIVATE d3d11 dxgi mfplat mfuuid ole32)
endif()

if(UNIX AND NOT APPLE)
  # Direct-mode HMD output (required by Linux SteamVR) + shared-memory preview.
  # Only Vulkan *headers* are needed: libvulkan is dlopen()ed at runtime.
  target_sources(driver_vhmd PRIVATE
    src/linux/direct_mode.cpp
    src/linux/frame_capture.cpp
    src/linux/vk_loader.cpp)
  find_path(VHMD_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
  if(NOT VHMD_VULKAN_INCLUDE_DIR)
    include(FetchContent)
    message(STATUS "Vulkan headers not found; fetching KhronosGroup/Vulkan-Headers ${VHMD_VULKAN_HEADERS_TAG}")
    FetchContent_Declare(vulkan_headers
      GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
      GIT_TAG ${VHMD_VULKAN_HEADERS_TAG}
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(vulkan_headers)
    set(VHMD_VULKAN_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include" CACHE PATH "Vulkan headers" FORCE)
  endif()
  message(STATUS "Vulkan headers: ${VHMD_VULKAN_INCLUDE_DIR}")
  target_include_directories(driver_vhmd SYSTEM PRIVATE ${VHMD_VULKAN_INCLUDE_DIR})
  target_link_libraries(driver_vhmd PRIVATE ${CMAKE_DL_LIBS})

  # vrserver loads many drivers into one process: export nothing but the
  # entry point, and link libstdc++/libgcc statically so the driver does not
  # depend on the (possibly older) libstdc++ of the SteamVR runtime.
  target_link_options(driver_vhmd PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/driver_exports.map
    -Wl,--no-undefined
    -static-libstdc++ -static-libgcc)
  set_property(TARGET driver_vhmd APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/driver_exports.map)
endif()

# Stage manifest and resources next to the binary.
add_custom_command(TARGET driver_vhmd PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VHMD_STAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest ${VHMD_STAGE_DIR}/driver.vrdrivermanifest
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/resources ${VHMD_STAGE_DIR}/resources
  COMMENT "Staging SteamVR driver folder in ${VHMD_STAGE_DIR}")

# cmake --install build --prefix <dir>  ->  <dir>/vhmd/...
install(TARGETS driver_vhmd
  RUNTIME DESTINATION vhmd/bin/${VHMD_PLATFORM_DIR}
  LIBRARY DESTINATION vhmd/bin/${VHMD_PLATFORM_DIR})
install(FILES driver.vrdrivermanifest DESTINATION vhmd)
install(DIRECTORY resources DESTINATION vhmd)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md DESTINATION vhmd OPTIONAL)

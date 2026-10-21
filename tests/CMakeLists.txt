add_executable(vhmd_unit_tests
  unit/test_main.cpp
  unit/test_math.cpp
  unit/test_parser.cpp
  unit/test_osc.cpp
  unit/test_config_state.cpp
  unit/test_visuals.cpp
  unit/test_udp_video.cpp
)
target_link_libraries(vhmd_unit_tests PRIVATE vhmd_core)
target_compile_options(vhmd_unit_tests PRIVATE ${VHMD_WARNINGS})
target_compile_definitions(vhmd_unit_tests PRIVATE VHMD_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND vhmd_unit_tests)

if(WIN32)
  # Hardware video encoding (D3D11 video processor + Media Foundation) without
  # SteamVR: encodes a moving test pattern and checks what a client receives.
  # Skipped (exit 77) when the machine has no hardware H.264 encoder.
  add_executable(vhmd_stream_selftest
    windows/stream_selftest.cpp
    ${PROJECT_SOURCE_DIR}/driver/src/windows/stream_encoder.cpp
    ${PROJECT_SOURCE_DIR}/driver/src/windows/amf_encoder.cpp)
  target_link_libraries(vhmd_stream_selftest PRIVATE vhmd_core d3d11 dxgi mfplat mfuuid ole32)
  add_test(NAME stream_selftest COMMAND vhmd_stream_selftest)
  set_tests_properties(stream_selftest PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(UNIX AND NOT APPLE)
  # Direct-mode frame capture against a real Vulkan device (no SteamVR).
  # Skipped (exit 77) when no Vulkan driver is available.
  add_executable(vhmd_capture_selftest
    linux/capture_selftest.cpp
    ${PROJECT_SOURCE_DIR}/driver/src/linux/frame_capture.cpp
    ${PROJECT_SOURCE_DIR}/driver/src/linux/vk_loader.cpp)
  target_include_directories(vhmd_capture_selftest SYSTEM PRIVATE ${VHMD_OPENVR_DIR}/headers ${VHMD_VULKAN_INCLUDE_DIR})
  target_link_libraries(vhmd_capture_selftest PRIVATE vhmd_core ${CMAKE_DL_LIBS})
  target_compile_options(vhmd_capture_selftest PRIVATE ${VHMD_WARNINGS})
  add_test(NAME capture_selftest COMMAND vhmd_capture_selftest)
  set_tests_properties(capture_selftest PROPERTIES SKIP_RETURN_CODE 77)
endif()

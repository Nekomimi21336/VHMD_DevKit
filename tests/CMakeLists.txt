add_executable(vhmd_unit_tests
  unit/test_main.cpp
  unit/test_math.cpp
  unit/test_parser.cpp
  unit/test_osc.cpp
  unit/test_config_state.cpp
)
target_link_libraries(vhmd_unit_tests PRIVATE vhmd_core)
target_compile_options(vhmd_unit_tests PRIVATE ${VHMD_WARNINGS})
target_compile_definitions(vhmd_unit_tests PRIVATE VHMD_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND vhmd_unit_tests)

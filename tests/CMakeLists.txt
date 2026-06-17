add_executable(salt_tests
  test_main.c
)
target_link_libraries(salt_tests PRIVATE salt_core)
add_test(NAME salt_tests COMMAND salt_tests)

add_test(
  NAME cli_smoke
  COMMAND ${CMAKE_COMMAND}
    -DSALT_BIN=$<TARGET_FILE:salt>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli_smoke
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake
)

add_executable(salt_tests
  test_main.c
)
target_link_libraries(salt_tests PRIVATE halite)
add_test(NAME salt_tests COMMAND salt_tests)

add_test(
  NAME cli_smoke
  COMMAND ${CMAKE_COMMAND}
    -DSALT_BIN=$<TARGET_FILE:salt>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli_smoke
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake
)

add_test(
  NAME ota_smoke
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/ota_smoke.sh
    $<TARGET_FILE:salt>
    ${CMAKE_SOURCE_DIR}/os/ota/salt-ota.sh
    ${CMAKE_CURRENT_BINARY_DIR}/ota_smoke
)

add_test(
  NAME pi_tryboot
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pi_tryboot.sh
    ${CMAKE_SOURCE_DIR}/os/pi/ab-update.sh
    ${CMAKE_CURRENT_BINARY_DIR}/pi_tryboot
)

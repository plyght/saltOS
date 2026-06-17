add_executable(salt
  main.cpp
  cli.cpp
  cmd_query.cpp
  cmd_install.cpp
  cmd_repo.cpp
  cmd_build.cpp
  cmd_trust.cpp
)

target_link_libraries(salt PRIVATE salt_core)
target_compile_features(salt PRIVATE cxx_std_23)

install(TARGETS salt RUNTIME DESTINATION bin)

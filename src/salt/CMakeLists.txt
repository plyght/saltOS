add_executable(salt
  main.cpp
  cli.cpp
  cmd_query.cpp
  cmd_install.cpp
  cmd_repo.cpp
  cmd_build.cpp
  cmd_trust.cpp
  cmd_stratum.cpp
  cmd_run.cpp
  cmd_strata_sugar.cpp
  cmd_expose.cpp
  cmd_provider.cpp
  cmd_service.cpp
)

target_link_libraries(salt PRIVATE halite)
target_compile_features(salt PRIVATE cxx_std_23)

install(TARGETS salt RUNTIME DESTINATION bin)

add_executable(salt-setup
  main.cpp
)

target_link_libraries(salt-setup PRIVATE halite)
target_compile_features(salt-setup PRIVATE cxx_std_23)

install(TARGETS salt-setup RUNTIME DESTINATION bin)

add_library(saltsetup STATIC
  config.cpp
)
target_include_directories(saltsetup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(saltsetup PUBLIC halite)
target_compile_features(saltsetup PUBLIC cxx_std_23)

add_executable(salt-setup
  main.cpp
)

target_link_libraries(salt-setup PRIVATE saltsetup halite)
target_compile_features(salt-setup PRIVATE cxx_std_23)

install(TARGETS salt-setup RUNTIME DESTINATION bin)

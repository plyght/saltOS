cmake_minimum_required(VERSION 3.20)
project(saltOS LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)
pkg_check_modules(SODIUM REQUIRED libsodium)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_subdirectory(src/libsalt)
add_subdirectory(src/salt)

enable_testing()
add_subdirectory(tests)

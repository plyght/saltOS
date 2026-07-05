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

set(SALT_DEP_INCLUDE_DIRS ${ZSTD_INCLUDE_DIRS} ${SODIUM_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(ZSTD_STATIC_A NAMES libzstd.a HINTS ${ZSTD_LIBRARY_DIRS})
  find_library(SODIUM_STATIC_A NAMES libsodium.a HINTS ${SODIUM_LIBRARY_DIRS})
  find_library(SQLITE3_STATIC_A NAMES libsqlite3.a HINTS ${SQLITE3_LIBRARY_DIRS})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ZSTD_STATIC_A AND SODIUM_STATIC_A AND SQLITE3_STATIC_A)
  set(SALT_DEP_LIBRARY_DIRS "")
  set(SALT_DEP_LIBS ${SODIUM_STATIC_A} ${ZSTD_STATIC_A} ${SQLITE3_STATIC_A} pthread dl m)
  message(STATUS "saltOS: static-linking libsodium/libzstd/sqlite3 (portable binary)")
else()
  set(SALT_DEP_LIBRARY_DIRS ${ZSTD_LIBRARY_DIRS} ${SODIUM_LIBRARY_DIRS} ${SQLITE3_LIBRARY_DIRS})
  set(SALT_DEP_LIBS ${ZSTD_LIBRARIES} ${SODIUM_LIBRARIES} ${SQLITE3_LIBRARIES})
endif()

add_subdirectory(src/halite)
add_subdirectory(src/salt)
add_subdirectory(src/setup)

enable_testing()
add_subdirectory(tests)

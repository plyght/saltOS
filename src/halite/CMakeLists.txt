add_library(halite STATIC
  src/util.c
  src/hash.c
  src/sign.c
  src/zst.c
  src/toml.c
  src/tar.c
  src/pkg.c
  src/archive.c
  src/db.c
  src/repo.c
  src/txn.c
  src/trust.c
  src/stratum.c
  src/run.c
)

target_include_directories(halite PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(halite PRIVATE
  ${ZSTD_INCLUDE_DIRS}
  ${SODIUM_INCLUDE_DIRS}
  ${SQLITE3_INCLUDE_DIRS}
)

target_link_directories(halite PUBLIC
  ${ZSTD_LIBRARY_DIRS}
  ${SODIUM_LIBRARY_DIRS}
  ${SQLITE3_LIBRARY_DIRS}
)

target_link_libraries(halite PUBLIC
  ${ZSTD_LIBRARIES}
  ${SODIUM_LIBRARIES}
  ${SQLITE3_LIBRARIES}
)

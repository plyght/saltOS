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
  ${SALT_DEP_INCLUDE_DIRS}
)

target_link_directories(halite PUBLIC
  ${SALT_DEP_LIBRARY_DIRS}
)

target_link_libraries(halite PUBLIC
  ${SALT_DEP_LIBS}
)

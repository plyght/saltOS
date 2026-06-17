if(NOT SALT_BIN OR NOT WORKDIR)
  message(FATAL_ERROR "SALT_BIN and WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(SRC "${WORKDIR}/src")
set(RECIPE "${WORKDIR}/recipes/hello")
set(OUT "${WORKDIR}/out")
set(KEYS "${WORKDIR}/keys")
set(ROOT "${WORKDIR}/root")
file(MAKE_DIRECTORY "${SRC}")
file(WRITE "${SRC}/.keep" "")
file(MAKE_DIRECTORY "${RECIPE}")

execute_process(COMMAND "${SALT_BIN}" --version OUTPUT_VARIABLE VER COMMAND_ERROR_IS_FATAL ANY)
string(REGEX MATCH "\\(([a-z0-9_]+)\\)" _m "${VER}")
set(ARCH "${CMAKE_MATCH_1}")
message(STATUS "smoke: arch=${ARCH}")

file(WRITE "${RECIPE}/recipe.toml"
"name = \"hello\"
version = \"1.0\"
release = 1
arch = [\"x86_64\", \"aarch64\"]
summary = \"smoke test package\"
license = \"MIT\"

[source]
url = \"file://${SRC}\"
sha256 = \"TODO-sha256\"

[build]
system = \"custom\"
script = \"\"\"
mkdir -p \"$SALT_DEST/usr/bin\"
printf '#!/bin/sh\\necho hi\\n' > \"$SALT_DEST/usr/bin/hello\"
chmod +x \"$SALT_DEST/usr/bin/hello\"
\"\"\"

[package]
deps = []

[reproducibility]
status = \"verified\"
")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env SALT_OUT=${OUT} SALT_WORK=${WORKDIR}/work
          "${SALT_BIN}" build "${RECIPE}"
  COMMAND_ERROR_IS_FATAL ANY)

if(NOT EXISTS "${OUT}/${ARCH}/packages/hello-1.0-1-${ARCH}.saltpkg")
  message(FATAL_ERROR "build did not produce the expected .saltpkg")
endif()

execute_process(COMMAND "${SALT_BIN}" keygen "${KEYS}" repo COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${SALT_BIN}" --key "${KEYS}/repo.sec" repo publish "${OUT}/${ARCH}"
  COMMAND_ERROR_IS_FATAL ANY)

if(NOT EXISTS "${OUT}/${ARCH}/index.toml.sig")
  message(FATAL_ERROR "publish did not sign the index")
endif()

file(READ "${KEYS}/repo.pub" PUBKEY)
string(STRIP "${PUBKEY}" PUBKEY)
file(MAKE_DIRECTORY "${ROOT}/etc/salt")
file(WRITE "${ROOT}/etc/salt/repo.conf"
"repo = \"current\"
source = \"file://${OUT}\"
key = \"${PUBKEY}\"
")

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" sync COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install hello
                COMMAND_ERROR_IS_FATAL ANY)

if(NOT EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "install did not place the file")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" query hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" files hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" owner /usr/bin/hello
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" verify hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" lint "${RECIPE}" COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello
                COMMAND_ERROR_IS_FATAL ANY)
if(EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "remove did not delete the file")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" rollback COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "rollback did not restore the removed file")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" deployments COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "smoke: all steps passed")

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

function(write_recipe dir name version deps conflicts)
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/recipe.toml"
"name = \"${name}\"
version = \"${version}\"
release = 1
arch = [\"x86_64\", \"aarch64\"]
summary = \"smoke test package ${name}\"
license = \"MIT\"

[source]
url = \"file://${SRC}\"
sha256 = \"TODO-sha256\"

[build]
system = \"custom\"
script = \"\"\"
mkdir -p \"$SALT_DEST/usr/bin\"
printf '#!/bin/sh\\necho ${name} ${version}\\n' > \"$SALT_DEST/usr/bin/${name}\"
chmod +x \"$SALT_DEST/usr/bin/${name}\"
\"\"\"

[package]
deps = [${deps}]
conflicts = [${conflicts}]

[reproducibility]
status = \"verified\"
")
endfunction()

function(build_recipe dir)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env SALT_OUT=${OUT} SALT_WORK=${WORKDIR}/work
            "${SALT_BIN}" build "${dir}"
    COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function(expect_fail what)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${what}: expected a non-zero exit, got 0")
  endif()
  message(STATUS "smoke: ${what} -> exit ${rc} (expected)")
endfunction()

function(expect_fail_output what needle)
  execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${what}: expected a non-zero exit, got 0")
  endif()
  if(NOT "${out}${err}" MATCHES "${needle}")
    message(FATAL_ERROR "${what}: output lacks '${needle}':\n${out}${err}")
  endif()
endfunction()

function(expect_output what needle)
  execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${what}: exit ${rc}\n${out}${err}")
  endif()
  if(NOT "${out}${err}" MATCHES "${needle}")
    message(FATAL_ERROR "${what}: output lacks '${needle}':\n${out}${err}")
  endif()
endfunction()

function(publish)
  execute_process(
    COMMAND "${SALT_BIN}" --key "${KEYS}/repo.sec" repo publish "${OUT}/${ARCH}"
    COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function(resign_index)
  execute_process(COMMAND "${SALT_BIN}" --key "${KEYS}/repo.sec" sign "${OUT}/${ARCH}/index.toml"
                  COMMAND_ERROR_IS_FATAL ANY)
endfunction()

write_recipe("${RECIPE}" hello 1.0 "" "")
write_recipe("${WORKDIR}/recipes/libgreet" libgreet 1.0 "" "")
write_recipe("${WORKDIR}/recipes/greeter" greeter 1.0 "\"libgreet\"" "")
write_recipe("${WORKDIR}/recipes/rival" rival 1.0 "" "\"hello\"")
write_recipe("${WORKDIR}/recipes/orphan" orphan 1.0 "\"nowhere\"" "")
build_recipe("${RECIPE}")
build_recipe("${WORKDIR}/recipes/libgreet")
build_recipe("${WORKDIR}/recipes/greeter")
build_recipe("${WORKDIR}/recipes/rival")
build_recipe("${WORKDIR}/recipes/orphan")

if(NOT EXISTS "${OUT}/${ARCH}/packages/hello-1.0-1-${ARCH}.grain")
  message(FATAL_ERROR "build did not produce the expected .grain")
endif()

execute_process(COMMAND "${SALT_BIN}" keygen "${KEYS}" repo COMMAND_ERROR_IS_FATAL ANY)
publish()

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
expect_output("history" "install" "${SALT_BIN}" --root "${ROOT}" history)

expect_output("info shows sha256" "sha256      : [0-9a-f]+" "${SALT_BIN}" --root "${ROOT}" info hello)
expect_output("info for available package" "available" "${SALT_BIN}" --root "${ROOT}" info greeter)
expect_fail("info for unknown package" "${SALT_BIN}" --root "${ROOT}" info no-such-package)
expect_output("search by description" "greeter" "${SALT_BIN}" --root "${ROOT}" search "package greeter")
expect_fail("search with no match" "${SALT_BIN}" --root "${ROOT}" search zzz-no-such-thing)
expect_output("list --installed" "hello" "${SALT_BIN}" --root "${ROOT}" list --installed)
expect_output("list --upgradable" "0 packages upgradable" "${SALT_BIN}" --root "${ROOT}" list --upgradable)
expect_output("list --available" "rival" "${SALT_BIN}" --root "${ROOT}" list --available)
expect_fail("list with bad flag" "${SALT_BIN}" --root "${ROOT}" list --bogus)
expect_fail("unknown option" "${SALT_BIN}" --root "${ROOT}" install --frobnicate hello)
expect_fail("install of unknown package" "${SALT_BIN}" --root "${ROOT}" --yes install no-such-package)
expect_fail("install with missing dependency" "${SALT_BIN}" --root "${ROOT}" --yes install orphan)
expect_fail("install of a conflicting package" "${SALT_BIN}" --root "${ROOT}" --yes install rival)
if(EXISTS "${ROOT}/usr/bin/rival")
  message(FATAL_ERROR "conflicting package was installed")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install greeter COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/libgreet" OR NOT EXISTS "${ROOT}/usr/bin/greeter")
  message(FATAL_ERROR "install did not pull in the dependency")
endif()
expect_fail("remove of a required package" "${SALT_BIN}" --root "${ROOT}" --yes remove libgreet)
if(NOT EXISTS "${ROOT}/usr/bin/libgreet")
  message(FATAL_ERROR "refused remove still deleted the file")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove --cascade libgreet COMMAND_ERROR_IS_FATAL ANY)
if(EXISTS "${ROOT}/usr/bin/libgreet" OR EXISTS "${ROOT}/usr/bin/greeter")
  message(FATAL_ERROR "cascade remove left files behind")
endif()
expect_fail("query of cascaded package" "${SALT_BIN}" --root "${ROOT}" files greeter)

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" lock COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/etc/salt/system.lock.toml")
  message(FATAL_ERROR "lock did not write the lockfile")
endif()
file(READ "${ROOT}/etc/salt/system.lock.toml" LOCK)
if(NOT LOCK MATCHES "name = \"hello\"" OR NOT LOCK MATCHES "grain_sha256 = \"sha256:[0-9a-f]+\"")
  message(FATAL_ERROR "lockfile does not pin hello with a sha256:\n${LOCK}")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" lock diff COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install greeter COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
expect_fail("lock diff after drift" "${SALT_BIN}" --root "${ROOT}" lock diff)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" lock diff OUTPUT_VARIABLE DIFF RESULT_VARIABLE rc)
if(NOT DIFF MATCHES "\\+ hello" OR NOT DIFF MATCHES "- greeter" OR NOT DIFF MATCHES "- libgreet")
  message(FATAL_ERROR "lock diff did not report the drift:\n${DIFF}")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes lock apply --dry-run COMMAND_ERROR_IS_FATAL ANY)
if(EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "lock apply --dry-run changed the system")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes lock apply COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/hello" OR EXISTS "${ROOT}/usr/bin/greeter" OR EXISTS "${ROOT}/usr/bin/libgreet")
  message(FATAL_ERROR "lock apply did not converge to the locked set")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" lock diff COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" config diff COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${ROOT}/etc/salt/system.toml" "[system]\nhostname = \"smoke\"\n")
expect_fail_output("config apply refuses a stale lock" "--relock" "${SALT_BIN}" --root "${ROOT}" --yes config apply)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply COMMAND_ERROR_IS_FATAL ANY)
file(READ "${ROOT}/etc/salt/system.lock.toml" RELOCKED)
if(NOT RELOCKED MATCHES "config_hash = \"sha256:")
  message(FATAL_ERROR "--relock did not record the config hash")
endif()

file(WRITE "${ROOT}/etc/salt/system.toml"
"schema = 1
[system]
hostname = \"smoke\"
[native]
repo = \"current\"
packages = [\"greeter\"]
[native.pin]
greeter = \"1.0-1\"
[policy]
require_signed_native = true
on_missing_artifact = \"fail\"
")
expect_output("config check" "ok" "${SALT_BIN}" --root "${ROOT}" config check)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock --dry-run
                OUTPUT_VARIABLE CFGDRY COMMAND_ERROR_IS_FATAL ANY)
if(NOT CFGDRY MATCHES "\\+ greeter" OR NOT CFGDRY MATCHES "\\+ libgreet" OR NOT CFGDRY MATCHES "- hello")
  message(FATAL_ERROR "config apply --dry-run did not plan the declared native set:\n${CFGDRY}")
endif()
if(EXISTS "${ROOT}/usr/bin/greeter" OR NOT EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "config apply --dry-run changed the system")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/greeter" OR NOT EXISTS "${ROOT}/usr/bin/libgreet" OR EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "config apply did not converge to the declared native set")
endif()
file(READ "${ROOT}/etc/salt/system.lock.toml" CFGLOCK)
if(NOT CFGLOCK MATCHES "name = \"greeter\"" OR NOT CFGLOCK MATCHES "name = \"libgreet\"" OR CFGLOCK MATCHES "name = \"hello\"")
  message(FATAL_ERROR "config apply did not relock the converged set:\n${CFGLOCK}")
endif()
expect_output("config apply is idempotent" "already matches" "${SALT_BIN}" --root "${ROOT}" --yes config apply)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" config diff COMMAND_ERROR_IS_FATAL ANY)

file(RENAME "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml.sig" "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml.sig.off")
expect_fail_output("policy require_signed_native refuses an unsigned index" "require_signed_native"
                   "${SALT_BIN}" --root "${ROOT}" --yes config apply)
file(RENAME "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml.sig.off" "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml.sig")

file(WRITE "${ROOT}/etc/salt/system.toml"
"[native]
packages = [\"greeter\"]
[native.pin]
greeter = \"9.9\"
")
expect_fail_output("config apply refuses an unavailable pin" "does not offer"
                   "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock)
file(WRITE "${ROOT}/etc/salt/system.toml"
"[native]
packages = [\"greeter\", \"no-such-package\"]
")
expect_fail_output("config apply fails on a missing root by default" "not in the repository index"
                   "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock)
file(WRITE "${ROOT}/etc/salt/system.toml"
"[native]
packages = [\"greeter\", \"no-such-package\"]
[policy]
on_missing_artifact = \"skip\"
")
expect_output("policy on_missing_artifact = skip" "skipping: native package no-such-package"
              "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock)
file(WRITE "${ROOT}/etc/salt/system.toml" "[native]\npackages = [\"greeter\"]\n[policy]\non_missing_artifact = \"ignore\"\n")
expect_fail_output("config rejects an unknown policy value" "on_missing_artifact"
                   "${SALT_BIN}" --root "${ROOT}" config check)
file(WRITE "${ROOT}/etc/salt/system.toml" "[native]\npackages = [\"greeter\"]\n[policy]\nfrobnicate = true\n")
expect_fail_output("config rejects an unknown policy key" "unknown key"
                   "${SALT_BIN}" --root "${ROOT}" config check)
file(WRITE "${ROOT}/etc/salt/system.toml" "[native]\npackages = [\"greeter\"]\n[native.pin]\nhello = \"1.0\"\n")
expect_fail_output("config rejects a pin outside the native set" "not part of the declared native set"
                   "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock)
file(WRITE "${ROOT}/etc/salt/system.toml" "[native]\npackages = [\"greeter\"]\n[expose]\n\"nowhere/rg\" = \"rg\"\n")
expect_fail_output("config apply reports an unknown expose stratum" "unknown stratum nowhere"
                   "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock)
file(WRITE "${ROOT}/etc/salt/system.toml" "[native]\npackages = [\"greeter\"]\n[expose]\nrg = \"rg\"\n")
expect_fail_output("config rejects a malformed expose key" "stratum/command"
                   "${SALT_BIN}" --root "${ROOT}" config check)

file(WRITE "${ROOT}/etc/salt/system.toml" "[system]\nhostname = \"smoke\"\n[native]\npackages = [\"hello\"]\n")
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes config apply --relock COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/hello" OR EXISTS "${ROOT}/usr/bin/greeter" OR EXISTS "${ROOT}/usr/bin/libgreet")
  message(FATAL_ERROR "config apply did not swap the native set back to hello")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install --locked COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "install --locked did not restore the locked set")
endif()

file(READ "${ROOT}/etc/salt/system.lock.toml" LOCK)
string(REGEX REPLACE "grain_sha256 = \"sha256:[0-9a-f]+\""
       "grain_sha256 = \"sha256:0000000000000000000000000000000000000000000000000000000000000000\""
       BADLOCK "${LOCK}")
file(WRITE "${WORKDIR}/bad.lock.toml" "${BADLOCK}")
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
expect_fail("lock apply with mismatched hash" "${SALT_BIN}" --root "${ROOT}" --yes lock apply "${WORKDIR}/bad.lock.toml")
if(EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "lock apply installed a package whose hash did not match the lock")
endif()
string(REGEX REPLACE "grain_sha256 = \"sha256:[0-9a-f]+\"\n" "" NOHASHLOCK "${LOCK}")
file(WRITE "${WORKDIR}/nohash.lock.toml" "${NOHASHLOCK}")
expect_fail("lock apply with hashless lock" "${SALT_BIN}" --root "${ROOT}" --yes lock apply "${WORKDIR}/nohash.lock.toml")
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install hello COMMAND_ERROR_IS_FATAL ANY)

file(WRITE "${WORKDIR}/stratum.lock.toml"
  "${LOCK}\n[[stratum]]\nname = \"ghost\"\nfamily = \"alpine\"\npackage_manager = \"apk\"\n\n[[stratum.package]]\nname = \"musl\"\nversion = \"1.2.5-r0\"\n")
expect_fail_output("lock diff reports an unbootstrapped stratum" "ghost .*not bootstrapped"
  "${SALT_BIN}" --root "${ROOT}" lock diff "${WORKDIR}/stratum.lock.toml")
file(WRITE "${WORKDIR}/unversioned.lock.toml"
  "${LOCK}\n[[stratum]]\nname = \"ghost\"\n\n[[stratum.package]]\nname = \"musl\"\n")
expect_fail_output("lock apply refuses an unversioned stratum package" "musl has no version"
  "${SALT_BIN}" --root "${ROOT}" --yes lock apply "${WORKDIR}/unversioned.lock.toml")
expect_fail_output("lock diff refuses an unversioned stratum package" "musl has no version"
  "${SALT_BIN}" --root "${ROOT}" lock diff "${WORKDIR}/unversioned.lock.toml")
file(WRITE "${WORKDIR}/dupstratum.lock.toml"
  "${LOCK}\n[[stratum]]\nname = \"ghost\"\n\n[[stratum.package]]\nname = \"musl\"\nversion = \"1\"\n\n[[stratum.package]]\nname = \"musl\"\nversion = \"2\"\n")
expect_fail_output("lock apply refuses a twice-pinned stratum package" "musl is pinned twice"
  "${SALT_BIN}" --root "${ROOT}" --yes lock apply "${WORKDIR}/dupstratum.lock.toml")

file(READ "${OUT}/${ARCH}/index.toml" INDEX)
string(REGEX REPLACE "(name = \"hello\"[^[]*sha256 = )\"[0-9a-f]+\"" "\\1\"TODO-sha256\"" BADINDEX "${INDEX}")
file(WRITE "${OUT}/${ARCH}/index.toml" "${BADINDEX}")
resign_index()
expect_fail("sync rejects a placeholder sha256" "${SALT_BIN}" --root "${ROOT}" sync)
string(REGEX REPLACE "(name = \"hello\"[^[]*)sha256 = \"[0-9a-f]+\"\n" "\\1" NOHASHINDEX "${INDEX}")
file(WRITE "${OUT}/${ARCH}/index.toml" "${NOHASHINDEX}")
resign_index()
expect_fail("sync rejects a missing sha256" "${SALT_BIN}" --root "${ROOT}" sync)
file(WRITE "${OUT}/${ARCH}/index.toml" "${INDEX}")
resign_index()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" sync COMMAND_ERROR_IS_FATAL ANY)
file(READ "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml" SYNCED)
if(NOT SYNCED MATCHES "name = \"hello\"")
  message(FATAL_ERROR "a rejected sync clobbered the local index")
endif()

execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml" "${NOHASHINDEX}")
expect_fail("install refuses an unverifiable package" "${SALT_BIN}" --root "${ROOT}" --yes install hello)
if(EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "unverifiable package was installed without --allow-unverified")
endif()
expect_output("install --allow-unverified warns" "WARNING" "${SALT_BIN}" --root "${ROOT}" --yes install --allow-unverified hello)
if(NOT EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "--allow-unverified did not install the package")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" sync COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
string(REGEX REPLACE "(name = \"hello\"[^[]*sha256 = )\"[0-9a-f]+\""
       "\\1\"0000000000000000000000000000000000000000000000000000000000000000\"" WRONGINDEX "${INDEX}")
file(WRITE "${ROOT}/var/lib/salt/repo/${ARCH}/index.toml" "${WRONGINDEX}")
expect_fail("install refuses an artifact whose sha256 differs from the index" "${SALT_BIN}" --root "${ROOT}" --yes install hello)
if(EXISTS "${ROOT}/usr/bin/hello")
  message(FATAL_ERROR "artifact with wrong sha256 was installed")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" sync COMMAND_ERROR_IS_FATAL ANY)

file(REMOVE "${ROOT}/usr/bin")
file(REMOVE_RECURSE "${ROOT}/usr/bin")
file(WRITE "${ROOT}/usr/bin" "not a directory")
expect_fail("install fails when extraction is impossible" "${SALT_BIN}" --root "${ROOT}" --yes install hello)
expect_fail("failed install left no db record" "${SALT_BIN}" --root "${ROOT}" files hello)
expect_output("failed transaction is recorded" "failed" "${SALT_BIN}" --root "${ROOT}" history)
file(REMOVE "${ROOT}/usr/bin")
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" verify hello COMMAND_ERROR_IS_FATAL ANY)
file(APPEND "${ROOT}/usr/bin/hello" "tampered")
expect_fail_output("verify detects modification" "MODIFIED" "${SALT_BIN}" --root "${ROOT}" verify hello)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes remove hello COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes install hello COMMAND_ERROR_IS_FATAL ANY)

write_recipe("${WORKDIR}/recipes/hello2" hello 2.0 "" "")
build_recipe("${WORKDIR}/recipes/hello2")
publish()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" sync COMMAND_ERROR_IS_FATAL ANY)
expect_output("list --upgradable sees the new version" "hello +1.0-1 -> 2.0-1" "${SALT_BIN}" --root "${ROOT}" list --upgradable)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes update --download-only COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ROOT}/var/lib/salt/cache/${ARCH}/hello-2.0-1-${ARCH}.grain")
  message(FATAL_ERROR "update --download-only did not fetch the artifact")
endif()
expect_output("download-only leaves the old version installed" "version     : 1.0-1" "${SALT_BIN}" --root "${ROOT}" info hello)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" --yes update COMMAND_ERROR_IS_FATAL ANY)
expect_output("update installed the new version" "version     : 2.0-1" "${SALT_BIN}" --root "${ROOT}" info hello)
expect_output("hello 2.0 runs" "hello 2.0" sh "${ROOT}/usr/bin/hello")
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" rollback COMMAND_ERROR_IS_FATAL ANY)
expect_output("rollback restored 1.0" "version     : 1.0-1" "${SALT_BIN}" --root "${ROOT}" info hello)
expect_output("hello 1.0 runs after rollback" "hello 1.0" sh "${ROOT}/usr/bin/hello")

file(GLOB TXN_DIRS "${ROOT}/var/lib/salt/state/txn-*")
list(LENGTH TXN_DIRS NTXN_BEFORE)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" config gc --keep 2 --dry-run OUTPUT_VARIABLE GCDRY COMMAND_ERROR_IS_FATAL ANY)
file(GLOB TXN_DIRS "${ROOT}/var/lib/salt/state/txn-*")
list(LENGTH TXN_DIRS NTXN_DRY)
if(NOT NTXN_DRY EQUAL NTXN_BEFORE)
  message(FATAL_ERROR "gc --dry-run removed generations")
endif()
if(NOT GCDRY MATCHES "would prune generation")
  message(FATAL_ERROR "gc --dry-run did not report what it would prune:\n${GCDRY}")
endif()
expect_fail("gc rejects a bad keep count" "${SALT_BIN}" --root "${ROOT}" config gc --keep 0)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" config gc --keep 2 OUTPUT_VARIABLE GCOUT COMMAND_ERROR_IS_FATAL ANY)
file(GLOB TXN_DIRS "${ROOT}/var/lib/salt/state/txn-*")
list(LENGTH TXN_DIRS NTXN_AFTER)
if(NOT NTXN_AFTER LESS NTXN_BEFORE)
  message(FATAL_ERROR "gc did not prune any generation (${NTXN_BEFORE} -> ${NTXN_AFTER})")
endif()
if(NOT GCOUT MATCHES "freed: [0-9]+ generations")
  message(FATAL_ERROR "gc did not report what it freed:\n${GCOUT}")
endif()
if(NOT EXISTS "${ROOT}/var/lib/salt/cache/${ARCH}/hello-1.0-1-${ARCH}.grain")
  message(FATAL_ERROR "gc removed the artifact of the installed package")
endif()
expect_output("gc preserved the installed package" "version     : 1.0-1" "${SALT_BIN}" --root "${ROOT}" info hello)
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" rollback COMMAND_ERROR_IS_FATAL ANY)
expect_output("rollback still works after gc" "version     : 2.0-1" "${SALT_BIN}" --root "${ROOT}" info hello)

file(GLOB CACHED_BEFORE "${ROOT}/var/lib/salt/cache/${ARCH}/*.grain")
if(NOT CACHED_BEFORE)
  message(FATAL_ERROR "expected cached artifacts before clean")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" clean --dry-run COMMAND_ERROR_IS_FATAL ANY)
file(GLOB CACHED_DRY "${ROOT}/var/lib/salt/cache/${ARCH}/*.grain")
if(NOT CACHED_DRY STREQUAL CACHED_BEFORE)
  message(FATAL_ERROR "clean --dry-run removed cached artifacts")
endif()
execute_process(COMMAND "${SALT_BIN}" --root "${ROOT}" clean COMMAND_ERROR_IS_FATAL ANY)
file(GLOB CACHED "${ROOT}/var/lib/salt/cache/${ARCH}/*.grain")
if(CACHED)
  message(FATAL_ERROR "clean left artifacts in the cache: ${CACHED}")
endif()
expect_output("help lists lock apply" "lock apply" "${SALT_BIN}" --help)
expect_fail("unknown command" "${SALT_BIN}" --root "${ROOT}" frobnicate)
message(STATUS "smoke: all steps passed")

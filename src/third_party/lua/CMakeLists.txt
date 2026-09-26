# Lua 5.4.7 (https://www.lua.org/ftp/lua-5.4.7.tar.gz,
# sha256 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30),
# vendored unmodified minus the lua/luac front ends. salt embeds it to evaluate
# recipes and configuration in a sandbox (see src/halite/src/conf.c).
file(GLOB LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(saltlua STATIC ${LUA_SOURCES})
target_include_directories(saltlua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(saltlua PRIVATE LUA_USE_POSIX)
set_target_properties(saltlua PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_options(saltlua PRIVATE -w)
# lmathlib needs libm; the host build gets it via the static dependency list,
# the native (dynamically linked) build does not.
target_link_libraries(saltlua PUBLIC m)

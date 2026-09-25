#ifndef SALT_CONF_H
#define SALT_CONF_H

#include <stddef.h>
#include "salt/toml.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Human-authored configuration (recipes, strata, system.lua, salt.lua, ...) is
 * Lua: a file evaluates to one table, e.g.
 *
 *   return {
 *     name = "zlib",
 *     version = "1.3.1",
 *     build = { system = "autotools" },
 *   }
 *
 * The chunk runs in a sandbox: only the base functions without loaders, and
 * the string, table, math and utf8 libraries; no io, os, require, load or
 * dofile; text chunks only; bounded memory and instruction count. A read-only
 * global `salt` carries `salt.arch` (the target, $SALT_ARCH or the host) and
 * `salt.host_arch`. The returned table is converted into the same tree the
 * TOML reader produces, so every salt_toml_* accessor works on it: sequences
 * become arrays, string-keyed tables become tables (keys sorted), strings,
 * integers and booleans map directly; anything else is an error.
 *
 * Machine-written data (grain metadata and manifests, repository indexes,
 * lockfiles) stays TOML and is never evaluated. */

/* Evaluate Lua source; chunkname is used in error messages. */
salt_toml *salt_conf_eval(const char *text, size_t len, const char *chunkname);

/* Load a configuration file: *.lua is evaluated, anything else is read as
 * TOML data. Returns NULL and sets salt_last_error() on failure. */
salt_toml *salt_conf_load(const char *path);

/* As salt_conf_load, for text already in memory; name picks the format. */
salt_toml *salt_conf_parse(const char *text, size_t len, const char *name);

#ifdef __cplusplus
}
#endif

#endif

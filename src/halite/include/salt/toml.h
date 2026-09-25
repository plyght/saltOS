#ifndef SALT_TOML_H
#define SALT_TOML_H

#include <stddef.h>
#include <stdbool.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SALT_TOML_STRING,
  SALT_TOML_INT,
  SALT_TOML_BOOL,
  SALT_TOML_ARRAY,
  SALT_TOML_TABLE,
} salt_toml_type;

typedef struct salt_toml salt_toml;

salt_toml *salt_toml_parse(const char *text, size_t len);
salt_toml *salt_toml_parse_file(const char *path);
void salt_toml_free(salt_toml *t);

salt_toml_type salt_toml_typeof(const salt_toml *t);
const salt_toml *salt_toml_get(const salt_toml *table, const char *key);
const salt_toml *salt_toml_path(const salt_toml *table, const char *dotted_key);
size_t salt_toml_array_len(const salt_toml *array);
const salt_toml *salt_toml_array_at(const salt_toml *array, size_t i);
size_t salt_toml_table_len(const salt_toml *table);
const char *salt_toml_table_key(const salt_toml *table, size_t i);
const salt_toml *salt_toml_table_val(const salt_toml *table, size_t i);

/* Building a tree by hand (used by the Lua configuration loader). The put and
 * push calls take ownership of val; table_put rejects a duplicate key. */
salt_toml *salt_toml_new(salt_toml_type type);
salt_toml *salt_toml_new_string(const char *s, size_t len);
salt_toml *salt_toml_new_int(long long v);
salt_toml *salt_toml_new_bool(bool v);
int salt_toml_table_put(salt_toml *table, const char *key, salt_toml *val);
int salt_toml_array_push(salt_toml *array, salt_toml *val);

const char *salt_toml_as_string(const salt_toml *t);
long long salt_toml_as_int(const salt_toml *t, long long fallback);
bool salt_toml_as_bool(const salt_toml *t, bool fallback);

const char *salt_toml_string(const salt_toml *table, const char *dotted_key, const char *fallback);
long long salt_toml_int(const salt_toml *table, const char *dotted_key, long long fallback);
bool salt_toml_bool(const salt_toml *table, const char *dotted_key, bool fallback);
int salt_toml_string_array(const salt_toml *table, const char *dotted_key, salt_strlist *out);

#ifdef __cplusplus
}
#endif

#endif

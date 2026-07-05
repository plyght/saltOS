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

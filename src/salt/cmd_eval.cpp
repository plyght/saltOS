#include "cli.hpp"

#include "salt/conf.h"
#include "salt/util.h"

#include <cstdio>
#include <string>

namespace {

void print_scalar(const salt_toml *v) {
  switch (salt_toml_typeof(v)) {
    case SALT_TOML_STRING:
      fputs(salt_toml_as_string(v), stdout);
      break;
    case SALT_TOML_INT:
      printf("%lld", salt_toml_as_int(v, 0));
      break;
    case SALT_TOML_BOOL:
      fputs(salt_toml_as_bool(v, false) ? "true" : "false", stdout);
      break;
    default:
      break;
  }
}

bool is_scalar(const salt_toml *v) {
  salt_toml_type t = salt_toml_typeof(v);
  return t == SALT_TOML_STRING || t == SALT_TOML_INT || t == SALT_TOML_BOOL;
}

void dump(const salt_toml *v, const std::string &path) {
  if (is_scalar(v)) {
    printf("%s = ", path.c_str());
    print_scalar(v);
    fputc('\n', stdout);
  } else if (salt_toml_typeof(v) == SALT_TOML_ARRAY) {
    for (size_t i = 0; i < salt_toml_array_len(v); i++)
      dump(salt_toml_array_at(v, i), path + "[" + std::to_string(i + 1) + "]");
  } else {
    for (size_t i = 0; i < salt_toml_table_len(v); i++) {
      std::string key = salt_toml_table_key(v, i);
      dump(salt_toml_table_val(v, i), path.empty() ? key : path + "." + key);
    }
  }
}

}  // namespace

/* salt eval FILE [KEY]: evaluate a configuration file (Lua, or TOML data) and
 * print it for shell scripts. With KEY (dotted path; numeric segments index lists
 * from 1, e.g. stratum.1.name) a scalar prints as is, a
 * list prints one element per line and a table prints its keys; without KEY
 * every leaf prints as `path = value`. Exit 1 if the key is absent. */
int cmd_eval(const Options &o, const std::vector<std::string> &args) {
  (void)o;
  if (args.empty() || args.size() > 2) {
    fprintf(stderr, "usage: salt eval <file> [dotted.key]\n");
    return 2;
  }
  salt_toml *t = salt_conf_load(args[0].c_str());
  if (!t) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  int rc = 0;
  if (args.size() == 1) {
    dump(t, "");
  } else {
    const salt_toml *v = salt_toml_path(t, args[1].c_str());
    if (!v) {
      rc = 1;
    } else if (is_scalar(v)) {
      print_scalar(v);
      fputc('\n', stdout);
    } else if (salt_toml_typeof(v) == SALT_TOML_ARRAY) {
      /* scalars one per line; a list of tables prints its indexes (1..n) so
       * scripts can loop: for i in $(salt eval f list); do salt eval f list.$i.x */
      for (size_t i = 0; i < salt_toml_array_len(v); i++) {
        const salt_toml *e = salt_toml_array_at(v, i);
        if (is_scalar(e))
          print_scalar(e);
        else
          printf("%zu", i + 1);
        fputc('\n', stdout);
      }
    } else {
      for (size_t i = 0; i < salt_toml_table_len(v); i++) printf("%s\n", salt_toml_table_key(v, i));
    }
  }
  salt_toml_free(t);
  return rc;
}

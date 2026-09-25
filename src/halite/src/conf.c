#include "salt/conf.h"
#include "salt/util.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#define CONF_MEM_LIMIT (64u * 1024u * 1024u)
#define CONF_INSN_LIMIT 100000000L
#define CONF_HOOK_STEP 10000
#define CONF_MAX_DEPTH 64

typedef struct {
  size_t used;
  long insns;
} conf_state;

static void *conf_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  conf_state *st = ud;
  size_t old = ptr ? osize : 0;
  if (nsize == 0) {
    st->used -= old;
    free(ptr);
    return NULL;
  }
  if (nsize > old && st->used + (nsize - old) > CONF_MEM_LIMIT) return NULL;
  void *p = realloc(ptr, nsize);
  if (p) st->used = st->used - old + nsize;
  return p;
}

static void conf_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  conf_state *st;
  lua_getallocf(L, (void **)&st);
  st->insns += CONF_HOOK_STEP;
  if (st->insns > CONF_INSN_LIMIT) luaL_error(L, "configuration took too long to evaluate");
}

static int conf_print(lua_State *L) {
  int n = lua_gettop(L);
  for (int i = 1; i <= n; i++) {
    size_t len;
    const char *s = luaL_tolstring(L, i, &len);
    if (i > 1) fputc('\t', stderr);
    fwrite(s, 1, len, stderr);
    lua_pop(L, 1);
  }
  fputc('\n', stderr);
  return 0;
}

static const char *host_arch(void) {
  static struct utsname u;
  if (uname(&u) != 0) return "unknown";
  if (strcmp(u.machine, "arm64") == 0) return "aarch64";
  if (strcmp(u.machine, "amd64") == 0) return "x86_64";
  return u.machine;
}

/* Build the sandbox environment table on top of the stack. */
static void push_env(lua_State *L) {
  static const char *const base_ok[] = {
      "assert", "error",        "ipairs",       "next",     "pairs",    "pcall",    "rawequal",
      "rawget", "rawlen",       "rawset",       "select",   "tonumber", "tostring", "type",
      "xpcall", "setmetatable", "getmetatable", "_VERSION", NULL};
  static const char *const libs_ok[] = {"string", "table", "math", "utf8", NULL};

  lua_newtable(L);
  for (int i = 0; base_ok[i]; i++) {
    lua_getglobal(L, base_ok[i]);
    lua_setfield(L, -2, base_ok[i]);
  }
  for (int i = 0; libs_ok[i]; i++) {
    lua_getglobal(L, libs_ok[i]);
    lua_setfield(L, -2, libs_ok[i]);
  }
  lua_pushcfunction(L, conf_print);
  lua_setfield(L, -2, "print");

  const char *arch = getenv("SALT_ARCH");
  lua_newtable(L);
  lua_pushstring(L, (arch && *arch) ? arch : host_arch());
  lua_setfield(L, -2, "arch");
  lua_pushstring(L, host_arch());
  lua_setfield(L, -2, "host_arch");
  lua_setfield(L, -2, "salt");

  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_G");
}

static salt_toml *convert(lua_State *L, int idx, int depth, const char *where);

static int key_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static salt_toml *convert_table(lua_State *L, int idx, int depth, const char *where) {
  idx = lua_absindex(L, idx);
  lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
  size_t count = 0, strkeys = 0;
  bool ok = true;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    count++;
    if (lua_type(L, -2) == LUA_TSTRING) {
      strkeys++;
    } else if (lua_isinteger(L, -2)) {
      lua_Integer k = lua_tointeger(L, -2);
      if (k < 1 || k > n) ok = false;
    } else {
      ok = false;
    }
    lua_pop(L, 1);
  }
  if (!ok || (strkeys && strkeys != count)) {
    salt_set_error("%s: a table must be either a list or have only string keys", where);
    return NULL;
  }

  if (strkeys == 0) {
    salt_toml *arr = salt_toml_new(SALT_TOML_ARRAY);
    for (lua_Integer i = 1; arr && i <= n; i++) {
      lua_rawgeti(L, idx, i);
      char sub[256];
      snprintf(sub, sizeof(sub), "%s[%lld]", where, (long long)i);
      salt_toml *v = convert(L, -1, depth + 1, sub);
      lua_pop(L, 1);
      if (!v || salt_toml_array_push(arr, v) != SALT_OK) {
        salt_toml_free(arr);
        return NULL;
      }
    }
    return arr;
  }

  char **keys = calloc(count, sizeof(char *));
  if (!keys) return NULL;
  size_t k = 0;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    keys[k++] = salt_strdup(lua_tostring(L, -2));
    lua_pop(L, 1);
  }
  qsort(keys, count, sizeof(char *), key_cmp);
  salt_toml *tab = salt_toml_new(SALT_TOML_TABLE);
  for (size_t i = 0; tab && i < count; i++) {
    lua_getfield(L, idx, keys[i]);
    char sub[256];
    snprintf(sub, sizeof(sub), "%s.%s", where, keys[i]);
    salt_toml *v = convert(L, -1, depth + 1, sub);
    lua_pop(L, 1);
    if (!v || salt_toml_table_put(tab, keys[i], v) != SALT_OK) {
      salt_toml_free(tab);
      tab = NULL;
    }
  }
  for (size_t i = 0; i < count; i++) free(keys[i]);
  free(keys);
  return tab;
}

static salt_toml *convert(lua_State *L, int idx, int depth, const char *where) {
  if (depth > CONF_MAX_DEPTH) {
    salt_set_error("%s: nested too deeply", where);
    return NULL;
  }
  switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      return salt_toml_new_string(s, len);
    }
    case LUA_TBOOLEAN:
      return salt_toml_new_bool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) return salt_toml_new_int((long long)lua_tointeger(L, idx));
      salt_set_error("%s: only integers are allowed, not %s", where, lua_tostring(L, idx));
      return NULL;
    case LUA_TTABLE:
      return convert_table(L, idx, depth, where);
    default:
      salt_set_error("%s: a %s cannot be stored in configuration", where, luaL_typename(L, idx));
      return NULL;
  }
}

salt_toml *salt_conf_eval(const char *text, size_t len, const char *chunkname) {
  conf_state st = {0, 0};
  lua_State *L = lua_newstate(conf_alloc, &st);
  if (!L) {
    salt_set_error("%s: cannot start the Lua interpreter", chunkname);
    return NULL;
  }
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_settop(L, 0);

  char name[512];
  snprintf(name, sizeof(name), "@%s", chunkname);
  salt_toml *out = NULL;
  if (luaL_loadbufferx(L, text, len, name, "t") != LUA_OK) {
    salt_set_error("%s", lua_tostring(L, -1));
    goto done;
  }
  push_env(L);
  if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
  lua_sethook(L, conf_hook, LUA_MASKCOUNT, CONF_HOOK_STEP);
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    salt_set_error("%s", lua_tostring(L, -1));
    goto done;
  }
  lua_sethook(L, NULL, 0, 0);
  if (lua_type(L, -1) != LUA_TTABLE) {
    salt_set_error("%s: must return a table (got %s)", chunkname, luaL_typename(L, -1));
    goto done;
  }
  out = convert(L, -1, 0, chunkname);
  if (out && salt_toml_typeof(out) != SALT_TOML_TABLE) {
    salt_toml_free(out);
    out = NULL;
    salt_set_error("%s: must return a table with named fields, not a list", chunkname);
  }
done:
  lua_close(L);
  return out;
}

static bool is_lua(const char *name) {
  size_t n = name ? strlen(name) : 0;
  return n >= 4 && strcmp(name + n - 4, ".lua") == 0;
}

salt_toml *salt_conf_parse(const char *text, size_t len, const char *name) {
  if (is_lua(name)) return salt_conf_eval(text, len, name);
  salt_toml *t = salt_toml_parse(text, len);
  if (!t) salt_set_error("%s: not valid TOML", name ? name : "(input)");
  return t;
}

salt_toml *salt_conf_load(const char *path) {
  salt_buf b;
  if (salt_read_file(path, &b) != SALT_OK) {
    salt_set_error("cannot read %s", path);
    return NULL;
  }
  salt_toml *t = salt_conf_parse(b.data ? b.data : "", b.len, path);
  salt_buf_free(&b);
  return t;
}

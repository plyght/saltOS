#include "salt/toml.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct salt_toml {
  salt_toml_type type;
  union {
    char *s;
    long long i;
    bool b;
    struct {
      salt_toml **items;
      size_t len, cap;
    } arr;
    struct {
      char **keys;
      salt_toml **vals;
      size_t len, cap;
    } tab;
  } u;
};

typedef struct {
  const char *p;
  const char *end;
  int line;
} parser;

static salt_toml *node_new(salt_toml_type t) {
  salt_toml *n = calloc(1, sizeof(*n));
  if (n) n->type = t;
  return n;
}

static salt_toml *node_string(char *owned) {
  salt_toml *n = node_new(SALT_TOML_STRING);
  if (n) n->u.s = owned;
  return n;
}

static int table_set(salt_toml *t, char *key, salt_toml *val);
static salt_toml *table_get_mut(salt_toml *t, const char *key);

static int array_push(salt_toml *a, salt_toml *v) {
  if (a->u.arr.len == a->u.arr.cap) {
    size_t nc = a->u.arr.cap ? a->u.arr.cap * 2 : 4;
    salt_toml **ni = realloc(a->u.arr.items, nc * sizeof(salt_toml *));
    if (!ni) return SALT_ERR;
    a->u.arr.items = ni;
    a->u.arr.cap = nc;
  }
  a->u.arr.items[a->u.arr.len++] = v;
  return SALT_OK;
}

static int table_set(salt_toml *t, char *key, salt_toml *val) {
  for (size_t i = 0; i < t->u.tab.len; i++) {
    if (strcmp(t->u.tab.keys[i], key) == 0) {
      salt_toml_free(t->u.tab.vals[i]);
      free(key);
      t->u.tab.vals[i] = val;
      return SALT_OK;
    }
  }
  if (t->u.tab.len == t->u.tab.cap) {
    size_t nc = t->u.tab.cap ? t->u.tab.cap * 2 : 8;
    char **nk = realloc(t->u.tab.keys, nc * sizeof(char *));
    salt_toml **nv = realloc(t->u.tab.vals, nc * sizeof(salt_toml *));
    if (!nk || !nv) {
      free(nk);
      free(nv);
      return SALT_ERR;
    }
    t->u.tab.keys = nk;
    t->u.tab.vals = nv;
    t->u.tab.cap = nc;
  }
  t->u.tab.keys[t->u.tab.len] = key;
  t->u.tab.vals[t->u.tab.len] = val;
  t->u.tab.len++;
  return SALT_OK;
}

static salt_toml *table_get_mut(salt_toml *t, const char *key) {
  if (!t || t->type != SALT_TOML_TABLE) return NULL;
  for (size_t i = 0; i < t->u.tab.len; i++)
    if (strcmp(t->u.tab.keys[i], key) == 0) return t->u.tab.vals[i];
  return NULL;
}

void salt_toml_free(salt_toml *t) {
  if (!t) return;
  switch (t->type) {
    case SALT_TOML_STRING:
      free(t->u.s);
      break;
    case SALT_TOML_ARRAY:
      for (size_t i = 0; i < t->u.arr.len; i++) salt_toml_free(t->u.arr.items[i]);
      free(t->u.arr.items);
      break;
    case SALT_TOML_TABLE:
      for (size_t i = 0; i < t->u.tab.len; i++) {
        free(t->u.tab.keys[i]);
        salt_toml_free(t->u.tab.vals[i]);
      }
      free(t->u.tab.keys);
      free(t->u.tab.vals);
      break;
    default:
      break;
  }
  free(t);
}

static void skip_ws_inline(parser *ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t')) ps->p++;
}

static void skip_ws_and_comments(parser *ps) {
  while (ps->p < ps->end) {
    char c = *ps->p;
    if (c == ' ' || c == '\t' || c == '\r') {
      ps->p++;
    } else if (c == '\n') {
      ps->line++;
      ps->p++;
    } else if (c == '#') {
      while (ps->p < ps->end && *ps->p != '\n') ps->p++;
    } else {
      break;
    }
  }
}

static void append_utf8(salt_buf *b, unsigned cp) {
  if (cp < 0x80) {
    char c = (char)cp;
    salt_buf_append(b, &c, 1);
  } else if (cp < 0x800) {
    char t[2] = {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
    salt_buf_append(b, t, 2);
  } else if (cp < 0x10000) {
    char t[3] = {(char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)),
                 (char)(0x80 | (cp & 0x3F))};
    salt_buf_append(b, t, 3);
  } else {
    char t[4] = {(char)(0xF0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 0x3F)),
                 (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
    salt_buf_append(b, t, 4);
  }
}

static int parse_escape(parser *ps, salt_buf *out) {
  ps->p++;
  if (ps->p >= ps->end) return SALT_ERR_FORMAT;
  char c = *ps->p++;
  switch (c) {
    case 'b': salt_buf_append(out, "\b", 1); break;
    case 't': salt_buf_append(out, "\t", 1); break;
    case 'n': salt_buf_append(out, "\n", 1); break;
    case 'f': salt_buf_append(out, "\f", 1); break;
    case 'r': salt_buf_append(out, "\r", 1); break;
    case '"': salt_buf_append(out, "\"", 1); break;
    case '\\': salt_buf_append(out, "\\", 1); break;
    case 'u':
    case 'U': {
      int n = (c == 'u') ? 4 : 8;
      unsigned cp = 0;
      for (int i = 0; i < n; i++) {
        if (ps->p >= ps->end || !isxdigit((unsigned char)*ps->p)) return SALT_ERR_FORMAT;
        char h = *ps->p++;
        cp = cp * 16 + (h <= '9' ? h - '0' : (tolower(h) - 'a' + 10));
      }
      append_utf8(out, cp);
      break;
    }
    default:
      salt_set_error("toml: bad escape \\%c at line %d", c, ps->line);
      return SALT_ERR_FORMAT;
  }
  return SALT_OK;
}

static salt_toml *parse_basic_string(parser *ps) {
  ps->p++;
  salt_buf b;
  salt_buf_init(&b);
  while (ps->p < ps->end) {
    char c = *ps->p;
    if (c == '"') {
      ps->p++;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    if (c == '\\') {
      if (parse_escape(ps, &b) != SALT_OK) {
        salt_buf_free(&b);
        return NULL;
      }
      continue;
    }
    if (c == '\n') {
      salt_set_error("toml: newline in string at line %d", ps->line);
      salt_buf_free(&b);
      return NULL;
    }
    salt_buf_append(&b, &c, 1);
    ps->p++;
  }
  salt_buf_free(&b);
  salt_set_error("toml: unterminated string");
  return NULL;
}

static salt_toml *parse_multiline_basic(parser *ps) {
  ps->p += 3;
  if (ps->p < ps->end && *ps->p == '\n') {
    ps->line++;
    ps->p++;
  } else if (ps->p + 1 < ps->end && ps->p[0] == '\r' && ps->p[1] == '\n') {
    ps->line++;
    ps->p += 2;
  }
  salt_buf b;
  salt_buf_init(&b);
  while (ps->p < ps->end) {
    if (ps->p + 2 < ps->end && ps->p[0] == '"' && ps->p[1] == '"' && ps->p[2] == '"' &&
        !(ps->p + 3 < ps->end && ps->p[3] == '"')) {
      ps->p += 3;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    if (ps->p + 2 == ps->end && ps->p[0] == '"' && ps->p[1] == '"' && ps->p[2] == '"') {
      ps->p += 3;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    char c = *ps->p;
    if (c == '\\') {
      const char *save = ps->p;
      ps->p++;
      const char *q = ps->p;
      bool only_ws = false;
      while (q < ps->end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
      if (q < ps->end && *q == '\n') only_ws = true;
      if (only_ws) {
        while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r')) ps->p++;
        if (ps->p < ps->end && *ps->p == '\n') {
          ps->line++;
          ps->p++;
        }
        while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n')) {
          if (*ps->p == '\n') ps->line++;
          ps->p++;
        }
        continue;
      }
      ps->p = save;
      if (parse_escape(ps, &b) != SALT_OK) {
        salt_buf_free(&b);
        return NULL;
      }
      continue;
    }
    if (c == '\n') ps->line++;
    salt_buf_append(&b, &c, 1);
    ps->p++;
  }
  salt_buf_free(&b);
  salt_set_error("toml: unterminated multiline string");
  return NULL;
}

static salt_toml *parse_literal_string(parser *ps) {
  ps->p++;
  salt_buf b;
  salt_buf_init(&b);
  while (ps->p < ps->end) {
    char c = *ps->p;
    if (c == '\'') {
      ps->p++;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    if (c == '\n') {
      salt_set_error("toml: newline in literal string at line %d", ps->line);
      salt_buf_free(&b);
      return NULL;
    }
    salt_buf_append(&b, &c, 1);
    ps->p++;
  }
  salt_buf_free(&b);
  salt_set_error("toml: unterminated literal string");
  return NULL;
}

static salt_toml *parse_multiline_literal(parser *ps) {
  ps->p += 3;
  if (ps->p < ps->end && *ps->p == '\n') {
    ps->line++;
    ps->p++;
  }
  salt_buf b;
  salt_buf_init(&b);
  while (ps->p < ps->end) {
    if (ps->p + 2 < ps->end && ps->p[0] == '\'' && ps->p[1] == '\'' && ps->p[2] == '\'') {
      ps->p += 3;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    if (ps->p + 2 == ps->end && ps->p[0] == '\'' && ps->p[1] == '\'' && ps->p[2] == '\'') {
      ps->p += 3;
      return node_string(b.data ? b.data : salt_strdup(""));
    }
    char c = *ps->p;
    if (c == '\n') ps->line++;
    salt_buf_append(&b, &c, 1);
    ps->p++;
  }
  salt_buf_free(&b);
  salt_set_error("toml: unterminated multiline literal");
  return NULL;
}

static salt_toml *parse_value(parser *ps);

static salt_toml *parse_array(parser *ps) {
  ps->p++;
  salt_toml *a = node_new(SALT_TOML_ARRAY);
  if (!a) return NULL;
  for (;;) {
    skip_ws_and_comments(ps);
    if (ps->p >= ps->end) {
      salt_set_error("toml: unterminated array");
      salt_toml_free(a);
      return NULL;
    }
    if (*ps->p == ']') {
      ps->p++;
      return a;
    }
    salt_toml *v = parse_value(ps);
    if (!v) {
      salt_toml_free(a);
      return NULL;
    }
    array_push(a, v);
    skip_ws_and_comments(ps);
    if (ps->p < ps->end && *ps->p == ',') {
      ps->p++;
      continue;
    }
    skip_ws_and_comments(ps);
    if (ps->p < ps->end && *ps->p == ']') {
      ps->p++;
      return a;
    }
  }
}

static char *parse_key_token(parser *ps);

static salt_toml *parse_inline_table(parser *ps) {
  ps->p++;
  salt_toml *t = node_new(SALT_TOML_TABLE);
  if (!t) return NULL;
  skip_ws_inline(ps);
  if (ps->p < ps->end && *ps->p == '}') {
    ps->p++;
    return t;
  }
  for (;;) {
    skip_ws_inline(ps);
    char *key = parse_key_token(ps);
    if (!key) {
      salt_toml_free(t);
      return NULL;
    }
    skip_ws_inline(ps);
    if (ps->p >= ps->end || *ps->p != '=') {
      salt_set_error("toml: expected = in inline table at line %d", ps->line);
      free(key);
      salt_toml_free(t);
      return NULL;
    }
    ps->p++;
    skip_ws_inline(ps);
    salt_toml *v = parse_value(ps);
    if (!v) {
      free(key);
      salt_toml_free(t);
      return NULL;
    }
    table_set(t, key, v);
    skip_ws_inline(ps);
    if (ps->p < ps->end && *ps->p == ',') {
      ps->p++;
      continue;
    }
    if (ps->p < ps->end && *ps->p == '}') {
      ps->p++;
      return t;
    }
    salt_set_error("toml: malformed inline table at line %d", ps->line);
    salt_toml_free(t);
    return NULL;
  }
}

static salt_toml *parse_scalar_token(parser *ps) {
  const char *start = ps->p;
  while (ps->p < ps->end) {
    char c = *ps->p;
    if (c == ',' || c == ']' || c == '}' || c == '\n' || c == '#' || c == ' ' || c == '\t' ||
        c == '\r')
      break;
    ps->p++;
  }
  size_t n = (size_t)(ps->p - start);
  char *tok = malloc(n + 1);
  if (!tok) return NULL;
  memcpy(tok, start, n);
  tok[n] = '\0';
  if (strcmp(tok, "true") == 0) {
    free(tok);
    salt_toml *b = node_new(SALT_TOML_BOOL);
    b->u.b = true;
    return b;
  }
  if (strcmp(tok, "false") == 0) {
    free(tok);
    salt_toml *b = node_new(SALT_TOML_BOOL);
    b->u.b = false;
    return b;
  }
  bool is_int = true;
  size_t start_i = 0;
  if (tok[0] == '+' || tok[0] == '-') start_i = 1;
  if (tok[start_i] == '\0') is_int = false;
  for (size_t i = start_i; tok[i]; i++) {
    if (tok[i] == '_') continue;
    if (!isdigit((unsigned char)tok[i])) {
      is_int = false;
      break;
    }
  }
  if (is_int) {
    char clean[64];
    size_t ci = 0;
    for (size_t i = 0; tok[i] && ci < sizeof(clean) - 1; i++)
      if (tok[i] != '_') clean[ci++] = tok[i];
    clean[ci] = '\0';
    salt_toml *v = node_new(SALT_TOML_INT);
    v->u.i = strtoll(clean, NULL, 10);
    free(tok);
    return v;
  }
  return node_string(tok);
}

static salt_toml *parse_value(parser *ps) {
  skip_ws_inline(ps);
  if (ps->p >= ps->end) {
    salt_set_error("toml: unexpected end of value at line %d", ps->line);
    return NULL;
  }
  char c = *ps->p;
  if (c == '"') {
    if (ps->p + 2 < ps->end && ps->p[1] == '"' && ps->p[2] == '"') return parse_multiline_basic(ps);
    return parse_basic_string(ps);
  }
  if (c == '\'') {
    if (ps->p + 2 < ps->end && ps->p[1] == '\'' && ps->p[2] == '\'')
      return parse_multiline_literal(ps);
    return parse_literal_string(ps);
  }
  if (c == '[') return parse_array(ps);
  if (c == '{') return parse_inline_table(ps);
  return parse_scalar_token(ps);
}

static char *parse_key_token(parser *ps) {
  skip_ws_inline(ps);
  if (ps->p >= ps->end) return NULL;
  if (*ps->p == '"') {
    salt_toml *s = parse_basic_string(ps);
    if (!s) return NULL;
    char *k = s->u.s;
    s->u.s = NULL;
    salt_toml_free(s);
    return k;
  }
  if (*ps->p == '\'') {
    salt_toml *s = parse_literal_string(ps);
    if (!s) return NULL;
    char *k = s->u.s;
    s->u.s = NULL;
    salt_toml_free(s);
    return k;
  }
  const char *start = ps->p;
  while (ps->p < ps->end) {
    char c = *ps->p;
    if (isalnum((unsigned char)c) || c == '_' || c == '-')
      ps->p++;
    else
      break;
  }
  if (ps->p == start) {
    salt_set_error("toml: expected key at line %d", ps->line);
    return NULL;
  }
  size_t n = (size_t)(ps->p - start);
  char *k = malloc(n + 1);
  if (!k) return NULL;
  memcpy(k, start, n);
  k[n] = '\0';
  return k;
}

static int parse_dotted_path(parser *ps, char ***out_keys, size_t *out_n) {
  char **keys = NULL;
  size_t n = 0, cap = 0;
  for (;;) {
    char *k = parse_key_token(ps);
    if (!k) {
      for (size_t i = 0; i < n; i++) free(keys[i]);
      free(keys);
      return SALT_ERR_FORMAT;
    }
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      keys = realloc(keys, cap * sizeof(char *));
    }
    keys[n++] = k;
    skip_ws_inline(ps);
    if (ps->p < ps->end && *ps->p == '.') {
      ps->p++;
      continue;
    }
    break;
  }
  *out_keys = keys;
  *out_n = n;
  return SALT_OK;
}

static salt_toml *descend_table(salt_toml *root, char **keys, size_t n, bool array_of_tables) {
  salt_toml *cur = root;
  for (size_t i = 0; i < n; i++) {
    bool last = (i + 1 == n);
    salt_toml *child = table_get_mut(cur, keys[i]);
    if (last && array_of_tables) {
      if (!child) {
        child = node_new(SALT_TOML_ARRAY);
        table_set(cur, salt_strdup(keys[i]), child);
      }
      if (child->type != SALT_TOML_ARRAY) return NULL;
      salt_toml *t = node_new(SALT_TOML_TABLE);
      array_push(child, t);
      return t;
    }
    if (!child) {
      child = node_new(SALT_TOML_TABLE);
      table_set(cur, salt_strdup(keys[i]), child);
    }
    if (child->type == SALT_TOML_ARRAY && child->u.arr.len > 0) {
      child = child->u.arr.items[child->u.arr.len - 1];
    }
    if (child->type != SALT_TOML_TABLE) return NULL;
    cur = child;
  }
  return cur;
}

salt_toml *salt_toml_parse(const char *text, size_t len) {
  parser ps = {text, text + len, 1};
  salt_toml *root = node_new(SALT_TOML_TABLE);
  if (!root) return NULL;
  salt_toml *current = root;
  for (;;) {
    skip_ws_and_comments(&ps);
    if (ps.p >= ps.end) break;
    if (*ps.p == '[') {
      bool aot = false;
      ps.p++;
      if (ps.p < ps.end && *ps.p == '[') {
        aot = true;
        ps.p++;
      }
      char **keys;
      size_t n;
      if (parse_dotted_path(&ps, &keys, &n) != SALT_OK) {
        salt_toml_free(root);
        return NULL;
      }
      skip_ws_inline(&ps);
      if (ps.p < ps.end && *ps.p == ']') ps.p++;
      if (aot && ps.p < ps.end && *ps.p == ']') ps.p++;
      salt_toml *tbl = descend_table(root, keys, n, aot);
      for (size_t i = 0; i < n; i++) free(keys[i]);
      free(keys);
      if (!tbl) {
        salt_set_error("toml: table path conflict at line %d", ps.line);
        salt_toml_free(root);
        return NULL;
      }
      current = tbl;
      continue;
    }
    char **keys;
    size_t n;
    if (parse_dotted_path(&ps, &keys, &n) != SALT_OK) {
      salt_toml_free(root);
      return NULL;
    }
    skip_ws_inline(&ps);
    if (ps.p >= ps.end || *ps.p != '=') {
      salt_set_error("toml: expected = at line %d", ps.line);
      for (size_t i = 0; i < n; i++) free(keys[i]);
      free(keys);
      salt_toml_free(root);
      return NULL;
    }
    ps.p++;
    salt_toml *val = parse_value(&ps);
    if (!val) {
      for (size_t i = 0; i < n; i++) free(keys[i]);
      free(keys);
      salt_toml_free(root);
      return NULL;
    }
    salt_toml *dest = current;
    if (n > 1) {
      dest = descend_table(current, keys, n - 1, false);
      if (!dest) {
        salt_set_error("toml: key path conflict at line %d", ps.line);
        for (size_t i = 0; i < n; i++) free(keys[i]);
        free(keys);
        salt_toml_free(val);
        salt_toml_free(root);
        return NULL;
      }
    }
    table_set(dest, salt_strdup(keys[n - 1]), val);
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    skip_ws_inline(&ps);
    if (ps.p < ps.end && *ps.p == '#')
      while (ps.p < ps.end && *ps.p != '\n') ps.p++;
  }
  return root;
}

salt_toml *salt_toml_parse_file(const char *path) {
  salt_buf b;
  if (salt_read_file(path, &b) != SALT_OK) return NULL;
  salt_toml *t = salt_toml_parse(b.data ? b.data : "", b.len);
  salt_buf_free(&b);
  return t;
}

salt_toml_type salt_toml_typeof(const salt_toml *t) {
  return t->type;
}

const salt_toml *salt_toml_get(const salt_toml *table, const char *key) {
  if (!table || table->type != SALT_TOML_TABLE) return NULL;
  for (size_t i = 0; i < table->u.tab.len; i++)
    if (strcmp(table->u.tab.keys[i], key) == 0) return table->u.tab.vals[i];
  return NULL;
}

const salt_toml *salt_toml_path(const salt_toml *table, const char *dotted_key) {
  const salt_toml *cur = table;
  const char *p = dotted_key;
  char part[256];
  while (cur && *p) {
    size_t i = 0;
    while (*p && *p != '.' && i < sizeof(part) - 1) part[i++] = *p++;
    part[i] = '\0';
    if (*p == '.') p++;
    cur = salt_toml_get(cur, part);
  }
  return cur;
}

size_t salt_toml_array_len(const salt_toml *array) {
  if (!array || array->type != SALT_TOML_ARRAY) return 0;
  return array->u.arr.len;
}

const salt_toml *salt_toml_array_at(const salt_toml *array, size_t i) {
  if (!array || array->type != SALT_TOML_ARRAY || i >= array->u.arr.len) return NULL;
  return array->u.arr.items[i];
}

const char *salt_toml_as_string(const salt_toml *t) {
  if (!t || t->type != SALT_TOML_STRING) return NULL;
  return t->u.s;
}

long long salt_toml_as_int(const salt_toml *t, long long fallback) {
  if (!t) return fallback;
  if (t->type == SALT_TOML_INT) return t->u.i;
  return fallback;
}

bool salt_toml_as_bool(const salt_toml *t, bool fallback) {
  if (!t || t->type != SALT_TOML_BOOL) return fallback;
  return t->u.b;
}

const char *salt_toml_string(const salt_toml *table, const char *dotted_key, const char *fallback) {
  const salt_toml *v = salt_toml_path(table, dotted_key);
  const char *s = salt_toml_as_string(v);
  return s ? s : fallback;
}

long long salt_toml_int(const salt_toml *table, const char *dotted_key, long long fallback) {
  return salt_toml_as_int(salt_toml_path(table, dotted_key), fallback);
}

bool salt_toml_bool(const salt_toml *table, const char *dotted_key, bool fallback) {
  return salt_toml_as_bool(salt_toml_path(table, dotted_key), fallback);
}

int salt_toml_string_array(const salt_toml *table, const char *dotted_key, salt_strlist *out) {
  const salt_toml *a = salt_toml_path(table, dotted_key);
  if (!a || a->type != SALT_TOML_ARRAY) return SALT_ERR_NOTFOUND;
  for (size_t i = 0; i < a->u.arr.len; i++) {
    const salt_toml *e = a->u.arr.items[i];
    if (e->type == SALT_TOML_STRING) salt_strlist_push(out, e->u.s);
  }
  return SALT_OK;
}

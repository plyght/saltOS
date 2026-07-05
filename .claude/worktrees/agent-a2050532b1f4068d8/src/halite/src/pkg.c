#include "salt/pkg.h"
#include "salt/toml.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void salt_pkg_meta_init(salt_pkg_meta *m) {
  memset(m, 0, sizeof(*m));
  salt_strlist_init(&m->deps);
}

void salt_pkg_meta_free(salt_pkg_meta *m) {
  free(m->name);
  free(m->version);
  free(m->arch);
  free(m->summary);
  free(m->license);
  free(m->repro_status);
  free(m->repro_reason);
  salt_strlist_free(&m->deps);
  memset(m, 0, sizeof(*m));
}

static void toml_escape(salt_buf *b, const char *s) {
  salt_buf_append_str(b, "\"");
  for (const char *p = s ? s : ""; *p; p++) {
    if (*p == '"' || *p == '\\') {
      char e[2] = {'\\', *p};
      salt_buf_append(b, e, 2);
    } else if (*p == '\n') {
      salt_buf_append_str(b, "\\n");
    } else {
      salt_buf_append(b, p, 1);
    }
  }
  salt_buf_append_str(b, "\"");
}

static void emit_str_array(salt_buf *b, const char *key, const salt_strlist *l) {
  salt_buf_printf(b, "%s = [", key);
  for (size_t i = 0; i < l->len; i++) {
    if (i) salt_buf_append_str(b, ", ");
    toml_escape(b, l->items[i]);
  }
  salt_buf_append_str(b, "]\n");
}

int salt_pkg_meta_to_toml(const salt_pkg_meta *m, salt_buf *out) {
  salt_buf_init(out);
  salt_buf_append_str(out, "name = ");
  toml_escape(out, m->name);
  salt_buf_append_str(out, "\n");
  salt_buf_append_str(out, "version = ");
  toml_escape(out, m->version);
  salt_buf_append_str(out, "\n");
  salt_buf_printf(out, "release = %d\n", m->release);
  salt_buf_append_str(out, "arch = ");
  toml_escape(out, m->arch);
  salt_buf_append_str(out, "\n");
  salt_buf_append_str(out, "summary = ");
  toml_escape(out, m->summary ? m->summary : "");
  salt_buf_append_str(out, "\n");
  salt_buf_append_str(out, "license = ");
  toml_escape(out, m->license ? m->license : "");
  salt_buf_append_str(out, "\n");
  emit_str_array(out, "deps", &m->deps);
  salt_buf_append_str(out, "\n[reproducibility]\n");
  salt_buf_append_str(out, "status = ");
  toml_escape(out, m->repro_status ? m->repro_status : "unverified");
  salt_buf_append_str(out, "\n");
  if (m->repro_reason && m->repro_reason[0]) {
    salt_buf_append_str(out, "reason = ");
    toml_escape(out, m->repro_reason);
    salt_buf_append_str(out, "\n");
  }
  return SALT_OK;
}

int salt_pkg_meta_from_toml(const char *text, size_t len, salt_pkg_meta *out) {
  salt_pkg_meta_init(out);
  salt_toml *t = salt_toml_parse(text, len);
  if (!t) return SALT_ERR_FORMAT;
  out->name = salt_strdup(salt_toml_string(t, "name", ""));
  out->version = salt_strdup(salt_toml_string(t, "version", ""));
  out->release = (int)salt_toml_int(t, "release", 1);
  out->arch = salt_strdup(salt_toml_string(t, "arch", ""));
  out->summary = salt_strdup(salt_toml_string(t, "summary", ""));
  out->license = salt_strdup(salt_toml_string(t, "license", ""));
  out->repro_status = salt_strdup(salt_toml_string(t, "reproducibility.status", "unverified"));
  const char *reason = salt_toml_string(t, "reproducibility.reason", NULL);
  out->repro_reason = reason ? salt_strdup(reason) : NULL;
  salt_toml_string_array(t, "deps", &out->deps);
  salt_toml_free(t);
  return SALT_OK;
}

void salt_manifest_init(salt_manifest *m) {
  m->items = NULL;
  m->len = 0;
  m->cap = 0;
}

int salt_manifest_push(salt_manifest *m, const salt_manifest_entry *e) {
  if (m->len == m->cap) {
    size_t nc = m->cap ? m->cap * 2 : 32;
    salt_manifest_entry *ni = realloc(m->items, nc * sizeof(*ni));
    if (!ni) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    m->items = ni;
    m->cap = nc;
  }
  salt_manifest_entry *d = &m->items[m->len++];
  d->path = salt_strdup(e->path);
  d->typeflag = e->typeflag;
  d->mode = e->mode;
  d->size = e->size;
  d->sha256 = e->sha256 ? salt_strdup(e->sha256) : NULL;
  d->linkname = e->linkname ? salt_strdup(e->linkname) : NULL;
  return SALT_OK;
}

void salt_manifest_free(salt_manifest *m) {
  for (size_t i = 0; i < m->len; i++) {
    free(m->items[i].path);
    free(m->items[i].sha256);
    free(m->items[i].linkname);
  }
  free(m->items);
  m->items = NULL;
  m->len = 0;
  m->cap = 0;
}

int salt_manifest_to_toml(const salt_manifest *m, salt_buf *out) {
  salt_buf_init(out);
  for (size_t i = 0; i < m->len; i++) {
    const salt_manifest_entry *e = &m->items[i];
    salt_buf_append_str(out, "[[file]]\n");
    salt_buf_append_str(out, "path = ");
    toml_escape(out, e->path);
    salt_buf_append_str(out, "\n");
    char tf[2] = {e->typeflag ? e->typeflag : '0', 0};
    salt_buf_printf(out, "type = \"%s\"\n", tf);
    salt_buf_printf(out, "mode = \"%04o\"\n", e->mode & 07777);
    salt_buf_printf(out, "size = %llu\n", (unsigned long long)e->size);
    if (e->sha256 && e->sha256[0]) salt_buf_printf(out, "sha256 = \"%s\"\n", e->sha256);
    if (e->linkname && e->linkname[0]) {
      salt_buf_append_str(out, "linkname = ");
      toml_escape(out, e->linkname);
      salt_buf_append_str(out, "\n");
    }
    salt_buf_append_str(out, "\n");
  }
  return SALT_OK;
}

int salt_manifest_from_toml(const char *text, size_t len, salt_manifest *out) {
  salt_manifest_init(out);
  salt_toml *t = salt_toml_parse(text, len);
  if (!t) return SALT_ERR_FORMAT;
  const salt_toml *files = salt_toml_get(t, "file");
  size_t n = salt_toml_array_len(files);
  for (size_t i = 0; i < n; i++) {
    const salt_toml *f = salt_toml_array_at(files, i);
    salt_manifest_entry e;
    memset(&e, 0, sizeof(e));
    e.path = (char *)salt_toml_string(f, "path", "");
    const char *type = salt_toml_string(f, "type", "0");
    e.typeflag = type[0];
    const char *mode = salt_toml_string(f, "mode", "0644");
    e.mode = (unsigned)strtoul(mode, NULL, 8);
    e.size = (uint64_t)salt_toml_int(f, "size", 0);
    e.sha256 = (char *)salt_toml_string(f, "sha256", NULL);
    e.linkname = (char *)salt_toml_string(f, "linkname", NULL);
    salt_manifest_push(out, &e);
  }
  salt_toml_free(t);
  return SALT_OK;
}

char *salt_pkg_filename(const salt_pkg_meta *m) {
  salt_buf b;
  salt_buf_init(&b);
  salt_buf_printf(&b, "%s-%s-%d-%s.grain", m->name, m->version, m->release, m->arch);
  return b.data;
}

#include "salt/repo.h"
#include "salt/toml.h"
#include "salt/archive.h"
#include "salt/hash.h"
#include "salt/sign.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

void salt_repo_index_init(salt_repo_index *idx) {
  memset(idx, 0, sizeof(*idx));
}

static void entry_free(salt_repo_entry *e) {
  free(e->name);
  free(e->version);
  free(e->arch);
  free(e->filename);
  free(e->sha256);
  salt_strlist_free(&e->deps);
}

void salt_repo_index_free(salt_repo_index *idx) {
  for (size_t i = 0; i < idx->len; i++) entry_free(&idx->items[i]);
  free(idx->items);
  free(idx->repo_name);
  free(idx->arch);
  memset(idx, 0, sizeof(*idx));
}

static int index_push(salt_repo_index *idx, salt_repo_entry *e) {
  if (idx->len == idx->cap) {
    size_t nc = idx->cap ? idx->cap * 2 : 16;
    salt_repo_entry *ni = realloc(idx->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    idx->items = ni;
    idx->cap = nc;
  }
  idx->items[idx->len++] = *e;
  return SALT_OK;
}

int salt_repo_index_load(const char *path, salt_repo_index *out) {
  salt_repo_index_init(out);
  salt_toml *t = salt_toml_parse_file(path);
  if (!t) {
    salt_set_error("repo index parse: %s", path);
    return SALT_ERR_FORMAT;
  }
  out->repo_name = salt_strdup(salt_toml_string(t, "repo", "current"));
  out->arch = salt_strdup(salt_toml_string(t, "arch", ""));
  const salt_toml *pkgs = salt_toml_get(t, "package");
  size_t n = salt_toml_array_len(pkgs);
  for (size_t i = 0; i < n; i++) {
    const salt_toml *p = salt_toml_array_at(pkgs, i);
    salt_repo_entry e;
    memset(&e, 0, sizeof(e));
    salt_strlist_init(&e.deps);
    e.name = salt_strdup(salt_toml_string(p, "name", ""));
    e.version = salt_strdup(salt_toml_string(p, "version", ""));
    e.release = (int)salt_toml_int(p, "release", 1);
    e.arch = salt_strdup(salt_toml_string(p, "arch", ""));
    e.filename = salt_strdup(salt_toml_string(p, "filename", ""));
    e.sha256 = salt_strdup(salt_toml_string(p, "sha256", ""));
    e.size = (uint64_t)salt_toml_int(p, "size", 0);
    salt_toml_string_array(p, "deps", &e.deps);
    index_push(out, &e);
  }
  salt_toml_free(t);
  return SALT_OK;
}

int salt_repo_index_to_toml(const salt_repo_index *idx, salt_buf *out) {
  salt_buf_init(out);
  salt_buf_printf(out, "repo = \"%s\"\n", idx->repo_name ? idx->repo_name : "current");
  salt_buf_printf(out, "arch = \"%s\"\n", idx->arch ? idx->arch : "");
  for (size_t i = 0; i < idx->len; i++) {
    const salt_repo_entry *e = &idx->items[i];
    salt_buf_append_str(out, "\n[[package]]\n");
    salt_buf_printf(out, "name = \"%s\"\n", e->name);
    salt_buf_printf(out, "version = \"%s\"\n", e->version);
    salt_buf_printf(out, "release = %d\n", e->release);
    salt_buf_printf(out, "arch = \"%s\"\n", e->arch);
    salt_buf_printf(out, "filename = \"%s\"\n", e->filename);
    salt_buf_printf(out, "sha256 = \"%s\"\n", e->sha256);
    salt_buf_printf(out, "size = %llu\n", (unsigned long long)e->size);
    salt_buf_append_str(out, "deps = [");
    for (size_t j = 0; j < e->deps.len; j++) {
      if (j) salt_buf_append_str(out, ", ");
      salt_buf_printf(out, "\"%s\"", e->deps.items[j]);
    }
    salt_buf_append_str(out, "]\n");
  }
  return SALT_OK;
}

const salt_repo_entry *salt_repo_index_find(const salt_repo_index *idx, const char *name) {
  for (size_t i = 0; i < idx->len; i++)
    if (strcmp(idx->items[i].name, name) == 0) return &idx->items[i];
  return NULL;
}

int salt_repo_build_index(const char *packages_dir, const char *repo_name, const char *arch,
                          salt_repo_index *out) {
  salt_repo_index_init(out);
  out->repo_name = salt_strdup(repo_name);
  out->arch = salt_strdup(arch);
  DIR *d = opendir(packages_dir);
  if (!d) {
    salt_set_error("opendir %s", packages_dir);
    return SALT_ERR_IO;
  }
  salt_strlist names;
  salt_strlist_init(&names);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t l = strlen(de->d_name);
    if (l > 8 && strcmp(de->d_name + l - 8, ".saltpkg") == 0) salt_strlist_push(&names, de->d_name);
  }
  closedir(d);
  for (size_t i = 0; i + 1 < names.len; i++)
    for (size_t j = i + 1; j < names.len; j++)
      if (strcmp(names.items[i], names.items[j]) > 0) {
        char *t = names.items[i];
        names.items[i] = names.items[j];
        names.items[j] = t;
      }
  for (size_t i = 0; i < names.len; i++) {
    char *full = salt_join_path(packages_dir, names.items[i]);
    salt_archive ar;
    if (salt_archive_open(full, &ar) != SALT_OK) {
      free(full);
      continue;
    }
    salt_repo_entry e;
    memset(&e, 0, sizeof(e));
    salt_strlist_init(&e.deps);
    e.name = salt_strdup(ar.meta.name);
    e.version = salt_strdup(ar.meta.version);
    e.release = ar.meta.release;
    e.arch = salt_strdup(ar.meta.arch);
    e.filename = salt_strdup(names.items[i]);
    char hex[SALT_SHA256_HEXLEN + 1];
    salt_sha256_file(full, hex);
    e.sha256 = salt_strdup(hex);
    salt_buf fb;
    if (salt_read_file(full, &fb) == SALT_OK) {
      e.size = fb.len;
      salt_buf_free(&fb);
    }
    for (size_t j = 0; j < ar.meta.deps.len; j++) salt_strlist_push(&e.deps, ar.meta.deps.items[j]);
    index_push(out, &e);
    salt_archive_free(&ar);
    free(full);
  }
  salt_strlist_free(&names);
  return SALT_OK;
}

int salt_repo_publish(const char *out_dir, const char *repo_name, const char *arch,
                      const char *sec_key_hex) {
  char *pkgdir = salt_join_path(out_dir, "packages");
  salt_repo_index idx;
  int rc = salt_repo_build_index(pkgdir, repo_name, arch, &idx);
  free(pkgdir);
  if (rc != SALT_OK) return rc;
  salt_buf toml;
  salt_repo_index_to_toml(&idx, &toml);
  salt_repo_index_free(&idx);
  char *index_path = salt_join_path(out_dir, "index.toml");
  rc = salt_write_file(index_path, toml.data, toml.len, 0644);
  if (rc == SALT_OK && sec_key_hex && sec_key_hex[0]) {
    char sig[SALT_SIG_HEXLEN + 1];
    rc = salt_sign_buf(toml.data, toml.len, sec_key_hex, sig);
    if (rc == SALT_OK) {
      char *sig_path = salt_join_path(out_dir, "index.toml.sig");
      rc = salt_write_file(sig_path, sig, strlen(sig), 0644);
      free(sig_path);
    }
  }
  free(index_path);
  salt_buf_free(&toml);
  return rc;
}

static int fetch_http(const char *url, salt_buf *out) {
  salt_buf cmd;
  salt_buf_init(&cmd);
  salt_buf_printf(&cmd, "curl -fsSL '%s'", url);
  FILE *p = popen(cmd.data, "r");
  salt_buf_free(&cmd);
  if (!p) {
    salt_set_error("fetch: cannot run curl for %s", url);
    return SALT_ERR_IO;
  }
  salt_buf_init(out);
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) salt_buf_append(out, chunk, n);
  int status = pclose(p);
  if (status != 0) {
    salt_set_error("fetch failed: %s", url);
    salt_buf_free(out);
    return SALT_ERR_IO;
  }
  return SALT_OK;
}

int salt_fetch(const char *url_or_path, salt_buf *out) {
  if (strncmp(url_or_path, "http://", 7) == 0 || strncmp(url_or_path, "https://", 8) == 0)
    return fetch_http(url_or_path, out);
  const char *path = url_or_path;
  if (strncmp(path, "file://", 7) == 0) path += 7;
  return salt_read_file(path, out);
}

int salt_fetch_to_file(const char *url_or_path, const char *dest_path) {
  salt_buf b;
  if (salt_fetch(url_or_path, &b) != SALT_OK) return SALT_ERR_IO;
  int rc = salt_write_file(dest_path, b.data, b.len, 0644);
  salt_buf_free(&b);
  return rc;
}

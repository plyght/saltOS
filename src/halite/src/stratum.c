#include "salt/stratum.h"
#include "salt/util.h"
#include "salt/toml.h"
#include "salt/repo.h"
#include "salt/hash.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <dirent.h>

struct salt_strata_db {
  sqlite3 *h;
};

static const char *STRATA_SCHEMA =
    "CREATE TABLE IF NOT EXISTS strata(name TEXT PRIMARY KEY, family TEXT, arch TEXT, root TEXT, "
    "package_manager TEXT, bootstrap TEXT, trust TEXT, state TEXT, graphics INTEGER, audio INTEGER, "
    "dbus INTEGER, created INTEGER);"
    "CREATE TABLE IF NOT EXISTS stratum_repos(stratum TEXT, name TEXT, url TEXT);"
    "CREATE TABLE IF NOT EXISTS stratum_snapshots(id INTEGER PRIMARY KEY AUTOINCREMENT, stratum "
    "TEXT, label TEXT, path TEXT, kind TEXT, time INTEGER);"
    "CREATE TABLE IF NOT EXISTS exposed(alias TEXT PRIMARY KEY, stratum TEXT, command TEXT, kind "
    "TEXT, shim_path TEXT, created INTEGER);"
    "CREATE TABLE IF NOT EXISTS providers(component TEXT PRIMARY KEY, provider TEXT, source TEXT, "
    "trust TEXT, rollback_target INTEGER, time INTEGER);"
    "CREATE TABLE IF NOT EXISTS provider_history(id INTEGER PRIMARY KEY AUTOINCREMENT, component "
    "TEXT, provider TEXT, source TEXT, trust TEXT, time INTEGER);";

void salt_stratum_free_fields(salt_stratum *s) {
  free(s->name);
  free(s->family);
  free(s->arch);
  free(s->root);
  free(s->package_manager);
  free(s->bootstrap);
  free(s->trust);
  free(s->state);
  memset(s, 0, sizeof(*s));
}

void salt_stratum_list_init(salt_stratum_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_stratum_list_free(salt_stratum_list *l) {
  for (size_t i = 0; i < l->len; i++) salt_stratum_free_fields(&l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int stratum_list_push(salt_stratum_list *l, const salt_stratum *s) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_stratum *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *s;
  return SALT_OK;
}

void salt_exposed_free_fields(salt_exposed *e) {
  free(e->alias);
  free(e->stratum);
  free(e->command);
  free(e->kind);
  free(e->shim_path);
  memset(e, 0, sizeof(*e));
}

void salt_exposed_list_init(salt_exposed_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_exposed_list_free(salt_exposed_list *l) {
  for (size_t i = 0; i < l->len; i++) salt_exposed_free_fields(&l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int exposed_list_push(salt_exposed_list *l, const salt_exposed *e) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_exposed *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *e;
  return SALT_OK;
}

void salt_provider_free_fields(salt_provider *p) {
  free(p->component);
  free(p->provider);
  free(p->source);
  free(p->trust);
  memset(p, 0, sizeof(*p));
}

void salt_provider_list_init(salt_provider_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_provider_list_free(salt_provider_list *l) {
  for (size_t i = 0; i < l->len; i++) salt_provider_free_fields(&l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int provider_list_push(salt_provider_list *l, const salt_provider *p) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_provider *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *p;
  return SALT_OK;
}

void salt_stratum_snapshot_free_fields(salt_stratum_snapshot *s) {
  free(s->stratum);
  free(s->label);
  free(s->path);
  free(s->kind);
  memset(s, 0, sizeof(*s));
}

void salt_stratum_snapshot_list_init(salt_stratum_snapshot_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_stratum_snapshot_list_free(salt_stratum_snapshot_list *l) {
  for (size_t i = 0; i < l->len; i++) salt_stratum_snapshot_free_fields(&l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int snapshot_list_push(salt_stratum_snapshot_list *l, const salt_stratum_snapshot *s) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_stratum_snapshot *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *s;
  return SALT_OK;
}

void salt_stratum_recipe_init(salt_stratum_recipe *r) {
  memset(r, 0, sizeof(*r));
  salt_strlist_init(&r->repo_names);
  salt_strlist_init(&r->repo_urls);
}

void salt_stratum_recipe_free(salt_stratum_recipe *r) {
  free(r->name);
  free(r->family);
  free(r->arch);
  free(r->root);
  free(r->package_manager);
  free(r->bootstrap);
  free(r->trust);
  free(r->rootfs_url);
  free(r->rootfs_sha256);
  salt_strlist_free(&r->repo_names);
  salt_strlist_free(&r->repo_urls);
  memset(r, 0, sizeof(*r));
}

int salt_stratum_recipe_load(const char *path, salt_stratum_recipe *out) {
  salt_stratum_recipe_init(out);
  salt_toml *t = salt_toml_parse_file(path);
  if (!t) {
    salt_set_error("recipe parse: %s", path);
    return SALT_ERR_FORMAT;
  }
  out->name = salt_strdup(salt_toml_string(t, "name", ""));
  out->family = salt_strdup(salt_toml_string(t, "family", ""));
  out->arch = salt_strdup(salt_toml_string(t, "arch", "x86_64"));
  out->root = salt_strdup(salt_toml_string(t, "root", ""));
  out->package_manager = salt_strdup(salt_toml_string(t, "package_manager", ""));
  out->trust = salt_strdup(salt_toml_string(t, "trust", ""));
  out->bootstrap = salt_strdup(salt_toml_string(t, "bootstrap.method", ""));
  out->rootfs_url = salt_strdup(salt_toml_string(t, "bootstrap.url", ""));
  out->rootfs_sha256 = salt_strdup(salt_toml_string(t, "bootstrap.sha256", ""));
  out->rootfs_strip = (int)salt_toml_int(t, "bootstrap.strip", 0);
  out->graphics = salt_toml_bool(t, "integration.graphics", false);
  out->audio = salt_toml_bool(t, "integration.audio", false);
  out->dbus = salt_toml_bool(t, "integration.dbus", false);
  const salt_toml *repos = salt_toml_get(t, "repository");
  size_t n = salt_toml_array_len(repos);
  for (size_t i = 0; i < n; i++) {
    const salt_toml *r = salt_toml_array_at(repos, i);
    salt_strlist_push(&out->repo_names, salt_toml_string(r, "name", ""));
    salt_strlist_push(&out->repo_urls, salt_toml_string(r, "url", ""));
  }
  salt_toml_free(t);
  return SALT_OK;
}

int salt_stratum_recipe_lint(const char *path, salt_buf *report) {
  salt_stratum_recipe r;
  if (salt_stratum_recipe_load(path, &r) != SALT_OK) {
    salt_buf_append_str(report, "recipe could not be parsed\n");
    return SALT_ERR;
  }
  int problems = 0;
  if (!r.name || !r.name[0]) {
    salt_buf_append_str(report, "missing name\n");
    problems++;
  }
  if (!r.family || !r.family[0]) {
    salt_buf_append_str(report, "missing family\n");
    problems++;
  }
  if (!r.arch || !r.arch[0]) {
    salt_buf_append_str(report, "missing arch\n");
    problems++;
  }
  if (!r.root || !r.root[0]) {
    salt_buf_append_str(report, "missing root\n");
    problems++;
  }
  if (!r.package_manager || !r.package_manager[0]) {
    salt_buf_append_str(report, "missing package_manager\n");
    problems++;
  }
  if (!r.bootstrap || !r.bootstrap[0]) {
    salt_buf_append_str(report, "missing bootstrap method\n");
    problems++;
  }
  salt_stratum_recipe_free(&r);
  return problems == 0 ? SALT_OK : SALT_ERR;
}

static char *stratum_target(const salt_strata_ctx *c, const char *recipe_root) {
  if (c->root && strcmp(c->root, "/") == 0) return salt_strdup(recipe_root);
  return salt_join_path(c->root, recipe_root);
}

static bool command_exists(const char *cmd) {
  salt_buf b;
  salt_buf_init(&b);
  salt_buf_printf(&b, "command -v '%s' >/dev/null 2>&1", cmd);
  int rc = system(b.data);
  salt_buf_free(&b);
  return rc == 0;
}

static bool dir_nonempty(const char *path) {
  DIR *d = opendir(path);
  if (!d) return false;
  struct dirent *de;
  bool any = false;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    any = true;
    break;
  }
  closedir(d);
  return any;
}

int salt_strata_ctx_init(salt_strata_ctx *c, const char *root) {
  memset(c, 0, sizeof(*c));
  c->root = salt_strdup(root);
  if (strcmp(root, "/") == 0)
    c->strata_root = salt_strdup("/strata");
  else
    c->strata_root = salt_join_path(root, "strata");
  c->cache_dir = salt_join_path(root, "var/lib/salt/cache/strata");
  c->use_btrfs = false;
  if (command_exists("btrfs")) {
    salt_buf b;
    salt_buf_init(&b);
    salt_buf_printf(&b, "btrfs filesystem df '%s' >/dev/null 2>&1", c->strata_root);
    if (system(b.data) == 0) c->use_btrfs = true;
    salt_buf_free(&b);
  }
  salt_mkdirs(c->cache_dir, 0755);
  salt_mkdirs(c->strata_root, 0755);
  return SALT_OK;
}

void salt_strata_ctx_free(salt_strata_ctx *c) {
  if (!c) return;
  free(c->root);
  free(c->strata_root);
  free(c->cache_dir);
  memset(c, 0, sizeof(*c));
}

int salt_strata_db_open(const char *root, salt_strata_db **out) {
  char *dbpath = salt_join_path(root, "var/lib/salt/strata.sqlite");
  char *dup = salt_strdup(dbpath);
  char *slash = strrchr(dup, '/');
  if (slash) {
    *slash = '\0';
    salt_mkdirs(dup, 0755);
  }
  free(dup);
  salt_strata_db *db = calloc(1, sizeof(*db));
  if (!db) {
    free(dbpath);
    return SALT_ERR;
  }
  if (sqlite3_open(dbpath, &db->h) != SQLITE_OK) {
    salt_set_error("strata db open %s: %s", dbpath, sqlite3_errmsg(db->h));
    sqlite3_close(db->h);
    free(db);
    free(dbpath);
    return SALT_ERR_IO;
  }
  free(dbpath);
  sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;PRAGMA foreign_keys=OFF;", NULL, NULL, NULL);
  char *err = NULL;
  if (sqlite3_exec(db->h, STRATA_SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
    salt_set_error("strata db schema: %s", err ? err : "?");
    sqlite3_free(err);
    sqlite3_close(db->h);
    free(db);
    return SALT_ERR;
  }
  *out = db;
  return SALT_OK;
}

void salt_strata_db_close(salt_strata_db *db) {
  if (!db) return;
  sqlite3_close(db->h);
  free(db);
}

int salt_stratum_register(salt_strata_db *db, const salt_stratum_recipe *r) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db->h,
                         "INSERT OR REPLACE INTO strata(name,family,arch,root,package_manager,"
                         "bootstrap,trust,state,graphics,audio,dbus,created) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
                         -1, &st, NULL) != SQLITE_OK) {
    salt_set_error("register prepare: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  sqlite3_bind_text(st, 1, r->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, r->family ? r->family : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, r->arch ? r->arch : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, r->root ? r->root : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, r->package_manager ? r->package_manager : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, r->bootstrap ? r->bootstrap : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, r->trust ? r->trust : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, "active", -1, SQLITE_STATIC);
  sqlite3_bind_int(st, 9, r->graphics ? 1 : 0);
  sqlite3_bind_int(st, 10, r->audio ? 1 : 0);
  sqlite3_bind_int(st, 11, r->dbus ? 1 : 0);
  sqlite3_bind_int64(st, 12, (int64_t)time(NULL));
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("register: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  sqlite3_prepare_v2(db->h, "DELETE FROM stratum_repos WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, r->name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  for (size_t i = 0; i < r->repo_names.len; i++) {
    const char *nm = r->repo_names.items[i];
    const char *url = i < r->repo_urls.len ? r->repo_urls.items[i] : "";
    sqlite3_prepare_v2(db->h, "INSERT INTO stratum_repos(stratum,name,url) VALUES(?,?,?);", -1, &st,
                       NULL);
    sqlite3_bind_text(st, 1, r->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, nm, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  return SALT_OK;
}

int salt_stratum_unregister(salt_strata_db *db, const char *name) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "DELETE FROM strata WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM stratum_repos WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return SALT_OK;
}

static void fill_stratum(sqlite3_stmt *st, salt_stratum *s) {
  memset(s, 0, sizeof(*s));
  s->name = salt_strdup((const char *)sqlite3_column_text(st, 0));
  s->family = salt_strdup((const char *)sqlite3_column_text(st, 1));
  s->arch = salt_strdup((const char *)sqlite3_column_text(st, 2));
  s->root = salt_strdup((const char *)sqlite3_column_text(st, 3));
  s->package_manager = salt_strdup((const char *)sqlite3_column_text(st, 4));
  s->bootstrap = salt_strdup((const char *)sqlite3_column_text(st, 5));
  s->trust = salt_strdup((const char *)sqlite3_column_text(st, 6));
  s->state = salt_strdup((const char *)sqlite3_column_text(st, 7));
  s->graphics = sqlite3_column_int(st, 8) != 0;
  s->audio = sqlite3_column_int(st, 9) != 0;
  s->dbus = sqlite3_column_int(st, 10) != 0;
  s->created = sqlite3_column_int64(st, 11);
}

int salt_stratum_get(salt_strata_db *db, const char *name, salt_stratum *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT name,family,arch,root,package_manager,bootstrap,trust,state,graphics,"
                     "audio,dbus,created FROM strata WHERE name=?;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    fill_stratum(st, out);
    sqlite3_finalize(st);
    return SALT_OK;
  }
  sqlite3_finalize(st);
  return SALT_ERR_NOTFOUND;
}

int salt_stratum_list_all(salt_strata_db *db, salt_stratum_list *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT name,family,arch,root,package_manager,bootstrap,trust,state,graphics,"
                     "audio,dbus,created FROM strata ORDER BY name;",
                     -1, &st, NULL);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_stratum s;
    fill_stratum(st, &s);
    stratum_list_push(out, &s);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_stratum_set_state(salt_strata_db *db, const char *name, const char *state) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "UPDATE strata SET state=? WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SALT_OK : SALT_ERR;
}

static int run_system(const char *cmd) {
  int rc = system(cmd);
  if (rc != 0) return SALT_ERR;
  return SALT_OK;
}

static int extract_rootfs(const char *url, const char *archive, const char *target, int strip) {
  const char *decomp = "tar -xpf";
  size_t l = strlen(url);
  if (l >= 4 && strcmp(url + l - 4, ".zst") == 0)
    decomp = "tar --zstd -xpf";
  else if (l >= 3 && strcmp(url + l - 3, ".xz") == 0)
    decomp = "tar -Jxpf";
  else if (l >= 3 && strcmp(url + l - 3, ".gz") == 0)
    decomp = "tar -zxpf";
  else if (l >= 4 && strcmp(url + l - 4, ".tgz") == 0)
    decomp = "tar -zxpf";
  salt_buf cmd;
  salt_buf_init(&cmd);
  salt_buf_printf(&cmd, "%s '%s' -C '%s'", decomp, archive, target);
  if (strip > 0) salt_buf_printf(&cmd, " --strip-components=%d", strip);
  int rc = run_system(cmd.data);
  salt_buf_free(&cmd);
  return rc;
}

static void write_resolv_conf(const char *target) {
  char *etc = salt_join_path(target, "etc");
  salt_mkdirs(etc, 0755);
  free(etc);
  char *dst = salt_join_path(target, "etc/resolv.conf");
  salt_copy_file("/etc/resolv.conf", dst);
  free(dst);
}

static void write_repo_config(const salt_stratum_recipe *r, const char *target) {
  if (!r->family) return;
  if (strcmp(r->family, "arch") == 0) {
    char *dir = salt_join_path(target, "etc/pacman.d");
    salt_mkdirs(dir, 0755);
    free(dir);
    salt_buf b;
    salt_buf_init(&b);
    for (size_t i = 0; i < r->repo_urls.len; i++)
      salt_buf_printf(&b, "Server = %s\n", r->repo_urls.items[i]);
    char *path = salt_join_path(target, "etc/pacman.d/mirrorlist");
    salt_write_file(path, b.data, b.len, 0644);
    free(path);
    salt_buf_free(&b);
  } else if (strcmp(r->family, "debian") == 0) {
    char *dir = salt_join_path(target, "etc/apt");
    salt_mkdirs(dir, 0755);
    free(dir);
    salt_buf b;
    salt_buf_init(&b);
    for (size_t i = 0; i < r->repo_urls.len; i++) {
      const char *nm = i < r->repo_names.len ? r->repo_names.items[i] : "stable";
      salt_buf_printf(&b, "deb %s %s main\n", r->repo_urls.items[i], nm);
    }
    char *path = salt_join_path(target, "etc/apt/sources.list");
    salt_write_file(path, b.data, b.len, 0644);
    free(path);
    salt_buf_free(&b);
  } else if (strcmp(r->family, "void") == 0) {
    char *dir = salt_join_path(target, "etc/xbps.d");
    salt_mkdirs(dir, 0755);
    free(dir);
    salt_buf b;
    salt_buf_init(&b);
    for (size_t i = 0; i < r->repo_urls.len; i++)
      salt_buf_printf(&b, "repository=%s\n", r->repo_urls.items[i]);
    char *path = salt_join_path(target, "etc/xbps.d/00-repository-main.conf");
    salt_write_file(path, b.data, b.len, 0644);
    free(path);
    salt_buf_free(&b);
  }
}

int salt_stratum_bootstrap(const salt_strata_ctx *c, salt_strata_db *db,
                           const salt_stratum_recipe *r) {
  char *target = stratum_target(c, r->root);
  if (salt_is_dir(target) && dir_nonempty(target)) {
    salt_set_error("stratum target already exists: %s", target);
    free(target);
    return SALT_ERR_EXISTS;
  }
  int made = SALT_ERR;
  if (c->use_btrfs) {
    salt_buf b;
    salt_buf_init(&b);
    salt_buf_printf(&b, "btrfs subvolume create '%s'", target);
    made = run_system(b.data);
    salt_buf_free(&b);
  }
  if (made != SALT_OK) {
    if (salt_mkdirs(target, 0755) != SALT_OK) {
      salt_set_error("cannot create stratum target: %s", target);
      free(target);
      return SALT_ERR_IO;
    }
  }

  if (r->bootstrap && strcmp(r->bootstrap, "oci") == 0) {
    if (!r->rootfs_url || !r->rootfs_url[0]) {
      salt_set_error("oci bootstrap needs an image reference in bootstrap.url");
      free(target);
      return SALT_ERR;
    }
    const char *runtime = command_exists("docker") ? "docker"
                          : command_exists("podman") ? "podman"
                                                     : NULL;
    if (!runtime) {
      salt_set_error("oci bootstrap needs docker or podman on the host");
      free(target);
      return SALT_ERR;
    }
    salt_buf cachebuf;
    salt_buf_init(&cachebuf);
    salt_buf_printf(&cachebuf, "%s.oci.tar", r->name);
    char *archive = salt_join_path(c->cache_dir, cachebuf.data);
    salt_buf_free(&cachebuf);
    salt_buf cmd;
    salt_buf_init(&cmd);
    salt_buf_printf(&cmd,
                    "set -e; %s pull '%s'; cid=$(%s create '%s'); "
                    "%s export \"$cid\" -o '%s'; %s rm \"$cid\" >/dev/null 2>&1",
                    runtime, r->rootfs_url, runtime, r->rootfs_url, runtime, archive, runtime);
    int rc = run_system(cmd.data);
    salt_buf_free(&cmd);
    if (rc != SALT_OK) {
      salt_set_error("oci export failed for image %s", r->rootfs_url);
      free(archive);
      free(target);
      return SALT_ERR;
    }
    if (extract_rootfs("rootfs.tar", archive, target, 0) != SALT_OK) {
      salt_set_error("rootfs extract failed for %s", r->name);
      free(archive);
      free(target);
      return SALT_ERR;
    }
    free(archive);
  } else if (r->rootfs_url && r->rootfs_url[0]) {
    salt_buf cachebuf;
    salt_buf_init(&cachebuf);
    salt_buf_printf(&cachebuf, "%s.rootfs", r->name);
    char *archive = salt_join_path(c->cache_dir, cachebuf.data);
    salt_buf_free(&cachebuf);
    if (salt_fetch_to_file(r->rootfs_url, archive) != SALT_OK) {
      salt_set_error("rootfs download failed: %s", r->rootfs_url);
      free(archive);
      free(target);
      return SALT_ERR_IO;
    }
    if (r->rootfs_sha256 && r->rootfs_sha256[0]) {
      char hex[SALT_SHA256_HEXLEN + 1];
      if (salt_sha256_file(archive, hex) != SALT_OK || strcmp(hex, r->rootfs_sha256) != 0) {
        salt_set_error("rootfs sha256 mismatch for %s", r->name);
        free(archive);
        free(target);
        return SALT_ERR_VERIFY;
      }
    }
    if (extract_rootfs(r->rootfs_url, archive, target, r->rootfs_strip) != SALT_OK) {
      salt_set_error("rootfs extract failed for %s", r->name);
      free(archive);
      free(target);
      return SALT_ERR;
    }
    free(archive);
  } else if (r->bootstrap && strcmp(r->bootstrap, "debootstrap") == 0) {
    if (!command_exists("debootstrap")) {
      salt_set_error("debootstrap not available");
      free(target);
      return SALT_ERR;
    }
    const char *suite = r->repo_names.len > 0 ? r->repo_names.items[0] : "stable";
    const char *mirror = r->repo_urls.len > 0 ? r->repo_urls.items[0] : "";
    const char *deb_arch = "amd64";
    if (r->arch && strcmp(r->arch, "aarch64") == 0) deb_arch = "arm64";
    else if (r->arch && strcmp(r->arch, "x86_64") != 0 && r->arch[0]) deb_arch = r->arch;
    salt_buf b;
    salt_buf_init(&b);
    salt_buf_printf(&b, "debootstrap --arch='%s' '%s' '%s' '%s'", deb_arch, suite, target, mirror);
    int rc = run_system(b.data);
    salt_buf_free(&b);
    if (rc != SALT_OK) {
      salt_set_error("debootstrap failed for %s", r->name);
      free(target);
      return SALT_ERR;
    }
  }

  write_resolv_conf(target);
  write_repo_config(r, target);

  int rc = salt_stratum_register(db, r);
  if (rc != SALT_OK) {
    free(target);
    return rc;
  }
  salt_stratum_snapshot_create(c, db, r->name, "bootstrap", NULL);
  free(target);
  return SALT_OK;
}

static int snapshot_timestamp(char *out, size_t n) {
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  if (strftime(out, n, "%Y%m%dT%H%M%SZ", &tmv) == 0) return SALT_ERR;
  return SALT_OK;
}

int salt_stratum_snapshot_create(const salt_strata_ctx *c, salt_strata_db *db, const char *name,
                                 const char *label, int64_t *id_out) {
  salt_stratum s;
  if (salt_stratum_get(db, name, &s) != SALT_OK) {
    salt_set_error("snapshot: unknown stratum %s", name);
    return SALT_ERR_NOTFOUND;
  }
  char *target = stratum_target(c, s.root);
  salt_stratum_free_fields(&s);

  salt_buf snapdir_buf;
  salt_buf_init(&snapdir_buf);
  salt_buf_printf(&snapdir_buf, ".snapshots/%s", name);
  char *snapdir = salt_join_path(c->strata_root, snapdir_buf.data);
  salt_buf_free(&snapdir_buf);
  salt_mkdirs(snapdir, 0755);

  char ts[32];
  if (snapshot_timestamp(ts, sizeof(ts)) != SALT_OK) {
    free(target);
    free(snapdir);
    return SALT_ERR;
  }

  char *path = NULL;
  const char *kind = NULL;
  int rc;
  if (c->use_btrfs) {
    salt_buf nm;
    salt_buf_init(&nm);
    salt_buf_printf(&nm, "%s-%s", label, ts);
    path = salt_join_path(snapdir, nm.data);
    salt_buf_free(&nm);
    kind = "btrfs";
    salt_buf cmd;
    salt_buf_init(&cmd);
    salt_buf_printf(&cmd, "btrfs subvolume snapshot -r '%s' '%s'", target, path);
    rc = run_system(cmd.data);
    salt_buf_free(&cmd);
  } else {
    salt_buf nm;
    salt_buf_init(&nm);
    salt_buf_printf(&nm, "%s-%s.tar", label, ts);
    path = salt_join_path(snapdir, nm.data);
    salt_buf_free(&nm);
    kind = "tar";
    salt_buf cmd;
    salt_buf_init(&cmd);
    salt_buf_printf(&cmd, "tar -cpf '%s' -C '%s' .", path, target);
    rc = run_system(cmd.data);
    salt_buf_free(&cmd);
  }
  free(target);
  free(snapdir);
  if (rc != SALT_OK) {
    salt_set_error("snapshot create failed for %s", name);
    free(path);
    return SALT_ERR;
  }

  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "INSERT INTO stratum_snapshots(stratum,label,path,kind,time) "
                     "VALUES(?,?,?,?,?);",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, label, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, kind, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, (int64_t)time(NULL));
  int sr6 = sqlite3_step(st);
  sqlite3_finalize(st);
  free(path);
  if (sr6 != SQLITE_DONE) {
    salt_set_error("snapshot record failed: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  if (id_out) *id_out = sqlite3_last_insert_rowid(db->h);
  return SALT_OK;
}

static void fill_snapshot(sqlite3_stmt *st, salt_stratum_snapshot *s) {
  memset(s, 0, sizeof(*s));
  s->id = sqlite3_column_int64(st, 0);
  s->stratum = salt_strdup((const char *)sqlite3_column_text(st, 1));
  s->label = salt_strdup((const char *)sqlite3_column_text(st, 2));
  s->path = salt_strdup((const char *)sqlite3_column_text(st, 3));
  s->kind = salt_strdup((const char *)sqlite3_column_text(st, 4));
  s->time = sqlite3_column_int64(st, 5);
}

int salt_stratum_snapshot_list_all(salt_strata_db *db, const char *name,
                                   salt_stratum_snapshot_list *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT id,stratum,label,path,kind,time FROM stratum_snapshots "
                     "WHERE stratum=? ORDER BY id DESC;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_stratum_snapshot s;
    fill_snapshot(st, &s);
    snapshot_list_push(out, &s);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_stratum_rollback(const salt_strata_ctx *c, salt_strata_db *db, const char *name,
                          int64_t snap_id) {
  salt_stratum s;
  if (salt_stratum_get(db, name, &s) != SALT_OK) {
    salt_set_error("rollback: unknown stratum %s", name);
    return SALT_ERR_NOTFOUND;
  }
  char *target = stratum_target(c, s.root);
  salt_stratum_free_fields(&s);

  sqlite3_stmt *st;
  if (snap_id > 0) {
    sqlite3_prepare_v2(db->h,
                       "SELECT id,stratum,label,path,kind,time FROM stratum_snapshots "
                       "WHERE stratum=? AND id=?;",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, snap_id);
  } else {
    sqlite3_prepare_v2(db->h,
                       "SELECT id,stratum,label,path,kind,time FROM stratum_snapshots "
                       "WHERE stratum=? ORDER BY id DESC LIMIT 1;",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  }
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    salt_set_error("rollback: no snapshot found for %s", name);
    free(target);
    return SALT_ERR_NOTFOUND;
  }
  salt_stratum_snapshot snap;
  fill_snapshot(st, &snap);
  sqlite3_finalize(st);

  int rc = SALT_OK;
  if (strcmp(snap.kind, "btrfs") == 0) {
    char ts[32];
    if (snapshot_timestamp(ts, sizeof(ts)) == SALT_OK) {
      salt_buf safedir_buf;
      salt_buf_init(&safedir_buf);
      salt_buf_printf(&safedir_buf, ".snapshots/%s", name);
      char *snapdir = salt_join_path(c->strata_root, safedir_buf.data);
      salt_buf_free(&safedir_buf);
      salt_mkdirs(snapdir, 0755);
      salt_buf safe;
      salt_buf_init(&safe);
      salt_buf_printf(&safe, "%s/prerollback-%s", snapdir, ts);
      salt_buf cmd;
      salt_buf_init(&cmd);
      salt_buf_printf(&cmd, "btrfs subvolume snapshot -r '%s' '%s'", target, safe.data);
      system(cmd.data);
      salt_buf_free(&cmd);
      salt_buf_free(&safe);
      free(snapdir);
    }
    salt_buf del;
    salt_buf_init(&del);
    salt_buf_printf(&del, "btrfs subvolume delete '%s'", target);
    system(del.data);
    salt_buf_free(&del);
    salt_buf cre;
    salt_buf_init(&cre);
    salt_buf_printf(&cre, "btrfs subvolume snapshot '%s' '%s'", snap.path, target);
    rc = run_system(cre.data);
    salt_buf_free(&cre);
  } else {
    salt_buf clr;
    salt_buf_init(&clr);
    salt_buf_printf(&clr, "find '%s' -mindepth 1 -delete", target);
    system(clr.data);
    salt_buf_free(&clr);
    salt_mkdirs(target, 0755);
    salt_buf ext;
    salt_buf_init(&ext);
    salt_buf_printf(&ext, "tar -xpf '%s' -C '%s'", snap.path, target);
    rc = run_system(ext.data);
    salt_buf_free(&ext);
  }
  salt_stratum_snapshot_free_fields(&snap);
  free(target);
  if (rc != SALT_OK) {
    salt_set_error("rollback failed for %s", name);
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_stratum_destroy(const salt_strata_ctx *c, salt_strata_db *db, const char *name) {
  salt_stratum s;
  if (salt_stratum_get(db, name, &s) != SALT_OK) {
    salt_set_error("destroy: unknown stratum %s", name);
    return SALT_ERR_NOTFOUND;
  }
  char *target = stratum_target(c, s.root);
  salt_stratum_free_fields(&s);

  if (c->use_btrfs) {
    salt_buf del;
    salt_buf_init(&del);
    salt_buf_printf(&del, "btrfs subvolume delete '%s'", target);
    if (run_system(del.data) != SALT_OK) salt_remove_recursive(target);
    salt_buf_free(&del);
  } else {
    salt_remove_recursive(target);
  }
  free(target);

  salt_buf snapdir_buf;
  salt_buf_init(&snapdir_buf);
  salt_buf_printf(&snapdir_buf, ".snapshots/%s", name);
  char *snapdir = salt_join_path(c->strata_root, snapdir_buf.data);
  salt_buf_free(&snapdir_buf);
  salt_remove_recursive(snapdir);
  free(snapdir);

  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT shim_path FROM exposed WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *sp = (const char *)sqlite3_column_text(st, 0);
    if (sp && sp[0]) remove(sp);
  }
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db->h, "DELETE FROM strata WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM stratum_repos WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM stratum_snapshots WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM exposed WHERE stratum=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return SALT_OK;
}

static bool stratum_exists(salt_strata_db *db, const char *name) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT 1 FROM strata WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

int salt_expose_add(salt_strata_db *db, const char *root, const char *stratum, const char *command,
                    const char *alias, const char *kind) {
  if (!stratum_exists(db, stratum)) {
    salt_set_error("expose: unknown stratum %s", stratum);
    return SALT_ERR_NOTFOUND;
  }
  char *shimdir = salt_join_path(root, "usr/local/salt/shims");
  salt_mkdirs(shimdir, 0755);
  char *shimpath = salt_join_path(shimdir, alias);
  free(shimdir);

  salt_buf content;
  salt_buf_init(&content);
  salt_buf_printf(&content, "#!/bin/sh\nexec salt run %s %s \"$@\"\n", stratum, command);
  int rc = salt_write_file(shimpath, content.data, content.len, 0755);
  salt_buf_free(&content);
  if (rc != SALT_OK) {
    salt_set_error("expose: cannot write shim %s", shimpath);
    free(shimpath);
    return SALT_ERR_IO;
  }

  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "INSERT OR REPLACE INTO exposed(alias,stratum,command,kind,shim_path,created) "
                     "VALUES(?,?,?,?,?,?);",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, alias, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, stratum, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, command, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, kind ? kind : "cli", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, shimpath, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, (int64_t)time(NULL));
  int srr = sqlite3_step(st);
  sqlite3_finalize(st);
  free(shimpath);
  if (srr != SQLITE_DONE) {
    salt_set_error("expose record failed: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_expose_remove(salt_strata_db *db, const char *root, const char *alias) {
  (void)root;
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT shim_path FROM exposed WHERE alias=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, alias, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    salt_set_error("expose: unknown alias %s", alias);
    return SALT_ERR_NOTFOUND;
  }
  const char *sp = (const char *)sqlite3_column_text(st, 0);
  if (sp && sp[0]) remove(sp);
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db->h, "DELETE FROM exposed WHERE alias=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, alias, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return SALT_OK;
}

static void fill_exposed(sqlite3_stmt *st, salt_exposed *e) {
  memset(e, 0, sizeof(*e));
  e->alias = salt_strdup((const char *)sqlite3_column_text(st, 0));
  e->stratum = salt_strdup((const char *)sqlite3_column_text(st, 1));
  e->command = salt_strdup((const char *)sqlite3_column_text(st, 2));
  e->kind = salt_strdup((const char *)sqlite3_column_text(st, 3));
  e->shim_path = salt_strdup((const char *)sqlite3_column_text(st, 4));
  e->created = sqlite3_column_int64(st, 5);
}

int salt_expose_list(salt_strata_db *db, salt_exposed_list *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT alias,stratum,command,kind,shim_path,created FROM exposed "
                     "ORDER BY alias;",
                     -1, &st, NULL);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_exposed e;
    fill_exposed(st, &e);
    exposed_list_push(out, &e);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_expose_desktop(salt_strata_db *db, const salt_stratum *s, const char *root,
                        const char *app) {
  char *base;
  if (root && strcmp(root, "/") != 0)
    base = salt_join_path(root, s->root);
  else
    base = salt_strdup(s->root);
  salt_buf rel;
  salt_buf_init(&rel);
  salt_buf_printf(&rel, "usr/share/applications/%s.desktop", app);
  char *src = salt_join_path(base, rel.data);
  salt_buf_free(&rel);
  free(base);

  salt_buf content;
  if (salt_read_file(src, &content) != SALT_OK) {
    salt_set_error("expose desktop: missing %s", src);
    free(src);
    return SALT_ERR_NOTFOUND;
  }
  free(src);

  salt_buf out;
  salt_buf_init(&out);
  const char *p = content.data;
  size_t remaining = content.len;
  while (remaining > 0) {
    const char *nl = memchr(p, '\n', remaining);
    size_t linelen = nl ? (size_t)(nl - p) + 1 : remaining;
    if (linelen >= 5 && strncmp(p, "Exec=", 5) == 0) {
      salt_buf_printf(&out, "Exec=salt run %s ", s->name);
      salt_buf_append(&out, p + 5, linelen - 5);
    } else {
      salt_buf_append(&out, p, linelen);
    }
    p += linelen;
    remaining -= linelen;
  }
  salt_buf_free(&content);

  char *dstdir = salt_join_path(root, "usr/local/share/applications");
  salt_mkdirs(dstdir, 0755);
  free(dstdir);
  salt_buf dstrel;
  salt_buf_init(&dstrel);
  salt_buf_printf(&dstrel, "usr/local/share/applications/%s-%s.desktop", s->name, app);
  char *dst = salt_join_path(root, dstrel.data);
  salt_buf_free(&dstrel);
  int rc = salt_write_file(dst, out.data, out.len, 0644);
  free(dst);
  salt_buf_free(&out);
  if (rc != SALT_OK) {
    salt_set_error("expose desktop: cannot write entry for %s", app);
    return SALT_ERR_IO;
  }

  salt_expose_add(db, root, s->name, app, app, "cli");
  return SALT_OK;
}

int salt_provider_set(salt_strata_db *db, const char *component, const char *provider,
                      const char *source, const char *trust) {
  int64_t prior = 0;
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT id FROM provider_history WHERE component=? ORDER BY id DESC LIMIT 1;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, component, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) prior = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db->h,
                     "INSERT INTO provider_history(component,provider,source,trust,time) "
                     "VALUES(?,?,?,?,?);",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, component, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, source ? source : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, trust ? trust : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, (int64_t)time(NULL));
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("provider history insert: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }

  sqlite3_prepare_v2(db->h,
                     "INSERT OR REPLACE INTO providers(component,provider,source,trust,"
                     "rollback_target,time) VALUES(?,?,?,?,?,?);",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, component, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, source ? source : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, trust ? trust : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, prior);
  sqlite3_bind_int64(st, 6, (int64_t)time(NULL));
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("provider set: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  return SALT_OK;
}

static void fill_provider(sqlite3_stmt *st, salt_provider *p) {
  memset(p, 0, sizeof(*p));
  p->component = salt_strdup((const char *)sqlite3_column_text(st, 0));
  p->provider = salt_strdup((const char *)sqlite3_column_text(st, 1));
  p->source = salt_strdup((const char *)sqlite3_column_text(st, 2));
  p->trust = salt_strdup((const char *)sqlite3_column_text(st, 3));
  p->rollback_target = sqlite3_column_int64(st, 4);
  p->time = sqlite3_column_int64(st, 5);
}

int salt_provider_get(salt_strata_db *db, const char *component, salt_provider *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT component,provider,source,trust,rollback_target,time FROM providers "
                     "WHERE component=?;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, component, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    fill_provider(st, out);
    sqlite3_finalize(st);
    return SALT_OK;
  }
  sqlite3_finalize(st);
  return SALT_ERR_NOTFOUND;
}

int salt_provider_list_all(salt_strata_db *db, salt_provider_list *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT component,provider,source,trust,rollback_target,time FROM providers "
                     "ORDER BY component;",
                     -1, &st, NULL);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_provider p;
    fill_provider(st, &p);
    provider_list_push(out, &p);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_provider_rollback(salt_strata_db *db, const char *component) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT rollback_target FROM providers WHERE component=?;", -1, &st,
                     NULL);
  sqlite3_bind_text(st, 1, component, -1, SQLITE_TRANSIENT);
  int64_t target = 0;
  if (sqlite3_step(st) == SQLITE_ROW) target = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  if (target <= 0) {
    salt_set_error("no previous provider for %s", component);
    return SALT_ERR;
  }

  sqlite3_prepare_v2(db->h,
                     "SELECT component,provider,source,trust FROM provider_history WHERE id=?;", -1,
                     &st, NULL);
  sqlite3_bind_int64(st, 1, target);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    salt_set_error("provider rollback: history %lld missing", (long long)target);
    return SALT_ERR_NOTFOUND;
  }
  char *hcomp = salt_strdup((const char *)sqlite3_column_text(st, 0));
  char *hprov = salt_strdup((const char *)sqlite3_column_text(st, 1));
  char *hsrc = salt_strdup((const char *)sqlite3_column_text(st, 2));
  char *htrust = salt_strdup((const char *)sqlite3_column_text(st, 3));
  sqlite3_finalize(st);

  int rc = salt_provider_set(db, hcomp, hprov, hsrc, htrust);
  free(hcomp);
  free(hprov);
  free(hsrc);
  free(htrust);
  return rc;
}

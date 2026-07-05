#include "salt/db.h"
#include "salt/txn.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>

struct salt_db {
  sqlite3 *h;
};

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS packages("
    " name TEXT PRIMARY KEY, version TEXT, release INTEGER, arch TEXT,"
    " summary TEXT, license TEXT, repo TEXT, sig_status TEXT,"
    " install_time INTEGER, txn_id INTEGER);"
    "CREATE TABLE IF NOT EXISTS files("
    " name TEXT, path TEXT, mode INTEGER, size INTEGER, sha256 TEXT,"
    " type TEXT, linkname TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_files_name ON files(name);"
    "CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);"
    "CREATE TABLE IF NOT EXISTS deps(name TEXT, dep TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_deps_dep ON deps(dep);"
    "CREATE TABLE IF NOT EXISTS transactions("
    " id INTEGER PRIMARY KEY AUTOINCREMENT, op TEXT, status TEXT,"
    " time INTEGER, snapshot TEXT);";

void salt_db_pkglist_init(salt_db_pkglist *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_db_pkg_free_fields(salt_db_pkg *p) {
  free(p->name);
  free(p->version);
  free(p->arch);
  free(p->repo);
  free(p->sig_status);
  memset(p, 0, sizeof(*p));
}

void salt_db_pkglist_free(salt_db_pkglist *l) {
  for (size_t i = 0; i < l->len; i++) salt_db_pkg_free_fields(&l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int pkglist_push(salt_db_pkglist *l, const salt_db_pkg *p) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_db_pkg *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *p;
  return SALT_OK;
}

int salt_db_open(const char *path, salt_db **out) {
  char *dup = salt_strdup(path);
  char *slash = strrchr(dup, '/');
  if (slash) {
    *slash = '\0';
    salt_mkdirs(dup, 0755);
  }
  free(dup);
  salt_db *db = calloc(1, sizeof(*db));
  if (!db) return SALT_ERR;
  if (sqlite3_open(path, &db->h) != SQLITE_OK) {
    salt_set_error("db open %s: %s", path, sqlite3_errmsg(db->h));
    sqlite3_close(db->h);
    free(db);
    return SALT_ERR_IO;
  }
  sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;PRAGMA foreign_keys=OFF;", NULL, NULL, NULL);
  char *err = NULL;
  if (sqlite3_exec(db->h, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
    salt_set_error("db schema: %s", err ? err : "?");
    sqlite3_free(err);
    sqlite3_close(db->h);
    free(db);
    return SALT_ERR;
  }
  *out = db;
  return SALT_OK;
}

void salt_db_close(salt_db *db) {
  if (!db) return;
  sqlite3_close(db->h);
  free(db);
}

static int exec(salt_db *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(db->h, sql, NULL, NULL, &err) != SQLITE_OK) {
    salt_set_error("db: %s", err ? err : "?");
    sqlite3_free(err);
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_db_sql_begin(salt_db *db) {
  return exec(db, "BEGIN IMMEDIATE;");
}
int salt_db_sql_commit(salt_db *db) {
  return exec(db, "COMMIT;");
}
int salt_db_sql_rollback(salt_db *db) {
  return exec(db, "ROLLBACK;");
}

int salt_db_txn_new(salt_db *db, const char *op, int64_t *txn_id_out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db->h, "INSERT INTO transactions(op,status,time,snapshot) VALUES(?,?,?,?);",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_text(st, 1, op, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, "started", -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 3, (int64_t)time(NULL));
  sqlite3_bind_null(st, 4);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("db txn_new: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  *txn_id_out = sqlite3_last_insert_rowid(db->h);
  return SALT_OK;
}

int salt_db_txn_finish(salt_db *db, int64_t txn_id, const char *status) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db->h, "UPDATE transactions SET status=? WHERE id=?;", -1, &st, NULL) !=
      SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_text(st, 1, status, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, txn_id);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SALT_OK : SALT_ERR;
}

int salt_db_txn_set_snapshot(salt_db *db, int64_t txn_id, const char *snapshot) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db->h, "UPDATE transactions SET snapshot=? WHERE id=?;", -1, &st, NULL) !=
      SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_text(st, 1, snapshot, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, txn_id);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SALT_OK : SALT_ERR;
}

int salt_db_record_install(salt_db *db, const salt_pkg_meta *meta, const salt_manifest *manifest,
                           const char *repo, const char *sig_status, int64_t txn_id) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "DELETE FROM packages WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM files WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM deps WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db->h,
                     "INSERT INTO packages(name,version,release,arch,summary,license,repo,"
                     "sig_status,install_time,txn_id) VALUES(?,?,?,?,?,?,?,?,?,?);",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, meta->version, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, meta->release);
  sqlite3_bind_text(st, 4, meta->arch, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, meta->summary ? meta->summary : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, meta->license ? meta->license : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, repo ? repo : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, sig_status ? sig_status : "unknown", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 9, (int64_t)time(NULL));
  sqlite3_bind_int64(st, 10, txn_id);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("db install pkg: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }

  for (size_t i = 0; i < manifest->len; i++) {
    const salt_manifest_entry *e = &manifest->items[i];
    char tf[2] = {e->typeflag ? e->typeflag : '0', 0};
    sqlite3_prepare_v2(db->h,
                       "INSERT INTO files(name,path,mode,size,sha256,type,linkname) "
                       "VALUES(?,?,?,?,?,?,?);",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, e->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, (int)e->mode);
    sqlite3_bind_int64(st, 4, (int64_t)e->size);
    sqlite3_bind_text(st, 5, e->sha256 ? e->sha256 : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, tf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, e->linkname ? e->linkname : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  for (size_t i = 0; i < meta->deps.len; i++) {
    sqlite3_prepare_v2(db->h, "INSERT INTO deps(name,dep) VALUES(?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, meta->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, meta->deps.items[i], -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  return SALT_OK;
}

int salt_db_record_remove(salt_db *db, const char *name, int64_t txn_id) {
  (void)txn_id;
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "DELETE FROM files WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM deps WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db->h, "DELETE FROM packages WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SALT_OK : SALT_ERR;
}

static void fill_pkg(sqlite3_stmt *st, salt_db_pkg *p) {
  memset(p, 0, sizeof(*p));
  p->name = salt_strdup((const char *)sqlite3_column_text(st, 0));
  p->version = salt_strdup((const char *)sqlite3_column_text(st, 1));
  p->release = sqlite3_column_int(st, 2);
  p->arch = salt_strdup((const char *)sqlite3_column_text(st, 3));
  p->repo = salt_strdup((const char *)sqlite3_column_text(st, 4));
  p->sig_status = salt_strdup((const char *)sqlite3_column_text(st, 5));
  p->install_time = sqlite3_column_int64(st, 6);
  p->txn_id = sqlite3_column_int64(st, 7);
}

int salt_db_get_pkg(salt_db *db, const char *name, salt_db_pkg *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT name,version,release,arch,repo,sig_status,install_time,txn_id "
                     "FROM packages WHERE name=?;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    fill_pkg(st, out);
    sqlite3_finalize(st);
    return SALT_OK;
  }
  sqlite3_finalize(st);
  return SALT_ERR_NOTFOUND;
}

bool salt_db_is_installed(salt_db *db, const char *name) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT 1 FROM packages WHERE name=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

int salt_db_list_installed(salt_db *db, salt_db_pkglist *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT name,version,release,arch,repo,sig_status,install_time,txn_id "
                     "FROM packages ORDER BY name;",
                     -1, &st, NULL);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_db_pkg p;
    fill_pkg(st, &p);
    pkglist_push(out, &p);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_db_search(salt_db *db, const char *term, salt_db_pkglist *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT name,version,release,arch,repo,sig_status,install_time,txn_id "
                     "FROM packages WHERE name LIKE ?1 OR summary LIKE ?1 ORDER BY name;",
                     -1, &st, NULL);
  salt_buf like;
  salt_buf_init(&like);
  salt_buf_printf(&like, "%%%s%%", term);
  sqlite3_bind_text(st, 1, like.data, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_db_pkg p;
    fill_pkg(st, &p);
    pkglist_push(out, &p);
  }
  sqlite3_finalize(st);
  salt_buf_free(&like);
  return SALT_OK;
}

int salt_db_pkg_files(salt_db *db, const char *name, salt_strlist *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT path FROM files WHERE name=? ORDER BY path;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW)
    salt_strlist_push(out, (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_db_owner(salt_db *db, const char *path, char **owner_out) {
  const char *q = path;
  while (*q == '/') q++;
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT name FROM files WHERE path=? OR path=? LIMIT 1;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, q, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *owner_out = salt_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return SALT_OK;
  }
  sqlite3_finalize(st);
  return SALT_ERR_NOTFOUND;
}

int salt_db_pkg_manifest(salt_db *db, const char *name, salt_manifest *out) {
  salt_manifest_init(out);
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT path,mode,size,sha256,type,linkname FROM files WHERE name=? "
                     "ORDER BY length(path) DESC;",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_manifest_entry e;
    memset(&e, 0, sizeof(e));
    e.path = (char *)sqlite3_column_text(st, 0);
    e.mode = (unsigned)sqlite3_column_int(st, 1);
    e.size = (uint64_t)sqlite3_column_int64(st, 2);
    e.sha256 = (char *)sqlite3_column_text(st, 3);
    const char *tf = (const char *)sqlite3_column_text(st, 4);
    e.typeflag = tf && tf[0] ? tf[0] : '0';
    e.linkname = (char *)sqlite3_column_text(st, 5);
    salt_manifest_push(out, &e);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_db_vacuum_into(salt_db *db, const char *path) {
  unlink(path);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db->h, "VACUUM INTO ?;", -1, &st, NULL) != SQLITE_OK) {
    salt_set_error("db vacuum prepare: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    salt_set_error("db vacuum: %s", sqlite3_errmsg(db->h));
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_db_restore_state_from(salt_db *db, const char *before_path) {
  salt_buf sql;
  salt_buf_init(&sql);
  salt_buf_printf(&sql,
                  "BEGIN IMMEDIATE;"
                  "DELETE FROM packages;DELETE FROM files;DELETE FROM deps;"
                  "ATTACH '%s' AS before;"
                  "INSERT INTO packages SELECT * FROM before.packages;"
                  "INSERT INTO files SELECT * FROM before.files;"
                  "INSERT INTO deps SELECT * FROM before.deps;"
                  "DETACH before;"
                  "COMMIT;",
                  before_path);
  int rc = exec(db, sql.data);
  salt_buf_free(&sql);
  return rc;
}

void salt_deployment_list_init(salt_deployment_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_deployment_list_free(salt_deployment_list *l) {
  for (size_t i = 0; i < l->len; i++) {
    free(l->items[i].op);
    free(l->items[i].status);
    free(l->items[i].snapshot);
  }
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int deployment_push(salt_deployment_list *l, salt_deployment *d) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_deployment *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *d;
  return SALT_OK;
}

int salt_db_deployments(salt_db *db, salt_deployment_list *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT id,op,status,time,snapshot FROM transactions ORDER BY id DESC;",
                     -1, &st, NULL);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_deployment d;
    memset(&d, 0, sizeof(d));
    d.id = sqlite3_column_int64(st, 0);
    d.op = salt_strdup((const char *)sqlite3_column_text(st, 1));
    d.status = salt_strdup((const char *)sqlite3_column_text(st, 2));
    d.time = sqlite3_column_int64(st, 3);
    const char *snap = (const char *)sqlite3_column_text(st, 4);
    d.snapshot = snap ? salt_strdup(snap) : NULL;
    deployment_push(out, &d);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_db_last_ok_txn(salt_db *db, int64_t *id_out, char **snapshot_out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h,
                     "SELECT id,snapshot FROM transactions WHERE status='ok' "
                     "ORDER BY id DESC LIMIT 1;",
                     -1, &st, NULL);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *id_out = sqlite3_column_int64(st, 0);
    const char *snap = (const char *)sqlite3_column_text(st, 1);
    if (snapshot_out) *snapshot_out = snap ? salt_strdup(snap) : NULL;
    sqlite3_finalize(st);
    return SALT_OK;
  }
  sqlite3_finalize(st);
  return SALT_ERR_NOTFOUND;
}

int salt_db_revdeps(salt_db *db, const char *name, salt_strlist *out) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db->h, "SELECT DISTINCT name FROM deps WHERE dep=? ORDER BY name;", -1, &st,
                     NULL);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW)
    salt_strlist_push(out, (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  return SALT_OK;
}

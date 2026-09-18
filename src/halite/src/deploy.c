#define _GNU_SOURCE
#include "salt/deploy.h"
#include "salt/repo.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sqlite3.h>

static const char *DEPLOY_SCHEMA =
    "CREATE TABLE IF NOT EXISTS txn_changes("
    " txn_id INTEGER, name TEXT, old_version TEXT, new_version TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_txn_changes_txn ON txn_changes(txn_id);"
    "CREATE TABLE IF NOT EXISTS txn_meta("
    " txn_id INTEGER PRIMARY KEY, kernel TEXT, pinned INTEGER DEFAULT 0);";

void salt_txn_change_list_init(salt_txn_change_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void salt_txn_change_list_free(salt_txn_change_list *l) {
  for (size_t i = 0; i < l->len; i++) {
    free(l->items[i].name);
    free(l->items[i].old_version);
    free(l->items[i].new_version);
  }
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static int change_push(salt_txn_change_list *l, salt_txn_change *c) {
  if (l->len == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    salt_txn_change *ni = realloc(l->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->len++] = *c;
  return SALT_OK;
}

void salt_txn_meta_free(salt_txn_meta *m) {
  free(m->kernel);
  m->kernel = NULL;
  m->pinned = false;
}

void salt_btrfs_layout_free(salt_btrfs_layout *l) {
  free(l->toplevel);
  free(l->root_subvol);
  free(l->snapshots_subvol);
  free(l->device);
  memset(l, 0, sizeof(*l));
}

static int exec_sql(salt_db *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(salt_db_handle(db), sql, NULL, NULL, &err) != SQLITE_OK) {
    salt_set_error("db: %s", err ? err : "?");
    sqlite3_free(err);
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_deploy_ensure_schema(salt_db *db) {
  return exec_sql(db, DEPLOY_SCHEMA);
}

static char *txn_state_dir(const salt_ctx *ctx, int64_t txn_id) {
  salt_buf b;
  salt_buf_init(&b);
  salt_buf_printf(&b, "%s/txn-%lld", ctx->state_dir, (long long)txn_id);
  return b.data;
}

static bool kernel_name_ok(const char *n) {
  static const char *prefixes[] = {"vmlinuz", "vmlinux", "bzImage", "Image", NULL};
  size_t len = strlen(n);
  if (len > 4 && strcmp(n + len - 4, ".img") == 0) return false;
  if (len > 4 && strcmp(n + len - 4, ".old") == 0) return false;
  if (strstr(n, "initr")) return false;
  for (int i = 0; prefixes[i]; i++) {
    size_t pl = strlen(prefixes[i]);
    if (strncmp(n, prefixes[i], pl) == 0 && (n[pl] == '\0' || n[pl] == '-')) return true;
  }
  return false;
}

static const char *kernel_version(const char *name) {
  const char *dash = strchr(name, '-');
  return dash ? dash + 1 : "";
}

static int kernel_cmp(const void *a, const void *b) {
  const char *ka = *(const char *const *)a;
  const char *kb = *(const char *const *)b;
  int vc = salt_vercmp(kernel_version(kb), kernel_version(ka));
  if (vc != 0) return vc;
  return strcmp(kb, ka);
}

int salt_boot_list_kernels(const char *bootdir, salt_strlist *out) {
  DIR *d = opendir(bootdir);
  if (!d) return SALT_OK;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!kernel_name_ok(e->d_name)) continue;
    char *full = salt_join_path(bootdir, e->d_name);
    struct stat st;
    bool reg = stat(full, &st) == 0 && S_ISREG(st.st_mode);
    free(full);
    if (reg) salt_strlist_push(out, e->d_name);
  }
  closedir(d);
  if (out->len > 1) qsort(out->items, out->len, sizeof(char *), kernel_cmp);
  return SALT_OK;
}

char *salt_boot_newest_kernel(const char *bootdir) {
  salt_strlist l;
  salt_strlist_init(&l);
  salt_boot_list_kernels(bootdir, &l);
  char *r = l.len ? salt_strdup(l.items[0]) : NULL;
  salt_strlist_free(&l);
  return r;
}

static int record_with_before(const salt_ctx *ctx, salt_db *db, int64_t txn_id, const char *before,
                              const char *rootdir) {
  if (salt_deploy_ensure_schema(db) != SALT_OK) return SALT_ERR;
  sqlite3 *h = salt_db_handle(db);
  if (before && salt_path_exists(before)) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(h, "ATTACH ? AS before;", -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, before, -1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc == SQLITE_DONE) {
        salt_buf sql;
        salt_buf_init(&sql);
        salt_buf_printf(&sql,
                        "DELETE FROM txn_changes WHERE txn_id=%lld;"
                        "INSERT INTO txn_changes(txn_id,name,old_version,new_version) "
                        "SELECT %lld,b.name,b.version||'-'||b.release,NULL FROM before.packages b "
                        "LEFT JOIN packages p ON p.name=b.name WHERE p.name IS NULL "
                        "UNION ALL "
                        "SELECT %lld,p.name,NULL,p.version||'-'||p.release FROM packages p "
                        "LEFT JOIN before.packages b ON b.name=p.name WHERE b.name IS NULL "
                        "UNION ALL "
                        "SELECT %lld,p.name,b.version||'-'||b.release,p.version||'-'||p.release "
                        "FROM packages p JOIN before.packages b ON b.name=p.name "
                        "WHERE p.version!=b.version OR p.release!=b.release "
                        "ORDER BY 2;",
                        (long long)txn_id, (long long)txn_id, (long long)txn_id, (long long)txn_id);
        exec_sql(db, sql.data);
        salt_buf_free(&sql);
        exec_sql(db, "DETACH before;");
      }
    }
  }
  char *bootdir = salt_join_path(rootdir ? rootdir : ctx->root, "boot");
  char *kernel = salt_boot_newest_kernel(bootdir);
  free(bootdir);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(h,
                         "INSERT OR REPLACE INTO txn_meta(txn_id,kernel,pinned) VALUES(?,?,"
                         "COALESCE((SELECT pinned FROM txn_meta WHERE txn_id=?),0));",
                         -1, &st, NULL) != SQLITE_OK) {
    salt_set_error("db txn_meta: %s", sqlite3_errmsg(h));
    free(kernel);
    return SALT_ERR;
  }
  sqlite3_bind_int64(st, 1, txn_id);
  if (kernel)
    sqlite3_bind_text(st, 2, kernel, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(st, 2);
  sqlite3_bind_int64(st, 3, txn_id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  free(kernel);
  return SALT_OK;
}

int salt_deploy_record(const salt_ctx *ctx, salt_db *db, int64_t txn_id) {
  char *sdir = txn_state_dir(ctx, txn_id);
  char *before = salt_join_path(sdir, "db.before");
  int rc = record_with_before(ctx, db, txn_id, before, NULL);
  free(sdir);
  free(before);
  return rc;
}

int salt_deploy_changes(salt_db *db, int64_t txn_id, salt_txn_change_list *out) {
  if (salt_deploy_ensure_schema(db) != SALT_OK) return SALT_ERR;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(salt_db_handle(db),
                         "SELECT name,old_version,new_version FROM txn_changes WHERE txn_id=? "
                         "ORDER BY name;",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_int64(st, 1, txn_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    salt_txn_change c;
    const char *o = (const char *)sqlite3_column_text(st, 1);
    const char *n = (const char *)sqlite3_column_text(st, 2);
    c.name = salt_strdup((const char *)sqlite3_column_text(st, 0));
    c.old_version = o ? salt_strdup(o) : NULL;
    c.new_version = n ? salt_strdup(n) : NULL;
    change_push(out, &c);
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_deploy_meta(salt_db *db, int64_t txn_id, salt_txn_meta *out) {
  memset(out, 0, sizeof(*out));
  if (salt_deploy_ensure_schema(db) != SALT_OK) return SALT_ERR;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(salt_db_handle(db), "SELECT kernel,pinned FROM txn_meta WHERE txn_id=?;",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_int64(st, 1, txn_id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char *k = (const char *)sqlite3_column_text(st, 0);
    out->kernel = k ? salt_strdup(k) : NULL;
    out->pinned = sqlite3_column_int(st, 1) != 0;
  }
  sqlite3_finalize(st);
  return SALT_OK;
}

int salt_deploy_pin(salt_db *db, int64_t txn_id, bool pinned) {
  if (salt_deploy_ensure_schema(db) != SALT_OK) return SALT_ERR;
  sqlite3 *h = salt_db_handle(db);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(h, "SELECT id FROM transactions WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_int64(st, 1, txn_id);
  bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (!found) {
    salt_set_error("no deployment #%lld", (long long)txn_id);
    return SALT_ERR_NOTFOUND;
  }
  if (sqlite3_prepare_v2(h,
                         "INSERT INTO txn_meta(txn_id,kernel,pinned) VALUES(?,NULL,?) "
                         "ON CONFLICT(txn_id) DO UPDATE SET pinned=excluded.pinned;",
                         -1, &st, NULL) != SQLITE_OK) {
    salt_set_error("db pin: %s", sqlite3_errmsg(h));
    return SALT_ERR;
  }
  sqlite3_bind_int64(st, 1, txn_id);
  sqlite3_bind_int(st, 2, pinned ? 1 : 0);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SALT_OK : SALT_ERR;
}

int salt_deploy_pick_rollback(salt_db *db, int64_t *id_out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(salt_db_handle(db),
                         "SELECT id FROM transactions WHERE status='ok' AND snapshot IS NOT NULL "
                         "AND snapshot!='' ORDER BY id DESC LIMIT 1;",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  int rc = SALT_ERR_NOTFOUND;
  if (sqlite3_step(st) == SQLITE_ROW) {
    *id_out = sqlite3_column_int64(st, 0);
    rc = SALT_OK;
  }
  sqlite3_finalize(st);
  if (rc != SALT_OK) salt_set_error("no deployment to roll back to");
  return rc;
}

int salt_deploy_root_changed(const salt_ctx *ctx, int64_t txn_id, const char *prefix, bool *changed) {
  *changed = false;
  char *sdir = txn_state_dir(ctx, txn_id);
  char *backup = salt_join_path(sdir, "backup");
  char *bprefix = salt_join_path(backup, prefix);
  if (salt_path_exists(bprefix)) *changed = true;
  char *added_path = salt_join_path(sdir, "added.list");
  salt_buf added;
  if (!*changed && salt_read_file(added_path, &added) == SALT_OK) {
    char *line = added.data;
    size_t pl = strlen(prefix);
    while (line && *line) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = '\0';
      if (strncmp(line, prefix, pl) == 0) {
        *changed = true;
        break;
      }
      line = nl ? nl + 1 : NULL;
    }
    salt_buf_free(&added);
  }
  free(sdir);
  free(backup);
  free(bprefix);
  free(added_path);
  return SALT_OK;
}

static int run_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run_cmd(const char *fmt, ...) {
  salt_buf cmd;
  salt_buf_init(&cmd);
  va_list ap;
  va_start(ap, fmt);
  char *s = NULL;
  int n = vasprintf(&s, fmt, ap);
  va_end(ap);
  if (n < 0 || !s) return SALT_ERR;
  salt_buf_append_str(&cmd, s);
  free(s);
  int rc = system(cmd.data);
  salt_buf_free(&cmd);
  return rc == 0 ? SALT_OK : SALT_ERR_IO;
}

static char *mountinfo_unescape(const char *s) {
  char *out = salt_strdup(s);
  char *w = out;
  for (const char *r = s; *r; r++) {
    if (r[0] == '\\' && r[1] && r[2] && r[3] && r[1] >= '0' && r[1] <= '3') {
      *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
      r += 3;
    } else {
      *w++ = *r;
    }
  }
  *w = '\0';
  return out;
}

/* Find the btrfs mount backing `path`: returns the subvolume path inside the
   filesystem (without the leading '/') and the block device. */
static int btrfs_mount_for(const char *path, char **subvol_out, char **device_out) {
  char real[PATH_MAX];
  if (!realpath(path, real)) {
    salt_set_error("realpath %s: %s", path, strerror(errno));
    return SALT_ERR_IO;
  }
  salt_buf mi;
  if (salt_read_file("/proc/self/mountinfo", &mi) != SALT_OK) return SALT_ERR_IO;
  int rc = SALT_ERR_NOTFOUND;
  char *line = mi.data;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    char *fields[16];
    int nf = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save); tok && nf < 16; tok = strtok_r(NULL, " ", &save))
      fields[nf++] = tok;
    int dash = -1;
    for (int i = 0; i < nf; i++)
      if (strcmp(fields[i], "-") == 0) dash = i;
    if (nf >= 6 && dash > 0 && dash + 2 < nf) {
      char *mp = mountinfo_unescape(fields[4]);
      if (strcmp(mp, real) == 0 && strcmp(fields[dash + 1], "btrfs") == 0) {
        char *fsroot = mountinfo_unescape(fields[3]);
        const char *sv = fsroot[0] == '/' ? fsroot + 1 : fsroot;
        free(*subvol_out);
        free(*device_out);
        *subvol_out = salt_strdup(sv);
        *device_out = mountinfo_unescape(fields[dash + 2]);
        free(fsroot);
        rc = SALT_OK;
      }
      free(mp);
    }
    line = nl ? nl + 1 : NULL;
  }
  salt_buf_free(&mi);
  return rc;
}

int salt_btrfs_layout_detect(const salt_ctx *ctx, salt_btrfs_layout *out) {
  memset(out, 0, sizeof(*out));
  if (btrfs_mount_for(ctx->root, &out->root_subvol, &out->device) != SALT_OK) {
    salt_set_error("%s is not a mounted btrfs subvolume", ctx->root);
    return SALT_ERR_NOTFOUND;
  }
  char *snapdev = NULL;
  if (btrfs_mount_for(ctx->snapshot_dir, &out->snapshots_subvol, &snapdev) != SALT_OK) {
    salt_set_error("%s must be a mounted btrfs subvolume (mount @snapshots there)",
                   ctx->snapshot_dir);
    salt_btrfs_layout_free(out);
    return SALT_ERR_NOTFOUND;
  }
  bool same = strcmp(snapdev, out->device) == 0;
  free(snapdev);
  if (!same) {
    salt_set_error("%s and %s are on different filesystems", ctx->root, ctx->snapshot_dir);
    salt_btrfs_layout_free(out);
    return SALT_ERR;
  }
  if (out->root_subvol[0] == '\0' || out->snapshots_subvol[0] == '\0') {
    salt_set_error("root and snapshot directory must be subvolumes below the btrfs top level");
    salt_btrfs_layout_free(out);
    return SALT_ERR;
  }
  size_t rl = strlen(out->root_subvol);
  if (strncmp(out->snapshots_subvol, out->root_subvol, rl) == 0 &&
      (out->snapshots_subvol[rl] == '/' || out->snapshots_subvol[rl] == '\0')) {
    salt_set_error("snapshot directory %s lives inside the root subvolume", ctx->snapshot_dir);
    salt_btrfs_layout_free(out);
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_btrfs_mount_toplevel(const salt_btrfs_layout *l, char **mountpoint_out) {
  salt_buf mp;
  salt_buf_init(&mp);
  salt_buf_printf(&mp, "/run/salt/toplevel-%ld", (long)getpid());
  if (salt_mkdirs(mp.data, 0700) != SALT_OK) {
    salt_buf_free(&mp);
    return SALT_ERR_IO;
  }
  if (mount(l->device, mp.data, "btrfs", 0, "subvolid=5") != 0) {
    salt_set_error("mount %s toplevel: %s", l->device, strerror(errno));
    rmdir(mp.data);
    salt_buf_free(&mp);
    return SALT_ERR_IO;
  }
  *mountpoint_out = mp.data;
  return SALT_OK;
}

int salt_btrfs_umount_toplevel(const char *mountpoint) {
  int rc = umount(mountpoint);
  rmdir(mountpoint);
  return rc == 0 ? SALT_OK : SALT_ERR_IO;
}

static int mark_undone(salt_db *db, int64_t from, int64_t to) {
  salt_buf sql;
  salt_buf_init(&sql);
  salt_buf_printf(&sql, "UPDATE transactions SET status='undone' WHERE id>=%lld AND id<%lld AND status='ok';",
                  (long long)from, (long long)to);
  int rc = exec_sql(db, sql.data);
  salt_buf_free(&sql);
  return rc;
}

static int64_t max_txn_id(salt_db *db) {
  sqlite3_stmt *st;
  int64_t id = 0;
  if (sqlite3_prepare_v2(salt_db_handle(db), "SELECT COALESCE(MAX(id),0) FROM transactions;", -1,
                         &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return id;
}

static int txn_status_snapshot(salt_db *db, int64_t id, char **status, char **snapshot) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(salt_db_handle(db), "SELECT status,snapshot FROM transactions WHERE id=?;",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  sqlite3_bind_int64(st, 1, id);
  int rc = SALT_ERR_NOTFOUND;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char *s = (const char *)sqlite3_column_text(st, 0);
    const char *sn = (const char *)sqlite3_column_text(st, 1);
    *status = salt_strdup(s ? s : "");
    *snapshot = sn ? salt_strdup(sn) : NULL;
    rc = SALT_OK;
  }
  sqlite3_finalize(st);
  return rc;
}

static int rollback_files(salt_ctx *ctx, salt_db *db, int64_t target, int64_t *rb_out) {
  int64_t latest = max_txn_id(db);
  for (int64_t id = latest; id >= target; id--) {
    char *status = NULL, *snap = NULL;
    if (txn_status_snapshot(db, id, &status, &snap) != SALT_OK) continue;
    if (strcmp(status, "ok") == 0) salt_txn_revert_files(ctx, id);
    free(status);
    free(snap);
  }
  int64_t rb = 0;
  if (salt_db_txn_new(db, "rollback", &rb) != SALT_OK) return SALT_ERR;
  char *rdir = txn_state_dir(ctx, rb);
  salt_mkdirs(rdir, 0755);
  char *rb_before = salt_join_path(rdir, "db.before");
  salt_db_vacuum_into(db, rb_before);
  char *tdir = txn_state_dir(ctx, target);
  char *before = salt_join_path(tdir, "db.before");
  int rc = SALT_OK;
  if (salt_path_exists(before)) rc = salt_db_restore_state_from(db, before);
  mark_undone(db, target, rb);
  salt_db_txn_finish(db, rb, "ok");
  record_with_before(ctx, db, rb, rb_before, NULL);
  *rb_out = rb;
  free(rdir);
  free(rb_before);
  free(tdir);
  free(before);
  return rc;
}

static int rollback_btrfs(salt_ctx *ctx, salt_db *db, int64_t target, const char *snapshot,
                          int64_t *rb_out, char **new_root_out) {
  salt_btrfs_layout lay;
  int rc = salt_btrfs_layout_detect(ctx, &lay);
  if (rc != SALT_OK) return rc;
  char *top = NULL;
  rc = salt_btrfs_mount_toplevel(&lay, &top);
  if (rc != SALT_OK) {
    salt_btrfs_layout_free(&lay);
    return rc;
  }

  salt_buf src, next, cur, saved;
  salt_buf_init(&src);
  salt_buf_init(&next);
  salt_buf_init(&cur);
  salt_buf_init(&saved);
  salt_buf_printf(&src, "%s/%s/%s", top, lay.snapshots_subvol, snapshot);
  salt_buf_printf(&next, "%s/%s.next", top, lay.root_subvol);
  salt_buf_printf(&cur, "%s/%s", top, lay.root_subvol);

  int64_t rb = max_txn_id(db) + 1;
  salt_buf_printf(&saved, "%s/%s/root-%lld", top, lay.snapshots_subvol, (long long)rb);

  salt_db *ndb = NULL;
  char *ndb_path = NULL;
  if (!salt_is_dir(src.data)) {
    salt_set_error("snapshot %s is missing", src.data);
    rc = SALT_ERR_NOTFOUND;
    goto out;
  }
  if (salt_path_exists(next.data)) run_cmd("btrfs subvolume delete '%s' >/dev/null 2>&1", next.data);
  rc = run_cmd("btrfs subvolume snapshot '%s' '%s' >/dev/null", src.data, next.data);
  if (rc != SALT_OK) {
    salt_set_error("btrfs snapshot %s -> %s failed", src.data, next.data);
    goto out;
  }

  ndb_path = salt_join_path(next.data, "var/lib/salt/db.sqlite");
  if (salt_db_open(ndb_path, &ndb) != SALT_OK) {
    rc = SALT_ERR;
    goto fail;
  }
  salt_deploy_ensure_schema(db);
  salt_deploy_ensure_schema(ndb);
  {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(salt_db_handle(ndb), "ATTACH ? AS cur;", -1, &st, NULL) != SQLITE_OK) {
      rc = SALT_ERR;
      goto fail;
    }
    sqlite3_bind_text(st, 1, ctx->db_path, -1, SQLITE_TRANSIENT);
    int src_rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (src_rc != SQLITE_DONE) {
      salt_set_error("attach %s: %s", ctx->db_path, sqlite3_errmsg(salt_db_handle(ndb)));
      rc = SALT_ERR;
      goto fail;
    }
    salt_buf sql;
    salt_buf_init(&sql);
    salt_buf_printf(&sql,
                    "BEGIN IMMEDIATE;"
                    "INSERT OR REPLACE INTO transactions SELECT * FROM cur.transactions WHERE id>=%lld;"
                    "INSERT OR REPLACE INTO txn_meta SELECT * FROM cur.txn_meta WHERE txn_id>=%lld;"
                    "DELETE FROM txn_changes WHERE txn_id>=%lld;"
                    "INSERT INTO txn_changes SELECT * FROM cur.txn_changes WHERE txn_id>=%lld;"
                    "COMMIT;",
                    (long long)target, (long long)target, (long long)target, (long long)target);
    rc = exec_sql(ndb, sql.data);
    salt_buf_free(&sql);
    exec_sql(ndb, "DETACH cur;");
    if (rc != SALT_OK) goto fail;
  }
  {
    salt_buf rdir;
    salt_buf_init(&rdir);
    salt_buf_printf(&rdir, "%s/var/lib/salt/state/txn-%lld", next.data, (long long)rb);
    salt_mkdirs(rdir.data, 0755);
    char *rb_before = salt_join_path(rdir.data, "db.before");
    salt_db_vacuum_into(db, rb_before);
    int64_t got = 0;
    salt_buf sql;
    salt_buf_init(&sql);
    salt_buf_printf(&sql,
                    "INSERT INTO transactions(id,op,status,time,snapshot) "
                    "VALUES(%lld,'rollback','ok',strftime('%%s','now'),'root-%lld');",
                    (long long)rb, (long long)rb);
    rc = exec_sql(ndb, sql.data);
    salt_buf_free(&sql);
    if (rc != SALT_OK) {
      free(rb_before);
      salt_buf_free(&rdir);
      goto fail;
    }
    got = rb;
    mark_undone(ndb, target, got);
    record_with_before(ctx, ndb, got, rb_before, ctx->root);
    free(rb_before);
    salt_buf_free(&rdir);
  }
  salt_db_close(ndb);
  ndb = NULL;

  if (rename(cur.data, saved.data) != 0) {
    salt_set_error("rename %s -> %s: %s", cur.data, saved.data, strerror(errno));
    rc = SALT_ERR_IO;
    goto fail;
  }
  if (rename(next.data, cur.data) != 0) {
    salt_set_error("rename %s -> %s: %s", next.data, cur.data, strerror(errno));
    rename(saved.data, cur.data);
    rc = SALT_ERR_IO;
    goto fail;
  }
  run_cmd("btrfs property set -ts '%s' ro true >/dev/null 2>&1", saved.data);

  {
    int64_t cur_rb = 0;
    if (salt_db_txn_new(db, "rollback", &cur_rb) == SALT_OK) {
      salt_buf sql;
      salt_buf_init(&sql);
      salt_buf_printf(&sql, "UPDATE transactions SET snapshot='root-%lld' WHERE id=%lld;",
                      (long long)rb, (long long)cur_rb);
      exec_sql(db, sql.data);
      salt_buf_free(&sql);
      salt_db_txn_finish(db, cur_rb, "ok");
    }
  }
  *rb_out = rb;
  *new_root_out = salt_strdup(cur.data);
  goto out;

fail:
  if (ndb) salt_db_close(ndb);
  ndb = NULL;
  run_cmd("btrfs subvolume delete '%s' >/dev/null 2>&1", next.data);
out:
  if (ndb) salt_db_close(ndb);
  free(ndb_path);
  salt_buf_free(&src);
  salt_buf_free(&next);
  salt_buf_free(&cur);
  salt_buf_free(&saved);
  if (rc != SALT_OK || !*new_root_out) {
    salt_btrfs_umount_toplevel(top);
    free(top);
  }
  salt_btrfs_layout_free(&lay);
  return rc;
}

int salt_rollback_to(salt_ctx *ctx, salt_db *db, int64_t txn_id, int64_t *rollback_txn_out,
                     char **new_root_out, bool *reboot_required) {
  *rollback_txn_out = 0;
  *new_root_out = NULL;
  *reboot_required = false;
  if (txn_id <= 0) {
    int rc = salt_deploy_pick_rollback(db, &txn_id);
    if (rc != SALT_OK) return rc;
  }
  char *status = NULL, *snapshot = NULL;
  if (txn_status_snapshot(db, txn_id, &status, &snapshot) != SALT_OK) {
    salt_set_error("no deployment #%lld", (long long)txn_id);
    return SALT_ERR_NOTFOUND;
  }
  int rc;
  if (ctx->use_btrfs && snapshot && strncmp(snapshot, "root-", 5) == 0) {
    rc = rollback_btrfs(ctx, db, txn_id, snapshot, rollback_txn_out, new_root_out);
    if (rc == SALT_OK) *reboot_required = true;
  } else {
    char *sdir = txn_state_dir(ctx, txn_id);
    char *before = salt_join_path(sdir, "db.before");
    bool ok = snapshot && snapshot[0] && salt_path_exists(before);
    free(sdir);
    free(before);
    if (!ok) {
      salt_set_error("deployment #%lld has no snapshot left to restore", (long long)txn_id);
      free(status);
      free(snapshot);
      return SALT_ERR_NOTFOUND;
    }
    rc = rollback_files(ctx, db, txn_id, rollback_txn_out);
  }
  free(status);
  free(snapshot);
  return rc;
}

int salt_deploy_prune(const salt_ctx *ctx, salt_db *db, int keep, salt_strlist *removed) {
  if (keep < 1) keep = 1;
  if (salt_deploy_ensure_schema(db) != SALT_OK) return SALT_ERR;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(salt_db_handle(db),
                         "SELECT t.id,t.snapshot FROM transactions t "
                         "LEFT JOIN txn_meta m ON m.txn_id=t.id "
                         "WHERE t.snapshot IS NOT NULL AND t.snapshot!='' "
                         "AND COALESCE(m.pinned,0)=0 ORDER BY t.id DESC;",
                         -1, &st, NULL) != SQLITE_OK)
    return SALT_ERR;
  int seen = 0;
  salt_strlist victims;
  salt_strlist_init(&victims);
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (seen++ < keep) continue;
    salt_buf b;
    salt_buf_init(&b);
    salt_buf_printf(&b, "%lld\t%s", (long long)sqlite3_column_int64(st, 0),
                    (const char *)sqlite3_column_text(st, 1));
    salt_strlist_push(&victims, b.data);
    salt_buf_free(&b);
  }
  sqlite3_finalize(st);
  for (size_t i = 0; i < victims.len; i++) {
    char *tab = strchr(victims.items[i], '\t');
    if (!tab) continue;
    *tab = '\0';
    int64_t id = strtoll(victims.items[i], NULL, 10);
    const char *snap = tab + 1;
    bool gone = true;
    if (strncmp(snap, "root-", 5) == 0) {
      char *full = salt_join_path(ctx->snapshot_dir, snap);
      if (salt_is_dir(full))
        gone = run_cmd("btrfs subvolume delete '%s' >/dev/null 2>&1", full) == SALT_OK;
      free(full);
    }
    if (!gone) continue;
    char *sdir = txn_state_dir(ctx, id);
    char *backup = salt_join_path(sdir, "backup");
    char *added = salt_join_path(sdir, "added.list");
    salt_remove_recursive(backup);
    unlink(added);
    free(sdir);
    free(backup);
    free(added);
    salt_buf sql;
    salt_buf_init(&sql);
    salt_buf_printf(&sql, "UPDATE transactions SET snapshot=NULL WHERE id=%lld;", (long long)id);
    exec_sql(db, sql.data);
    salt_buf_free(&sql);
    if (removed) salt_strlist_push(removed, snap);
  }
  salt_strlist_free(&victims);
  return SALT_OK;
}

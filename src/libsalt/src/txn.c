#include "salt/txn.h"
#include "salt/zst.h"
#include "salt/tar.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

int salt_ctx_init(salt_ctx *ctx, const char *root) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->root = salt_strdup(root && root[0] ? root : "/");
  ctx->db_path = salt_join_path(ctx->root, "var/lib/salt/db.sqlite");
  ctx->state_dir = salt_join_path(ctx->root, "var/lib/salt/state");
  ctx->snapshot_dir = salt_join_path(ctx->root, ".snapshots");
  ctx->use_btrfs = false;
  const char *force = getenv("SALT_BTRFS");
  if (force && force[0] == '1') ctx->use_btrfs = true;
  return SALT_OK;
}

void salt_ctx_free(salt_ctx *ctx) {
  free(ctx->root);
  free(ctx->db_path);
  free(ctx->state_dir);
  free(ctx->snapshot_dir);
  memset(ctx, 0, sizeof(*ctx));
}

static char *txn_state_dir(const salt_ctx *ctx, int64_t txn_id) {
  salt_buf b;
  salt_buf_init(&b);
  salt_buf_printf(&b, "%s/txn-%lld", ctx->state_dir, (long long)txn_id);
  return b.data;
}

int salt_snapshot_create(const salt_ctx *ctx, salt_db *db, int64_t txn_id, char **snapshot_out) {
  char *sdir = txn_state_dir(ctx, txn_id);
  salt_mkdirs(sdir, 0755);
  char *before_db = salt_join_path(sdir, "db.before");
  salt_db_vacuum_into(db, before_db);
  free(before_db);
  if (ctx->use_btrfs) {
    salt_buf name;
    salt_buf_init(&name);
    salt_buf_printf(&name, "root-%lld", (long long)txn_id);
    salt_buf cmd;
    salt_buf_init(&cmd);
    salt_buf_printf(&cmd, "btrfs subvolume snapshot -r '%s' '%s/%s' >/dev/null 2>&1", ctx->root,
                    ctx->snapshot_dir, name.data);
    salt_mkdirs(ctx->snapshot_dir, 0755);
    int rc = system(cmd.data);
    salt_buf_free(&cmd);
    if (rc != 0) {
      salt_set_error("btrfs snapshot failed");
      salt_buf_free(&name);
      free(sdir);
      return SALT_ERR_IO;
    }
    *snapshot_out = name.data;
    free(sdir);
    return SALT_OK;
  }
  *snapshot_out = txn_state_dir(ctx, txn_id);
  free(sdir);
  return SALT_OK;
}

int salt_snapshot_restore(const salt_ctx *ctx, const char *snapshot) {
  if (!ctx->use_btrfs) return SALT_OK;
  salt_buf cmd;
  salt_buf_init(&cmd);
  salt_buf_printf(&cmd, "btrfs subvolume set-default '%s/%s' '%s' >/dev/null 2>&1", ctx->snapshot_dir,
                  snapshot, ctx->root);
  int rc = system(cmd.data);
  salt_buf_free(&cmd);
  return rc == 0 ? SALT_OK : SALT_ERR_IO;
}

static void backup_file(const char *root, const char *backup_dir, const char *relpath) {
  char *src = salt_join_path(root, relpath);
  struct stat st;
  if (lstat(src, &st) != 0) {
    free(src);
    return;
  }
  char *dst = salt_join_path(backup_dir, relpath);
  char *dup = salt_strdup(dst);
  char *slash = strrchr(dup, '/');
  if (slash) {
    *slash = '\0';
    salt_mkdirs(dup, 0755);
  }
  free(dup);
  if (S_ISLNK(st.st_mode)) {
    char target[1024];
    ssize_t n = readlink(src, target, sizeof(target) - 1);
    if (n >= 0) {
      target[n] = '\0';
      unlink(dst);
      symlink(target, dst);
    }
  } else if (S_ISREG(st.st_mode)) {
    salt_copy_file(src, dst);
  }
  free(src);
  free(dst);
}

static bool manifest_has(const salt_manifest *m, const char *path) {
  for (size_t i = 0; i < m->len; i++)
    if (strcmp(m->items[i].path, path) == 0) return true;
  return false;
}

int salt_install_archive(salt_ctx *ctx, salt_db *db, const salt_archive *ar, const char *repo,
                         const char *sig_status, int64_t txn_id) {
  char *sdir = txn_state_dir(ctx, txn_id);
  char *backup_dir = salt_join_path(sdir, "backup");
  char *added_path = salt_join_path(sdir, "added.list");
  salt_mkdirs(backup_dir, 0755);

  salt_manifest old;
  salt_manifest_init(&old);
  bool upgrade = salt_db_is_installed(db, ar->meta.name);
  if (upgrade) salt_db_pkg_manifest(db, ar->meta.name, &old);

  salt_buf added;
  salt_buf_init(&added);
  for (size_t i = 0; i < ar->manifest.len; i++) {
    const salt_manifest_entry *e = &ar->manifest.items[i];
    if (e->typeflag == SALT_TAR_DIR) continue;
    char *full = salt_join_path(ctx->root, e->path);
    bool exists = salt_path_exists(full);
    free(full);
    if (exists) {
      backup_file(ctx->root, backup_dir, e->path);
    } else {
      salt_buf_printf(&added, "%s\n", e->path);
    }
  }

  salt_strlist installed;
  salt_strlist_init(&installed);
  int rc = salt_archive_extract_payload(ar, ctx->root, &installed);
  salt_strlist_free(&installed);
  if (rc != SALT_OK) goto done;

  if (upgrade) {
    for (size_t i = 0; i < old.len; i++) {
      const salt_manifest_entry *e = &old.items[i];
      if (e->typeflag == SALT_TAR_DIR) continue;
      if (manifest_has(&ar->manifest, e->path)) continue;
      backup_file(ctx->root, backup_dir, e->path);
      char *full = salt_join_path(ctx->root, e->path);
      unlink(full);
      free(full);
    }
  }

  salt_write_file(added_path, added.data ? added.data : "", added.len, 0644);
  rc = salt_db_record_install(db, &ar->meta, &ar->manifest, repo, sig_status, txn_id);

done:
  salt_manifest_free(&old);
  salt_buf_free(&added);
  free(sdir);
  free(backup_dir);
  free(added_path);
  return rc;
}

int salt_remove_pkg(salt_ctx *ctx, salt_db *db, const char *name, int64_t txn_id) {
  if (!salt_db_is_installed(db, name)) {
    salt_set_error("package not installed: %s", name);
    return SALT_ERR_NOTFOUND;
  }
  char *sdir = txn_state_dir(ctx, txn_id);
  char *backup_dir = salt_join_path(sdir, "backup");
  salt_mkdirs(backup_dir, 0755);

  salt_manifest man;
  salt_manifest_init(&man);
  salt_db_pkg_manifest(db, name, &man);
  for (size_t i = 0; i < man.len; i++) {
    const salt_manifest_entry *e = &man.items[i];
    char *full = salt_join_path(ctx->root, e->path);
    if (e->typeflag == SALT_TAR_DIR) {
      rmdir(full);
    } else {
      backup_file(ctx->root, backup_dir, e->path);
      unlink(full);
    }
    free(full);
  }
  salt_manifest_free(&man);
  int rc = salt_db_record_remove(db, name, txn_id);
  free(sdir);
  free(backup_dir);
  return rc;
}

int salt_deployments_list(salt_ctx *ctx, salt_db *db, salt_deployment_list *out) {
  (void)ctx;
  return salt_db_deployments(db, out);
}

static int restore_tree(const char *backup_dir, const char *rel, const char *root) {
  char *dirpath = rel[0] ? salt_join_path(backup_dir, rel) : salt_strdup(backup_dir);
  DIR *d = opendir(dirpath);
  if (!d) {
    free(dirpath);
    return SALT_OK;
  }
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *childrel = rel[0] ? salt_join_path(rel, e->d_name) : salt_strdup(e->d_name);
    char *childfull = salt_join_path(dirpath, e->d_name);
    struct stat st;
    if (lstat(childfull, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        restore_tree(backup_dir, childrel, root);
      } else {
        char *dest = salt_join_path(root, childrel);
        char *dup = salt_strdup(dest);
        char *slash = strrchr(dup, '/');
        if (slash) {
          *slash = '\0';
          salt_mkdirs(dup, 0755);
        }
        free(dup);
        if (S_ISLNK(st.st_mode)) {
          char target[1024];
          ssize_t n = readlink(childfull, target, sizeof(target) - 1);
          if (n >= 0) {
            target[n] = '\0';
            unlink(dest);
            symlink(target, dest);
          }
        } else {
          salt_copy_file(childfull, dest);
        }
        free(dest);
      }
    }
    free(childrel);
    free(childfull);
  }
  closedir(d);
  free(dirpath);
  return SALT_OK;
}

int salt_txn_revert_files(const salt_ctx *ctx, int64_t txn_id) {
  char *sdir = txn_state_dir(ctx, txn_id);
  char *backup_dir = salt_join_path(sdir, "backup");
  char *added_path = salt_join_path(sdir, "added.list");
  salt_buf added;
  if (salt_read_file(added_path, &added) == SALT_OK) {
    char *line = added.data;
    while (line && *line) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = '\0';
      if (line[0]) {
        char *full = salt_join_path(ctx->root, line);
        unlink(full);
        free(full);
      }
      line = nl ? nl + 1 : NULL;
    }
    salt_buf_free(&added);
  }
  restore_tree(backup_dir, "", ctx->root);
  free(sdir);
  free(backup_dir);
  free(added_path);
  return SALT_OK;
}

int salt_rollback_last(salt_ctx *ctx, salt_db *db) {
  int64_t id = 0;
  char *snapshot = NULL;
  if (salt_db_last_ok_txn(db, &id, &snapshot) != SALT_OK) {
    salt_set_error("no deployment to roll back to");
    return SALT_ERR_NOTFOUND;
  }
  char *sdir = txn_state_dir(ctx, id);
  char *before_db = salt_join_path(sdir, "db.before");

  int rc = SALT_OK;
  if (ctx->use_btrfs && snapshot && snapshot[0]) {
    rc = salt_snapshot_restore(ctx, snapshot);
  } else {
    salt_txn_revert_files(ctx, id);
    if (salt_path_exists(before_db)) salt_db_restore_state_from(db, before_db);
  }

  int64_t rb_txn;
  if (salt_db_txn_new(db, "rollback", &rb_txn) == SALT_OK)
    salt_db_txn_finish(db, rb_txn, "ok");

  free(snapshot);
  free(sdir);
  free(before_db);
  return rc;
}

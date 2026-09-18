#include "salt/gc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

void salt_gc_report_init(salt_gc_report *r) {
  memset(r, 0, sizeof(*r));
  salt_strlist_init(&r->actions);
}

void salt_gc_report_free(salt_gc_report *r) {
  salt_strlist_free(&r->actions);
  memset(r, 0, sizeof(*r));
}

static void note(salt_gc_report *r, const char *fmt, ...) {
  char tmp[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  salt_strlist_push(&r->actions, tmp);
}

static uint64_t tree_size(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
  uint64_t total = 0;
  DIR *d = opendir(path);
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *child = salt_join_path(path, e->d_name);
    total += tree_size(child);
    free(child);
  }
  closedir(d);
  return total;
}

int64_t salt_gc_booted_generation(const salt_ctx *ctx) {
  if (!ctx->root || strcmp(ctx->root, "/") != 0) return -1;
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if (!f) return -1;
  char line[4096];
  int64_t found = -1;
  while (fgets(line, sizeof(line), f)) {
    char subvol[1024], mnt[1024];
    if (sscanf(line, "%*d %*d %*d:%*d %1023s %1023s", subvol, mnt) != 2) continue;
    if (strcmp(mnt, "/") != 0) continue;
    const char *p = strstr(subvol, "/.snapshots/root-");
    if (!p) continue;
    found = strtoll(p + strlen("/.snapshots/root-"), NULL, 10);
    break;
  }
  fclose(f);
  return found;
}

static bool id_in(const int64_t *ids, size_t n, int64_t id) {
  for (size_t i = 0; i < n; i++)
    if (ids[i] == id) return true;
  return false;
}

static int prune_generation(salt_ctx *ctx, salt_db *db, const salt_deployment *d, bool dry_run,
                            salt_gc_report *out) {
  salt_buf sdir;
  salt_buf_init(&sdir);
  salt_buf_printf(&sdir, "%s/txn-%lld", ctx->state_dir, (long long)d->id);
  uint64_t bytes = tree_size(sdir.data);
  bool had_snapshot = false;
  salt_buf snap;
  salt_buf_init(&snap);
  if (ctx->use_btrfs && d->snapshot && d->snapshot[0] && strchr(d->snapshot, '/') == NULL) {
    salt_buf_printf(&snap, "%s/%s", ctx->snapshot_dir, d->snapshot);
    had_snapshot = salt_is_dir(snap.data);
  }
  int rc = SALT_OK;
  if (!dry_run) {
    if (had_snapshot) {
      salt_buf cmd;
      salt_buf_init(&cmd);
      salt_buf_printf(&cmd, "btrfs subvolume delete '%s' >/dev/null 2>&1", snap.data);
      if (system(cmd.data) != 0) {
        salt_set_error("btrfs subvolume delete failed for %s", snap.data);
        rc = SALT_ERR_IO;
      }
      salt_buf_free(&cmd);
    }
    if (rc == SALT_OK) rc = salt_remove_recursive(sdir.data);
    if (rc == SALT_OK) {
      salt_db_txn_finish(db, d->id, "pruned");
      salt_db_txn_set_snapshot(db, d->id, "");
    }
  }
  if (rc == SALT_OK) {
    out->generations_removed++;
    out->bytes_freed += bytes;
    note(out, "%s generation %lld (%s, %s%s, %llu bytes)", dry_run ? "would prune" : "pruned",
         (long long)d->id, d->op, d->status, had_snapshot ? ", btrfs snapshot" : "",
         (unsigned long long)bytes);
  }
  salt_buf_free(&snap);
  salt_buf_free(&sdir);
  return rc;
}

static int sweep_cache_dir(const char *dir, const salt_strlist *keep, bool dry_run,
                           salt_gc_report *out) {
  DIR *d = opendir(dir);
  if (!d) return SALT_OK;
  struct dirent *e;
  int rc = SALT_OK;
  while ((e = readdir(d)) != NULL && rc == SALT_OK) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (keep && salt_strlist_contains(keep, e->d_name)) continue;
    char *full = salt_join_path(dir, e->d_name);
    struct stat st;
    if (lstat(full, &st) == 0 && S_ISREG(st.st_mode)) {
      if (!dry_run && unlink(full) != 0) {
        salt_set_error("unlink %s failed", full);
        rc = SALT_ERR_IO;
      } else {
        out->cache_files_removed++;
        out->bytes_freed += (uint64_t)st.st_size;
        note(out, "%s %s (%llu bytes)", dry_run ? "would remove" : "removed", full,
             (unsigned long long)st.st_size);
      }
    }
    free(full);
  }
  closedir(d);
  return rc;
}

static int sweep_cache(const salt_ctx *ctx, const salt_strlist *keep, bool dry_run,
                       salt_gc_report *out) {
  char *cache = salt_join_path(ctx->root, "var/lib/salt/cache");
  DIR *d = opendir(cache);
  if (!d) {
    free(cache);
    return SALT_OK;
  }
  struct dirent *e;
  int rc = SALT_OK;
  while ((e = readdir(d)) != NULL && rc == SALT_OK) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (strcmp(e->d_name, "strata") == 0) continue;
    char *sub = salt_join_path(cache, e->d_name);
    if (salt_is_dir(sub)) rc = sweep_cache_dir(sub, keep, dry_run, out);
    free(sub);
  }
  closedir(d);
  free(cache);
  return rc;
}

int salt_gc_run(salt_ctx *ctx, salt_db *db, const salt_gc_opts *opts, salt_gc_report *out) {
  int keep = opts->keep < 1 ? 1 : opts->keep;
  salt_deployment_list deps;
  salt_deployment_list_init(&deps);
  salt_db_deployments(db, &deps);

  int64_t booted = salt_gc_booted_generation(ctx);
  int64_t *kept = calloc(deps.len + opts->npinned + 2, sizeof(int64_t));
  size_t nkept = 0;
  int ok_seen = 0;
  int64_t newest = deps.len ? deps.items[0].id : 0;
  for (size_t i = 0; i < deps.len; i++) {
    const salt_deployment *d = &deps.items[i];
    bool keep_it = false;
    if (strcmp(d->status, "ok") == 0 && ok_seen < keep) {
      ok_seen++;
      keep_it = true;
    }
    if (d->id == booted || id_in(opts->pinned, opts->npinned, d->id)) keep_it = true;
    if (strcmp(d->status, "started") == 0 && d->id == newest) keep_it = true;
    if (keep_it) kept[nkept++] = d->id;
  }

  salt_strlist referenced;
  salt_strlist_init(&referenced);
  salt_db_pkglist live;
  salt_db_pkglist_init(&live);
  salt_db_list_installed(db, &live);
  for (size_t i = 0; i < live.len; i++) {
    const char *f = live.items[i].filename;
    if (f && f[0] && !salt_strlist_contains(&referenced, f)) salt_strlist_push(&referenced, f);
  }
  salt_db_pkglist_free(&live);
  for (size_t i = 0; i < nkept; i++) {
    salt_buf before;
    salt_buf_init(&before);
    salt_buf_printf(&before, "%s/txn-%lld/db.before", ctx->state_dir, (long long)kept[i]);
    if (salt_path_exists(before.data)) salt_db_snapshot_filenames(before.data, &referenced);
    salt_buf_free(&before);
  }

  int rc = SALT_OK;
  for (size_t i = 0; i < deps.len && rc == SALT_OK; i++) {
    const salt_deployment *d = &deps.items[i];
    if (strcmp(d->status, "pruned") == 0) continue;
    if (id_in(kept, nkept, d->id)) continue;
    rc = prune_generation(ctx, db, d, opts->dry_run, out);
  }
  if (rc == SALT_OK) rc = sweep_cache(ctx, &referenced, opts->dry_run, out);

  salt_strlist_free(&referenced);
  free(kept);
  salt_deployment_list_free(&deps);
  return rc;
}

int salt_cache_clean(const salt_ctx *ctx, const char *cache_dir, bool dry_run,
                     salt_gc_report *out) {
  if (cache_dir && cache_dir[0]) return sweep_cache_dir(cache_dir, NULL, dry_run, out);
  return sweep_cache(ctx, NULL, dry_run, out);
}

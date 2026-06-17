#include "salt/archive.h"
#include "salt/tar.h"
#include "salt/zst.h"
#include "salt/hash.h"

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

void salt_archive_init(salt_archive *a) {
  salt_pkg_meta_init(&a->meta);
  salt_manifest_init(&a->manifest);
  salt_buf_init(&a->payload);
  salt_strlist_init(&a->scripts);
}

void salt_archive_free(salt_archive *a) {
  salt_pkg_meta_free(&a->meta);
  salt_manifest_free(&a->manifest);
  salt_buf_free(&a->payload);
  salt_strlist_free(&a->scripts);
}

static int collect_paths(const char *root, const char *rel, salt_strlist *out) {
  char *dirpath = rel[0] ? salt_join_path(root, rel) : salt_strdup(root);
  DIR *d = opendir(dirpath);
  if (!d) {
    free(dirpath);
    return SALT_ERR_IO;
  }
  struct dirent *e;
  salt_strlist names;
  salt_strlist_init(&names);
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    salt_strlist_push(&names, e->d_name);
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
    char *childrel = rel[0] ? salt_join_path(rel, names.items[i]) : salt_strdup(names.items[i]);
    char *childfull = salt_join_path(dirpath, names.items[i]);
    struct stat st;
    if (lstat(childfull, &st) == 0) {
      salt_strlist_push(out, childrel);
      if (S_ISDIR(st.st_mode)) collect_paths(root, childrel, out);
    }
    free(childrel);
    free(childfull);
  }
  salt_strlist_free(&names);
  free(dirpath);
  return SALT_OK;
}

int salt_archive_build_from_dir(const char *staging_dir, const salt_pkg_meta *meta,
                                const char *scripts_dir, salt_archive *out) {
  salt_archive_init(out);

  out->meta.name = salt_strdup(meta->name);
  out->meta.version = salt_strdup(meta->version);
  out->meta.release = meta->release;
  out->meta.arch = salt_strdup(meta->arch);
  out->meta.summary = salt_strdup(meta->summary ? meta->summary : "");
  out->meta.license = salt_strdup(meta->license ? meta->license : "");
  out->meta.repro_status = salt_strdup(meta->repro_status ? meta->repro_status : "unverified");
  out->meta.repro_reason = meta->repro_reason ? salt_strdup(meta->repro_reason) : NULL;
  for (size_t i = 0; i < meta->deps.len; i++) salt_strlist_push(&out->meta.deps, meta->deps.items[i]);

  salt_strlist paths;
  salt_strlist_init(&paths);
  if (collect_paths(staging_dir, "", &paths) != SALT_OK) {
    salt_strlist_free(&paths);
    salt_archive_free(out);
    return SALT_ERR_IO;
  }

  salt_buf payload;
  salt_buf_init(&payload);
  salt_tar_writer *w = salt_tar_writer_new(&payload);

  for (size_t i = 0; i < paths.len; i++) {
    char *full = salt_join_path(staging_dir, paths.items[i]);
    struct stat st;
    if (lstat(full, &st) != 0) {
      free(full);
      continue;
    }
    salt_manifest_entry me;
    memset(&me, 0, sizeof(me));
    me.path = paths.items[i];
    me.mode = st.st_mode & 07777;
    if (S_ISDIR(st.st_mode)) {
      me.typeflag = SALT_TAR_DIR;
      salt_tar_entry te = {paths.items[i], SALT_TAR_DIR, me.mode, 0, NULL, NULL};
      salt_tar_writer_add(w, &te);
    } else if (S_ISLNK(st.st_mode)) {
      char target[1024];
      ssize_t n = readlink(full, target, sizeof(target) - 1);
      if (n < 0) {
        free(full);
        continue;
      }
      target[n] = '\0';
      me.typeflag = SALT_TAR_SYMLINK;
      me.linkname = target;
      salt_tar_entry te = {paths.items[i], SALT_TAR_SYMLINK, me.mode, 0, target, NULL};
      salt_tar_writer_add(w, &te);
    } else {
      salt_buf fb;
      if (salt_read_file(full, &fb) != SALT_OK) {
        free(full);
        continue;
      }
      char hex[SALT_SHA256_HEXLEN + 1];
      salt_sha256_buf(fb.data, fb.len, hex);
      me.typeflag = SALT_TAR_FILE;
      me.size = fb.len;
      me.sha256 = hex;
      salt_tar_entry te = {paths.items[i], SALT_TAR_FILE, me.mode, fb.len, NULL, fb.data};
      salt_tar_writer_add(w, &te);
      salt_manifest_push(&out->manifest, &me);
      salt_buf_free(&fb);
      free(full);
      continue;
    }
    salt_manifest_push(&out->manifest, &me);
    free(full);
  }
  salt_tar_writer_finish(w);
  salt_tar_writer_free(w);
  salt_strlist_free(&paths);

  int rc = salt_zst_compress(payload.data, payload.len, 19, &out->payload);
  salt_buf_free(&payload);
  if (rc != SALT_OK) {
    salt_archive_free(out);
    return rc;
  }

  if (scripts_dir && salt_is_dir(scripts_dir)) {
    DIR *d = opendir(scripts_dir);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        salt_strlist_push(&out->scripts, e->d_name);
      }
      closedir(d);
    }
  }
  return SALT_OK;
}

int salt_archive_write(const salt_archive *a, const char *out_path) {
  salt_buf outer;
  salt_buf_init(&outer);
  salt_tar_writer *w = salt_tar_writer_new(&outer);

  salt_buf meta_toml;
  salt_pkg_meta_to_toml(&a->meta, &meta_toml);
  salt_tar_entry me = {"metadata.toml", SALT_TAR_FILE, 0644, meta_toml.len, NULL, meta_toml.data};
  salt_tar_writer_add(w, &me);

  salt_buf man_toml;
  salt_manifest_to_toml(&a->manifest, &man_toml);
  salt_tar_entry mf = {"manifest.toml", SALT_TAR_FILE, 0644, man_toml.len, NULL, man_toml.data};
  salt_tar_writer_add(w, &mf);

  salt_tar_entry pf = {"files.tar.zst", SALT_TAR_FILE, 0644, a->payload.len, NULL, a->payload.data};
  salt_tar_writer_add(w, &pf);

  salt_tar_writer_finish(w);
  salt_tar_writer_free(w);
  salt_buf_free(&meta_toml);
  salt_buf_free(&man_toml);

  int rc = salt_write_file(out_path, outer.data, outer.len, 0644);
  salt_buf_free(&outer);
  return rc;
}

typedef struct {
  salt_archive *a;
  bool got_meta;
  bool got_manifest;
} open_ctx;

static int open_cb(const salt_tar_entry *e, void *ud) {
  open_ctx *ctx = ud;
  if (strcmp(e->path, "metadata.toml") == 0) {
    salt_pkg_meta_from_toml(e->data, e->size, &ctx->a->meta);
    ctx->got_meta = true;
  } else if (strcmp(e->path, "manifest.toml") == 0) {
    salt_manifest_from_toml(e->data, e->size, &ctx->a->manifest);
    ctx->got_manifest = true;
  } else if (strcmp(e->path, "files.tar.zst") == 0) {
    salt_buf_append(&ctx->a->payload, e->data, e->size);
  } else if (strncmp(e->path, "scripts/", 8) == 0 && e->typeflag != SALT_TAR_DIR) {
    salt_strlist_push(&ctx->a->scripts, e->path + 8);
  }
  return SALT_OK;
}

int salt_archive_open(const char *path, salt_archive *out) {
  salt_archive_init(out);
  salt_buf b;
  if (salt_read_file(path, &b) != SALT_OK) return SALT_ERR_IO;
  open_ctx ctx = {out, false, false};
  int rc = salt_tar_read(b.data, b.len, open_cb, &ctx);
  salt_buf_free(&b);
  if (rc != SALT_OK || !ctx.got_meta || !ctx.got_manifest) {
    salt_set_error("invalid .saltpkg: %s", path);
    salt_archive_free(out);
    return SALT_ERR_FORMAT;
  }
  return SALT_OK;
}

int salt_archive_extract_payload(const salt_archive *a, const char *dest_dir, salt_strlist *installed_paths) {
  salt_buf inner;
  int rc = salt_zst_decompress(a->payload.data, a->payload.len, &inner);
  if (rc != SALT_OK) return rc;
  rc = salt_tar_extract(inner.data, inner.len, dest_dir, installed_paths);
  salt_buf_free(&inner);
  return rc;
}

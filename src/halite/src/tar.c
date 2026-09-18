#include "salt/tar.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define TAR_BLOCK 512

struct salt_tar_writer {
  salt_buf *out;
};

static void octal(char *field, size_t width, unsigned long long val) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%0*llo", (int)(width - 1), val);
  memcpy(field, tmp, width - 1);
  field[width - 1] = '\0';
}

static void write_block(salt_buf *out, const char *block) {
  salt_buf_append(out, block, TAR_BLOCK);
}

static void fill_header(char *h, const char *name, char typeflag, unsigned mode, uint64_t size,
                        const char *linkname) {
  memset(h, 0, TAR_BLOCK);
  strncpy(h, name, 100);
  octal(h + 100, 8, mode & 07777);
  octal(h + 108, 8, 0);
  octal(h + 116, 8, 0);
  octal(h + 124, 12, size);
  octal(h + 136, 12, 0);
  h[156] = typeflag;
  if (linkname) strncpy(h + 157, linkname, 100);
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < TAR_BLOCK; i++) sum += (unsigned char)h[i];
  char chk[8];
  snprintf(chk, sizeof(chk), "%06o", sum);
  memcpy(h + 148, chk, 6);
  h[154] = '\0';
  h[155] = ' ';
}

static void write_longname(salt_buf *out, const char *name) {
  char h[TAR_BLOCK];
  size_t nl = strlen(name) + 1;
  fill_header(h, "././@LongLink", 'L', 0644, nl, NULL);
  write_block(out, h);
  size_t blocks = (nl + TAR_BLOCK - 1) / TAR_BLOCK;
  char *pad = calloc(blocks, TAR_BLOCK);
  memcpy(pad, name, nl);
  salt_buf_append(out, pad, blocks * TAR_BLOCK);
  free(pad);
}

salt_tar_writer *salt_tar_writer_new(salt_buf *out) {
  salt_tar_writer *w = calloc(1, sizeof(*w));
  if (w) w->out = out;
  return w;
}

int salt_tar_writer_add(salt_tar_writer *w, const salt_tar_entry *e) {
  if (strlen(e->path) > 100) write_longname(w->out, e->path);
  if (e->linkname && strlen(e->linkname) > 100) write_longname(w->out, e->linkname);
  char h[TAR_BLOCK];
  uint64_t size = (e->typeflag == SALT_TAR_FILE) ? e->size : 0;
  fill_header(h, e->path, e->typeflag, e->mode, size, e->linkname);
  write_block(w->out, h);
  if (e->typeflag == SALT_TAR_FILE && e->size > 0) {
    salt_buf_append(w->out, e->data, e->size);
    size_t rem = e->size % TAR_BLOCK;
    if (rem) {
      char pad[TAR_BLOCK];
      memset(pad, 0, TAR_BLOCK - rem);
      salt_buf_append(w->out, pad, TAR_BLOCK - rem);
    }
  }
  return SALT_OK;
}

int salt_tar_writer_add_file(salt_tar_writer *w, const char *archive_path, const char *src_path) {
  struct stat st;
  if (lstat(src_path, &st) != 0) {
    salt_set_error("lstat %s: %s", src_path, strerror(errno));
    return SALT_ERR_IO;
  }
  salt_tar_entry e;
  memset(&e, 0, sizeof(e));
  e.path = (char *)archive_path;
  e.mode = st.st_mode & 07777;
  if (S_ISDIR(st.st_mode)) {
    e.typeflag = SALT_TAR_DIR;
    return salt_tar_writer_add(w, &e);
  }
  if (S_ISLNK(st.st_mode)) {
    char target[1024];
    ssize_t n = readlink(src_path, target, sizeof(target) - 1);
    if (n < 0) {
      salt_set_error("readlink %s", src_path);
      return SALT_ERR_IO;
    }
    target[n] = '\0';
    e.typeflag = SALT_TAR_SYMLINK;
    e.linkname = target;
    return salt_tar_writer_add(w, &e);
  }
  salt_buf b;
  if (salt_read_file(src_path, &b) != SALT_OK) return SALT_ERR_IO;
  e.typeflag = SALT_TAR_FILE;
  e.size = b.len;
  e.data = b.data;
  int r = salt_tar_writer_add(w, &e);
  salt_buf_free(&b);
  return r;
}

int salt_tar_writer_finish(salt_tar_writer *w) {
  char zero[TAR_BLOCK * 2];
  memset(zero, 0, sizeof(zero));
  salt_buf_append(w->out, zero, sizeof(zero));
  return SALT_OK;
}

void salt_tar_writer_free(salt_tar_writer *w) {
  free(w);
}

static uint64_t parse_octal(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] >= '0' && p[i] <= '7')
      v = v * 8 + (uint64_t)(p[i] - '0');
    else if (p[i] == ' ' || p[i] == '\0')
      continue;
  }
  return v;
}

int salt_tar_read(const void *data, size_t len, salt_tar_cb cb, void *ud) {
  const char *p = data;
  const char *end = p + len;
  char *longname = NULL;
  char *longlink = NULL;
  while (p + TAR_BLOCK <= end) {
    bool zero = true;
    for (int i = 0; i < TAR_BLOCK; i++)
      if (p[i]) {
        zero = false;
        break;
      }
    if (zero) break;
    const char *h = p;
    p += TAR_BLOCK;
    uint64_t size = parse_octal(h + 124, 12);
    char typeflag = h[156];
    bool has_data =
        typeflag == 'L' || typeflag == 'K' || typeflag == SALT_TAR_FILE || typeflag == '\0';
    if (has_data && size > (uint64_t)(end - p)) {
      free(longname);
      free(longlink);
      salt_set_error("tar: entry size exceeds archive");
      return SALT_ERR_FORMAT;
    }
    if (typeflag == 'L') {
      free(longname);
      longname = malloc(size + 1);
      memcpy(longname, p, size);
      longname[size] = '\0';
      p += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
      continue;
    }
    if (typeflag == 'K') {
      free(longlink);
      longlink = malloc(size + 1);
      memcpy(longlink, p, size);
      longlink[size] = '\0';
      p += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
      continue;
    }
    salt_tar_entry e;
    memset(&e, 0, sizeof(e));
    char namebuf[256];
    char name[101];
    if (longname) {
      e.path = longname;
    } else {
      char prefix[156];
      memcpy(name, h, 100);
      name[100] = '\0';
      memcpy(prefix, h + 345, 155);
      prefix[155] = '\0';
      if (prefix[0]) {
        snprintf(namebuf, sizeof(namebuf), "%s/%s", prefix, name);
        e.path = namebuf;
      } else {
        e.path = name;
      }
    }
    char linkbuf[101];
    if (longlink) {
      e.linkname = longlink;
    } else {
      memcpy(linkbuf, h + 157, 100);
      linkbuf[100] = '\0';
      e.linkname = linkbuf;
    }
    e.typeflag = typeflag;
    e.mode = (unsigned)parse_octal(h + 100, 8);
    e.size = size;
    e.data = (typeflag == SALT_TAR_FILE || typeflag == '\0') ? p : NULL;
    int r = cb(&e, ud);
    free(longname);
    longname = NULL;
    free(longlink);
    longlink = NULL;
    if (r != SALT_OK) return r;
    if (typeflag == SALT_TAR_FILE || typeflag == '\0')
      p += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
  }
  free(longname);
  free(longlink);
  return SALT_OK;
}

typedef struct {
  const char *dest;
  salt_strlist *installed;
} extract_ctx;

static const char *strip_dot_slash(const char *path) {
  while (path[0] == '.' && path[1] == '/') {
    path += 2;
    while (*path == '/') path++;
  }
  return path;
}

static bool existing_ancestor_within(const char *dest, const char *path) {
  char *dup = salt_strdup(path);
  if (!dup) return false;
  struct stat st;
  while (lstat(dup, &st) != 0) {
    char *slash = strrchr(dup, '/');
    if (!slash || slash == dup) {
      free(dup);
      return salt_path_within_root(dest, ".");
    }
    *slash = '\0';
  }
  bool ok = salt_path_within_root(dest, dup);
  free(dup);
  return ok;
}

static int confine_parent(const char *dest, const char *full) {
  char *dup = salt_strdup(full);
  if (!dup) return SALT_ERR;
  char *slash = strrchr(dup, '/');
  const char *parent = dest;
  if (slash && slash != dup) {
    *slash = '\0';
    if (!existing_ancestor_within(dest, dup)) {
      salt_set_error("tar: %s escapes the install root", full);
      free(dup);
      return SALT_ERR_FORMAT;
    }
    if (salt_mkdirs(dup, 0755) != SALT_OK) {
      free(dup);
      return SALT_ERR_IO;
    }
    parent = dup;
  }
  bool ok = salt_path_within_root(dest, parent);
  free(dup);
  if (!ok) {
    salt_set_error("tar: %s escapes the install root", full);
    return SALT_ERR_FORMAT;
  }
  return SALT_OK;
}

static int extract_cb(const salt_tar_entry *e, void *ud) {
  extract_ctx *ctx = ud;
  const char *rel = strip_dot_slash(e->path);
  char tf = e->typeflag;
  if (!salt_path_is_confined(rel)) {
    if (tf == SALT_TAR_DIR && (strcmp(rel, "") == 0 || strcmp(rel, ".") == 0)) return SALT_OK;
    salt_set_error("tar: refusing unsafe path '%s'", e->path);
    return SALT_ERR_FORMAT;
  }
  char *full = salt_join_path(ctx->dest, rel);
  int rc = confine_parent(ctx->dest, full);
  if (rc != SALT_OK) {
    free(full);
    return rc;
  }
  struct stat st;
  if (tf == SALT_TAR_DIR) {
    if (lstat(full, &st) == 0 && S_ISLNK(st.st_mode) && !salt_path_within_root(ctx->dest, full)) {
      salt_set_error("tar: %s escapes the install root", full);
      rc = SALT_ERR_FORMAT;
    } else {
      rc = salt_mkdirs(full, e->mode ? e->mode : 0755);
    }
  } else if (tf == SALT_TAR_SYMLINK) {
    if (lstat(full, &st) == 0 && !S_ISDIR(st.st_mode)) unlink(full);
    if (symlink(e->linkname, full) != 0) {
      salt_set_error("symlink %s: %s", full, strerror(errno));
      rc = SALT_ERR_IO;
    }
  } else if (tf == SALT_TAR_FILE || tf == '0' || tf == '\0') {
    rc = salt_write_file(full, e->data, e->size, e->mode ? e->mode : 0644);
  }
  if (rc == SALT_OK && ctx->installed && tf != SALT_TAR_DIR) salt_strlist_push(ctx->installed, rel);
  free(full);
  return rc;
}

int salt_tar_extract(const void *data, size_t len, const char *dest_dir,
                     salt_strlist *installed_paths) {
  extract_ctx ctx = {dest_dir, installed_paths};
  if (salt_mkdirs(dest_dir, 0755) != SALT_OK) return SALT_ERR_IO;
  return salt_tar_read(data, len, extract_cb, &ctx);
}

#include "salt/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>

static char g_error[1024];

void salt_set_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_error, sizeof(g_error), fmt, ap);
  va_end(ap);
}

const char *salt_last_error(void) {
  return g_error[0] ? g_error : "no error";
}

void salt_buf_init(salt_buf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static int salt_buf_reserve(salt_buf *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return SALT_OK;
  size_t ncap = b->cap ? b->cap * 2 : 64;
  while (ncap < b->len + extra + 1) ncap *= 2;
  char *nd = realloc(b->data, ncap);
  if (!nd) {
    salt_set_error("out of memory");
    return SALT_ERR;
  }
  b->data = nd;
  b->cap = ncap;
  return SALT_OK;
}

int salt_buf_append(salt_buf *b, const void *p, size_t n) {
  if (salt_buf_reserve(b, n) != SALT_OK) return SALT_ERR;
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
  return SALT_OK;
}

int salt_buf_append_str(salt_buf *b, const char *s) {
  return salt_buf_append(b, s, strlen(s));
}

int salt_buf_printf(salt_buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return SALT_ERR;
  }
  if (salt_buf_reserve(b, (size_t)n) != SALT_OK) {
    va_end(ap2);
    return SALT_ERR;
  }
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->len += (size_t)n;
  return SALT_OK;
}

void salt_buf_free(salt_buf *b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

char *salt_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

char *salt_join_path(const char *a, const char *b) {
  if (!a || !a[0]) return salt_strdup(b);
  if (!b || !b[0]) return salt_strdup(a);
  size_t la = strlen(a);
  bool sep = a[la - 1] == '/';
  while (*b == '/') b++;
  size_t lb = strlen(b);
  char *r = malloc(la + lb + 2);
  if (!r) return NULL;
  memcpy(r, a, la);
  size_t o = la;
  if (!sep) r[o++] = '/';
  memcpy(r + o, b, lb + 1);
  return r;
}

int salt_mkdirs(const char *path, unsigned mode) {
  if (!path || !path[0]) return SALT_OK;
  char *tmp = salt_strdup(path);
  if (!tmp) return SALT_ERR;
  size_t len = strlen(tmp);
  if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        salt_set_error("mkdir %s: %s", tmp, strerror(errno));
        free(tmp);
        return SALT_ERR_IO;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    salt_set_error("mkdir %s: %s", tmp, strerror(errno));
    free(tmp);
    return SALT_ERR_IO;
  }
  free(tmp);
  return SALT_OK;
}

int salt_read_file(const char *path, salt_buf *out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    salt_set_error("open %s: %s", path, strerror(errno));
    return SALT_ERR_IO;
  }
  salt_buf_init(out);
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (salt_buf_append(out, chunk, n) != SALT_OK) {
      fclose(f);
      salt_buf_free(out);
      return SALT_ERR;
    }
  }
  if (ferror(f)) {
    salt_set_error("read %s: %s", path, strerror(errno));
    fclose(f);
    salt_buf_free(out);
    return SALT_ERR_IO;
  }
  fclose(f);
  return SALT_OK;
}

int salt_write_file(const char *path, const void *data, size_t len, unsigned mode) {
  char *dup = salt_strdup(path);
  if (!dup) return SALT_ERR;
  char *dir = dirname(dup);
  if (dir && strcmp(dir, ".") != 0 && strcmp(dir, "/") != 0) salt_mkdirs(dir, 0755);
  free(dup);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) {
    salt_set_error("open %s: %s", path, strerror(errno));
    return SALT_ERR_IO;
  }
  const char *p = data;
  size_t left = len;
  while (left > 0) {
    ssize_t w = write(fd, p, left);
    if (w < 0) {
      salt_set_error("write %s: %s", path, strerror(errno));
      close(fd);
      return SALT_ERR_IO;
    }
    p += w;
    left -= (size_t)w;
  }
  close(fd);
  return SALT_OK;
}

bool salt_path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

bool salt_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int salt_remove_recursive(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) return SALT_OK;
    salt_set_error("lstat %s: %s", path, strerror(errno));
    return SALT_ERR_IO;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d) {
      salt_set_error("opendir %s: %s", path, strerror(errno));
      return SALT_ERR_IO;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char *child = salt_join_path(path, e->d_name);
      int r = salt_remove_recursive(child);
      free(child);
      if (r != SALT_OK) {
        closedir(d);
        return r;
      }
    }
    closedir(d);
    if (rmdir(path) != 0) {
      salt_set_error("rmdir %s: %s", path, strerror(errno));
      return SALT_ERR_IO;
    }
  } else {
    if (unlink(path) != 0) {
      salt_set_error("unlink %s: %s", path, strerror(errno));
      return SALT_ERR_IO;
    }
  }
  return SALT_OK;
}

int salt_copy_file(const char *src, const char *dst) {
  salt_buf b;
  if (salt_read_file(src, &b) != SALT_OK) return SALT_ERR_IO;
  struct stat st;
  unsigned mode = 0644;
  if (stat(src, &st) == 0) mode = st.st_mode & 0777;
  int r = salt_write_file(dst, b.data, b.len, mode);
  salt_buf_free(&b);
  return r;
}

void salt_strlist_init(salt_strlist *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

int salt_strlist_push(salt_strlist *l, const char *s) {
  if (l->len == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 8;
    char **ni = realloc(l->items, ncap * sizeof(char *));
    if (!ni) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    l->items = ni;
    l->cap = ncap;
  }
  l->items[l->len] = salt_strdup(s);
  if (!l->items[l->len]) return SALT_ERR;
  l->len++;
  return SALT_OK;
}

bool salt_strlist_contains(const salt_strlist *l, const char *s) {
  for (size_t i = 0; i < l->len; i++)
    if (strcmp(l->items[i], s) == 0) return true;
  return false;
}

void salt_strlist_free(salt_strlist *l) {
  for (size_t i = 0; i < l->len; i++) free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

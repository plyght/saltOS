#ifndef SALT_UTIL_H
#define SALT_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALT_OK 0
#define SALT_ERR -1
#define SALT_ERR_IO -2
#define SALT_ERR_FORMAT -3
#define SALT_ERR_NOTFOUND -4
#define SALT_ERR_VERIFY -5
#define SALT_ERR_EXISTS -6
#define SALT_ERR_DEP -7
#define SALT_ERR_USAGE -8

void salt_set_error(const char *fmt, ...);
const char *salt_last_error(void);

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} salt_buf;

void salt_buf_init(salt_buf *b);
int salt_buf_append(salt_buf *b, const void *p, size_t n);
int salt_buf_append_str(salt_buf *b, const char *s);
int salt_buf_printf(salt_buf *b, const char *fmt, ...);
void salt_buf_free(salt_buf *b);

char *salt_strdup(const char *s);
char *salt_join_path(const char *a, const char *b);
int salt_mkdirs(const char *path, unsigned mode);
int salt_read_file(const char *path, salt_buf *out);
int salt_write_file(const char *path, const void *data, size_t len, unsigned mode);
bool salt_path_exists(const char *path);
bool salt_is_dir(const char *path);
int salt_remove_recursive(const char *path);
int salt_copy_file(const char *src, const char *dst);

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} salt_strlist;

void salt_strlist_init(salt_strlist *l);
int salt_strlist_push(salt_strlist *l, const char *s);
bool salt_strlist_contains(const salt_strlist *l, const char *s);
void salt_strlist_free(salt_strlist *l);

#ifdef __cplusplus
}
#endif

#endif

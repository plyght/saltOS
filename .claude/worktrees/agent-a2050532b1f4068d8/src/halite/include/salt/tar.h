#ifndef SALT_TAR_H
#define SALT_TAR_H

#include <stddef.h>
#include <stdint.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SALT_TAR_FILE '0'
#define SALT_TAR_DIR '5'
#define SALT_TAR_SYMLINK '2'

typedef struct {
  char *path;
  char typeflag;
  unsigned mode;
  uint64_t size;
  char *linkname;
  const char *data;
} salt_tar_entry;

typedef struct salt_tar_writer salt_tar_writer;
salt_tar_writer *salt_tar_writer_new(salt_buf *out);
int salt_tar_writer_add(salt_tar_writer *w, const salt_tar_entry *e);
int salt_tar_writer_add_file(salt_tar_writer *w, const char *archive_path, const char *src_path);
int salt_tar_writer_finish(salt_tar_writer *w);
void salt_tar_writer_free(salt_tar_writer *w);

typedef int (*salt_tar_cb)(const salt_tar_entry *e, void *ud);
int salt_tar_read(const void *data, size_t len, salt_tar_cb cb, void *ud);
int salt_tar_extract(const void *data, size_t len, const char *dest_dir, salt_strlist *installed_paths);

#ifdef __cplusplus
}
#endif

#endif

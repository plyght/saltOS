#ifndef SALT_ARCHIVE_H
#define SALT_ARCHIVE_H

#include "salt/util.h"
#include "salt/pkg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  salt_pkg_meta meta;
  salt_manifest manifest;
  salt_buf payload;
  salt_strlist scripts;
} salt_archive;

void salt_archive_init(salt_archive *a);
void salt_archive_free(salt_archive *a);

int salt_archive_build_from_dir(const char *staging_dir, const salt_pkg_meta *meta,
                                const char *scripts_dir, salt_archive *out);
int salt_archive_write(const salt_archive *a, const char *out_path);
int salt_archive_open(const char *path, salt_archive *out);
int salt_archive_extract_payload(const salt_archive *a, const char *dest_dir, salt_strlist *installed_paths);

#ifdef __cplusplus
}
#endif

#endif

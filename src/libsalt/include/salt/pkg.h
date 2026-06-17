#ifndef SALT_PKG_H
#define SALT_PKG_H

#include <stdint.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *summary;
  char *license;
  char *repro_status;
  char *repro_reason;
  salt_strlist deps;
} salt_pkg_meta;

typedef struct {
  char *path;
  char typeflag;
  unsigned mode;
  uint64_t size;
  char *sha256;
  char *linkname;
} salt_manifest_entry;

typedef struct {
  salt_manifest_entry *items;
  size_t len;
  size_t cap;
} salt_manifest;

void salt_pkg_meta_init(salt_pkg_meta *m);
void salt_pkg_meta_free(salt_pkg_meta *m);
int salt_pkg_meta_to_toml(const salt_pkg_meta *m, salt_buf *out);
int salt_pkg_meta_from_toml(const char *text, size_t len, salt_pkg_meta *out);

void salt_manifest_init(salt_manifest *m);
int salt_manifest_push(salt_manifest *m, const salt_manifest_entry *e);
void salt_manifest_free(salt_manifest *m);
int salt_manifest_to_toml(const salt_manifest *m, salt_buf *out);
int salt_manifest_from_toml(const char *text, size_t len, salt_manifest *out);

char *salt_pkg_filename(const salt_pkg_meta *m);

#ifdef __cplusplus
}
#endif

#endif

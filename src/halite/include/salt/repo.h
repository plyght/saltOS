#ifndef SALT_REPO_H
#define SALT_REPO_H

#include "salt/util.h"
#include "salt/pkg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *filename;
  char *sha256;
  uint64_t size;
  salt_strlist deps;
} salt_repo_entry;

typedef struct {
  salt_repo_entry *items;
  size_t len;
  size_t cap;
  char *repo_name;
  char *arch;
} salt_repo_index;

void salt_repo_index_init(salt_repo_index *idx);
void salt_repo_index_free(salt_repo_index *idx);

int salt_repo_index_load(const char *path, salt_repo_index *out);
int salt_repo_index_to_toml(const salt_repo_index *idx, salt_buf *out);
const salt_repo_entry *salt_repo_index_find(const salt_repo_index *idx, const char *name);

int salt_repo_build_index(const char *packages_dir, const char *repo_name, const char *arch,
                          salt_repo_index *out);
int salt_repo_publish(const char *out_dir, const char *repo_name, const char *arch,
                      const char *sec_key_hex);

int salt_fetch(const char *url_or_path, salt_buf *out);
int salt_fetch_to_file(const char *url_or_path, const char *dest_path);

#ifdef __cplusplus
}
#endif

#endif

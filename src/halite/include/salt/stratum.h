#ifndef SALT_STRATUM_H
#define SALT_STRATUM_H

#include <stdint.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct salt_strata_db salt_strata_db;

typedef struct {
  char *name;
  char *family;
  char *arch;
  char *root;
  char *package_manager;
  char *bootstrap;
  char *trust;
  char *state;
  bool graphics;
  bool audio;
  bool dbus;
  int64_t created;
} salt_stratum;

typedef struct {
  salt_stratum *items;
  size_t len;
  size_t cap;
} salt_stratum_list;

typedef struct {
  char *name;
  char *family;
  char *arch;
  char *root;
  char *package_manager;
  char *bootstrap;
  char *trust;
  char *rootfs_url;
  char *rootfs_sha256;
  int rootfs_strip;
  bool graphics;
  bool audio;
  bool dbus;
  salt_strlist repo_names;
  salt_strlist repo_urls;
} salt_stratum_recipe;

typedef struct {
  char *alias;
  char *stratum;
  char *command;
  char *kind;
  char *shim_path;
  int64_t created;
} salt_exposed;

typedef struct {
  salt_exposed *items;
  size_t len;
  size_t cap;
} salt_exposed_list;

typedef struct {
  char *component;
  char *provider;
  char *source;
  char *trust;
  int64_t rollback_target;
  int64_t time;
} salt_provider;

typedef struct {
  salt_provider *items;
  size_t len;
  size_t cap;
} salt_provider_list;

typedef struct {
  int64_t id;
  char *stratum;
  char *label;
  char *path;
  char *kind;
  int64_t time;
} salt_stratum_snapshot;

typedef struct {
  salt_stratum_snapshot *items;
  size_t len;
  size_t cap;
} salt_stratum_snapshot_list;

typedef struct {
  char *root;
  char *strata_root;
  char *cache_dir;
  bool use_btrfs;
} salt_strata_ctx;

void salt_stratum_free_fields(salt_stratum *s);
void salt_stratum_list_init(salt_stratum_list *l);
void salt_stratum_list_free(salt_stratum_list *l);

void salt_exposed_free_fields(salt_exposed *e);
void salt_exposed_list_init(salt_exposed_list *l);
void salt_exposed_list_free(salt_exposed_list *l);

void salt_provider_free_fields(salt_provider *p);
void salt_provider_list_init(salt_provider_list *l);
void salt_provider_list_free(salt_provider_list *l);

void salt_stratum_snapshot_free_fields(salt_stratum_snapshot *s);
void salt_stratum_snapshot_list_init(salt_stratum_snapshot_list *l);
void salt_stratum_snapshot_list_free(salt_stratum_snapshot_list *l);

void salt_stratum_recipe_init(salt_stratum_recipe *r);
void salt_stratum_recipe_free(salt_stratum_recipe *r);
int salt_stratum_recipe_load(const char *path, salt_stratum_recipe *out);
int salt_stratum_recipe_lint(const char *path, salt_buf *report);

int salt_strata_ctx_init(salt_strata_ctx *c, const char *root);
void salt_strata_ctx_free(salt_strata_ctx *c);

int salt_strata_db_open(const char *root, salt_strata_db **out);
void salt_strata_db_close(salt_strata_db *db);

int salt_stratum_register(salt_strata_db *db, const salt_stratum_recipe *r);
int salt_stratum_unregister(salt_strata_db *db, const char *name);
int salt_stratum_get(salt_strata_db *db, const char *name, salt_stratum *out);
int salt_stratum_list_all(salt_strata_db *db, salt_stratum_list *out);
int salt_stratum_set_state(salt_strata_db *db, const char *name, const char *state);

int salt_stratum_bootstrap(const salt_strata_ctx *c, salt_strata_db *db,
                           const salt_stratum_recipe *r);
int salt_stratum_destroy(const salt_strata_ctx *c, salt_strata_db *db, const char *name);

int salt_stratum_snapshot_create(const salt_strata_ctx *c, salt_strata_db *db, const char *name,
                                 const char *label, int64_t *id_out);
int salt_stratum_snapshot_list_all(salt_strata_db *db, const char *name,
                                   salt_stratum_snapshot_list *out);
int salt_stratum_rollback(const salt_strata_ctx *c, salt_strata_db *db, const char *name,
                          int64_t snap_id);

int salt_expose_add(salt_strata_db *db, const char *root, const char *stratum, const char *command,
                    const char *alias, const char *kind);
int salt_expose_pm(salt_strata_db *db, const char *root, const char *stratum, const char *binary);
int salt_expose_remove(salt_strata_db *db, const char *root, const char *alias);
int salt_expose_list(salt_strata_db *db, salt_exposed_list *out);
int salt_expose_desktop(salt_strata_db *db, const salt_stratum *s, const char *root,
                        const char *app);

int salt_provider_set(salt_strata_db *db, const char *component, const char *provider,
                      const char *source, const char *trust);
int salt_provider_get(salt_strata_db *db, const char *component, salt_provider *out);
int salt_provider_list_all(salt_strata_db *db, salt_provider_list *out);
int salt_provider_rollback(salt_strata_db *db, const char *component);

#ifdef __cplusplus
}
#endif

#endif

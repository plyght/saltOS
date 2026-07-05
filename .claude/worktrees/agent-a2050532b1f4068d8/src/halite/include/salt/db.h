#ifndef SALT_DB_H
#define SALT_DB_H

#include <stdint.h>
#include "salt/util.h"
#include "salt/pkg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct salt_db salt_db;

typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *repo;
  char *sig_status;
  int64_t install_time;
  int64_t txn_id;
} salt_db_pkg;

typedef struct {
  salt_db_pkg *items;
  size_t len;
  size_t cap;
} salt_db_pkglist;

void salt_db_pkglist_init(salt_db_pkglist *l);
void salt_db_pkglist_free(salt_db_pkglist *l);

int salt_db_open(const char *path, salt_db **out);
void salt_db_close(salt_db *db);

int salt_db_sql_begin(salt_db *db);
int salt_db_sql_commit(salt_db *db);
int salt_db_sql_rollback(salt_db *db);

int salt_db_txn_new(salt_db *db, const char *op, int64_t *txn_id_out);
int salt_db_txn_finish(salt_db *db, int64_t txn_id, const char *status);
int salt_db_txn_set_snapshot(salt_db *db, int64_t txn_id, const char *snapshot);

int salt_db_record_install(salt_db *db, const salt_pkg_meta *meta, const salt_manifest *manifest,
                           const char *repo, const char *sig_status, int64_t txn_id);
int salt_db_record_remove(salt_db *db, const char *name, int64_t txn_id);

int salt_db_get_pkg(salt_db *db, const char *name, salt_db_pkg *out);
bool salt_db_is_installed(salt_db *db, const char *name);
int salt_db_list_installed(salt_db *db, salt_db_pkglist *out);
int salt_db_search(salt_db *db, const char *term, salt_db_pkglist *out);
int salt_db_pkg_files(salt_db *db, const char *name, salt_strlist *out);
int salt_db_owner(salt_db *db, const char *path, char **owner_out);
int salt_db_pkg_manifest(salt_db *db, const char *name, salt_manifest *out);
int salt_db_revdeps(salt_db *db, const char *name, salt_strlist *out);

int salt_db_vacuum_into(salt_db *db, const char *path);
int salt_db_restore_state_from(salt_db *db, const char *before_path);

void salt_db_pkg_free_fields(salt_db_pkg *p);

#ifdef __cplusplus
}
#endif

#endif

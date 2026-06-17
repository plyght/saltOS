#ifndef SALT_TXN_H
#define SALT_TXN_H

#include "salt/util.h"
#include "salt/db.h"
#include "salt/archive.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *root;
  char *db_path;
  char *state_dir;
  char *snapshot_dir;
  bool use_btrfs;
} salt_ctx;

int salt_ctx_init(salt_ctx *ctx, const char *root);
void salt_ctx_free(salt_ctx *ctx);

typedef struct {
  int64_t id;
  char *op;
  char *status;
  int64_t time;
  char *snapshot;
} salt_deployment;

typedef struct {
  salt_deployment *items;
  size_t len;
  size_t cap;
} salt_deployment_list;

void salt_deployment_list_init(salt_deployment_list *l);
void salt_deployment_list_free(salt_deployment_list *l);

int salt_snapshot_create(const salt_ctx *ctx, salt_db *db, int64_t txn_id, char **snapshot_out);
int salt_snapshot_restore(const salt_ctx *ctx, const char *snapshot);

int salt_install_archive(salt_ctx *ctx, salt_db *db, const salt_archive *ar,
                         const char *repo, const char *sig_status, int64_t txn_id);
int salt_remove_pkg(salt_ctx *ctx, salt_db *db, const char *name, int64_t txn_id);

int salt_db_deployments(salt_db *db, salt_deployment_list *out);
int salt_db_last_ok_txn(salt_db *db, int64_t *id_out, char **snapshot_out);

int salt_deployments_list(salt_ctx *ctx, salt_db *db, salt_deployment_list *out);
int salt_txn_revert_files(const salt_ctx *ctx, int64_t txn_id);
int salt_rollback_last(salt_ctx *ctx, salt_db *db);

#ifdef __cplusplus
}
#endif

#endif

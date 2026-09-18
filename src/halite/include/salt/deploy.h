#ifndef SALT_DEPLOY_H
#define SALT_DEPLOY_H

#include "salt/txn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *name;
  char *old_version;
  char *new_version;
} salt_txn_change;

typedef struct {
  salt_txn_change *items;
  size_t len;
  size_t cap;
} salt_txn_change_list;

void salt_txn_change_list_init(salt_txn_change_list *l);
void salt_txn_change_list_free(salt_txn_change_list *l);

typedef struct {
  char *kernel;
  bool pinned;
} salt_txn_meta;

typedef struct {
  char *toplevel;
  char *root_subvol;
  char *snapshots_subvol;
  char *device;
} salt_btrfs_layout;

void salt_btrfs_layout_free(salt_btrfs_layout *l);

int salt_deploy_ensure_schema(salt_db *db);
int salt_deploy_record(const salt_ctx *ctx, salt_db *db, int64_t txn_id);
int salt_deploy_changes(salt_db *db, int64_t txn_id, salt_txn_change_list *out);
int salt_deploy_meta(salt_db *db, int64_t txn_id, salt_txn_meta *out);
void salt_txn_meta_free(salt_txn_meta *m);
int salt_deploy_pin(salt_db *db, int64_t txn_id, bool pinned);
int salt_deploy_prune(const salt_ctx *ctx, salt_db *db, int keep, salt_strlist *removed);
int salt_deploy_pick_rollback(salt_db *db, int64_t *id_out);
int salt_deploy_root_changed(const salt_ctx *ctx, int64_t txn_id, const char *prefix, bool *changed);

char *salt_boot_newest_kernel(const char *bootdir);
int salt_boot_list_kernels(const char *bootdir, salt_strlist *out);

int salt_btrfs_layout_detect(const salt_ctx *ctx, salt_btrfs_layout *out);
int salt_btrfs_mount_toplevel(const salt_btrfs_layout *l, char **mountpoint_out);
int salt_btrfs_umount_toplevel(const char *mountpoint);

int salt_rollback_to(salt_ctx *ctx, salt_db *db, int64_t txn_id, int64_t *rollback_txn_out,
                     char **new_root_out, bool *reboot_required);

#ifdef __cplusplus
}
#endif

#endif

#ifndef SALT_GC_H
#define SALT_GC_H

#include "salt/util.h"
#include "salt/db.h"
#include "salt/txn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int keep;
  const int64_t *pinned;
  size_t npinned;
  bool dry_run;
} salt_gc_opts;

typedef struct {
  size_t generations_removed;
  size_t cache_files_removed;
  uint64_t bytes_freed;
  salt_strlist actions;
} salt_gc_report;

void salt_gc_report_init(salt_gc_report *r);
void salt_gc_report_free(salt_gc_report *r);

int64_t salt_gc_booted_generation(const salt_ctx *ctx);

int salt_gc_run(salt_ctx *ctx, salt_db *db, const salt_gc_opts *opts, salt_gc_report *out);
int salt_cache_clean(const salt_ctx *ctx, const char *cache_dir, bool dry_run,
                     salt_gc_report *out);

#ifdef __cplusplus
}
#endif

#endif

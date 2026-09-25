#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/toml.h"
#include "salt/txn.h"
#include "salt/gc.h"
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int gc_keep_from_conf(const Options &o, std::vector<int64_t> &pinned_out) {
  int keep = 3;
  salt_toml *t = load_salt_conf(o.root, "salt");
  if (t) {
    keep = (int)salt_toml_int(t, "gc.keep", keep);
    const salt_toml *arr = salt_toml_path(t, "gc.pinned");
    if (arr && salt_toml_typeof(arr) == SALT_TOML_ARRAY) {
      size_t n = salt_toml_array_len(arr);
      for (size_t i = 0; i < n; i++) {
        long long id = salt_toml_as_int(salt_toml_array_at(arr, i), -1);
        if (id > 0) pinned_out.push_back(id);
      }
    }
    salt_toml_free(t);
  }
  return keep < 1 ? 1 : keep;
}

static void print_report(const salt_gc_report &r, bool dry_run) {
  for (size_t i = 0; i < r.actions.len; i++) printf("%s\n", r.actions.items[i]);
  double mb = (double)r.bytes_freed / (1024.0 * 1024.0);
  printf("%s%zu generation%s, %zu cached artifact%s, %.1f MiB\n",
         dry_run ? "would free: " : "freed: ", r.generations_removed,
         r.generations_removed == 1 ? "" : "s", r.cache_files_removed,
         r.cache_files_removed == 1 ? "" : "s", mb);
}

static const char *GC_USAGE = "usage: salt config gc [--keep N] [--pin ID]... [--dry-run]\n";

int cmd_gc(const Options &o, const std::vector<std::string> &args) {
  std::vector<int64_t> pinned;
  int keep = gc_keep_from_conf(o, pinned);
  bool dry_run = false;
  for (size_t i = 0; i < args.size(); i++) {
    const std::string &a = args[i];
    if (a == "--keep" && i + 1 < args.size()) {
      char *end = nullptr;
      long v = strtol(args[++i].c_str(), &end, 10);
      if (!end || *end || v < 1) {
        fprintf(stderr, "salt: --keep needs a positive integer\n");
        return 2;
      }
      keep = (int)v;
    } else if (a == "--pin" && i + 1 < args.size()) {
      char *end = nullptr;
      long long v = strtoll(args[++i].c_str(), &end, 10);
      if (!end || *end || v < 1) {
        fprintf(stderr, "salt: --pin needs a generation id\n");
        return 2;
      }
      pinned.push_back(v);
    } else if (a == "--dry-run" || a == "-n") {
      dry_run = true;
    } else {
      fprintf(stderr, "%s", GC_USAGE);
      return 2;
    }
  }

  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = nullptr;
  if (salt_db_open(ctx.db_path, &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_ctx_free(&ctx);
    return 1;
  }
  salt_gc_opts opts;
  opts.keep = keep;
  opts.pinned = pinned.empty() ? nullptr : pinned.data();
  opts.npinned = pinned.size();
  opts.dry_run = dry_run;
  salt_gc_report report;
  salt_gc_report_init(&report);
  int64_t booted = salt_gc_booted_generation(&ctx);
  printf("keeping the %d most recent generation%s%s\n", keep, keep == 1 ? "" : "s",
         booted > 0 ? " plus the booted one" : "");
  int rc = salt_gc_run(&ctx, db, &opts, &report);
  if (rc != SALT_OK) fprintf(stderr, "salt: gc stopped: %s\n", salt_last_error());
  print_report(report, dry_run);
  salt_gc_report_free(&report);
  salt_db_close(db);
  salt_ctx_free(&ctx);
  return rc == SALT_OK ? 0 : 1;
}

static const char *CLEAN_USAGE = "usage: salt clean [--all] [--dry-run]\n";

int cmd_clean(const Options &o, const std::vector<std::string> &args) {
  bool dry_run = false, all = false;
  for (const auto &a : args) {
    if (a == "--dry-run" || a == "-n")
      dry_run = true;
    else if (a == "--all")
      all = true;
    else {
      fprintf(stderr, "%s", CLEAN_USAGE);
      return 2;
    }
  }
  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_gc_report report;
  salt_gc_report_init(&report);
  std::string cache = cache_dir_for(o);
  int rc = salt_cache_clean(&ctx, all ? nullptr : cache.c_str(), dry_run, &report);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());
  print_report(report, dry_run);
  salt_gc_report_free(&report);
  salt_ctx_free(&ctx);
  return rc == SALT_OK ? 0 : 1;
}

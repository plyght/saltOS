#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/toml.h"
#include "salt/hash.h"
}

#include <cstdio>
#include <string>
#include <vector>

static std::string system_config_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.toml");
}

static void config_usage() {
  fprintf(stderr,
          "usage: salt config <subcommand>\n"
          "  show              print the resolved declarative config\n"
          "  apply [--relock]  converge the system to config + lock\n"
          "  diff              show config/lock vs. the live system\n"
          "  history           list generations\n"
          "  rollback [id]     restore a previous generation\n"
          "  gc [--keep N] [--dry-run]\n"
          "                    prune old generations and unreferenced artifacts\n");
}

static int config_show(const Options &o) {
  std::string p = system_config_path(o);
  salt_buf b;
  salt_buf_init(&b);
  if (salt_read_file(p.c_str(), &b) != SALT_OK) {
    fprintf(stderr, "salt: no system config at %s\n", p.c_str());
    salt_buf_free(&b);
    return 1;
  }
  if (b.data && b.len) fwrite(b.data, 1, b.len, stdout);
  salt_buf_free(&b);
  return 0;
}

static bool config_matches_lock(const Options &o, const std::string &lock_path) {
  salt_toml *t = salt_toml_parse_file(lock_path.c_str());
  if (!t) return true;
  const char *locked = salt_toml_string(t, "config_hash", nullptr);
  std::string want = locked ? locked : "";
  salt_toml_free(t);
  if (want.rfind("sha256:", 0) == 0) want = want.substr(7);
  char cur[SALT_SHA256_HEXLEN + 1] = {0};
  bool have_cfg = salt_sha256_file(system_config_path(o).c_str(), cur) == SALT_OK;
  if (want.empty()) return !have_cfg;
  return have_cfg && want == cur;
}

static int config_diff(const Options &o) {
  return lock_diff(o, lock_path_for(o, ""), false);
}

static int config_apply(const Options &o, const std::vector<std::string> &in_args) {
  std::vector<std::string> args = in_args;
  bool relock = false;
  TxnFlags f;
  static const char *usage = "usage: salt config apply [--relock] [--dry-run] [--download-only]\n";
  std::vector<std::string> rest;
  for (const auto &a : args) {
    if (a == "--relock")
      relock = true;
    else
      rest.push_back(a);
  }
  if (!parse_txn_flags(rest, f, usage)) return 2;
  if (!rest.empty()) {
    fprintf(stderr, "%s", usage);
    return 2;
  }
  std::string lp = lock_path_for(o, "");
  if (!relock && !config_matches_lock(o, lp)) {
    fprintf(stderr,
            "salt: %s changed since %s was generated; pass --relock to apply and regenerate\n",
            system_config_path(o).c_str(), lp.c_str());
    return 1;
  }
  int rc = lock_apply(o, lp, f);
  if (rc == 0 && relock && !f.dry_run && !f.download_only) rc = lock_write(o, lp, false);
  return rc;
}

int cmd_config(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    config_usage();
    return 2;
  }
  const std::string &sub = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (sub == "show") return config_show(o);
  if (sub == "diff") return config_diff(o);
  if (sub == "apply") return config_apply(o, rest);
  if (sub == "history") return cmd_deployments(o, rest);
  if (sub == "rollback") return cmd_rollback(o, rest);
  if (sub == "gc") return cmd_gc(o, rest);
  config_usage();
  return 2;
}

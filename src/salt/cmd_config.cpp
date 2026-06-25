#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/toml.h"
}

#include <cstdio>
#include <set>
#include <string>
#include <vector>

static std::string system_config_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.toml");
}

static std::string system_lock_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.lock.toml");
}

static void config_usage() {
  fprintf(stderr,
          "usage: salt config <subcommand>\n"
          "  show              print the resolved declarative config\n"
          "  apply [--relock]  converge the system to config + lock\n"
          "  diff              show config/lock vs. the live system\n"
          "  history           list generations\n"
          "  rollback [id]     restore a previous generation\n"
          "  gc [--keep N]     prune old generations and unreferenced artifacts\n");
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

static std::vector<std::string> lock_array_names(const salt_toml *root, const char *key) {
  std::vector<std::string> names;
  const salt_toml *arr = salt_toml_get(root, key);
  if (!arr || salt_toml_typeof(arr) != SALT_TOML_ARRAY) return names;
  size_t n = salt_toml_array_len(arr);
  for (size_t i = 0; i < n; i++) {
    const salt_toml *t = salt_toml_array_at(arr, i);
    const char *nm = salt_toml_string(t, "name", nullptr);
    if (nm) names.push_back(nm);
  }
  return names;
}

static int config_diff(const Options &o) {
  std::string lp = system_lock_path(o);
  salt_toml *root = salt_toml_parse_file(lp.c_str());
  if (!root) {
    fprintf(stderr, "salt: no lockfile at %s (run 'salt lock')\n", lp.c_str());
    return 1;
  }
  std::vector<std::string> locked = lock_array_names(root, "native");
  salt_toml_free(root);

  salt_db *db = nullptr;
  std::set<std::string> installed;
  if (salt_db_open(db_path_for(o).c_str(), &db) == SALT_OK) {
    salt_db_pkglist l;
    salt_db_pkglist_init(&l);
    salt_db_list_installed(db, &l);
    for (size_t i = 0; i < l.len; i++) installed.insert(l.items[i].name);
    salt_db_pkglist_free(&l);
    salt_db_close(db);
  }

  std::set<std::string> lockset(locked.begin(), locked.end());
  int diffs = 0;
  for (const auto &n : locked)
    if (!installed.count(n)) {
      printf("+ native %s (in lock, not installed)\n", n.c_str());
      diffs++;
    }
  for (const auto &n : installed)
    if (!lockset.count(n)) {
      printf("- native %s (installed, not in lock)\n", n.c_str());
      diffs++;
    }
  if (!diffs) printf("system matches the lockfile (native plane)\n");
  return 0;
}

static int config_apply(const Options &o, const std::vector<std::string> &args) {
  for (const auto &a : args)
    if (a != "--relock") {
      fprintf(stderr, "usage: salt config apply [--relock]\n");
      return 2;
    }

  std::string lp = system_lock_path(o);
  salt_toml *root = salt_toml_parse_file(lp.c_str());
  if (!root) {
    fprintf(stderr, "salt: no lockfile at %s (run 'salt lock')\n", lp.c_str());
    return 1;
  }

  std::vector<std::string> native = lock_array_names(root, "native");

  salt_db *db = nullptr;
  std::set<std::string> installed;
  if (salt_db_open(db_path_for(o).c_str(), &db) == SALT_OK) {
    salt_db_pkglist l;
    salt_db_pkglist_init(&l);
    salt_db_list_installed(db, &l);
    for (size_t i = 0; i < l.len; i++) installed.insert(l.items[i].name);
    salt_db_pkglist_free(&l);
    salt_db_close(db);
  }

  std::vector<std::string> to_install;
  for (const auto &n : native)
    if (!installed.count(n)) to_install.push_back(n);

  int rc = 0;
  if (!to_install.empty()) {
    printf("native: installing %zu package(s) to match lock\n", to_install.size());
    rc = cmd_install(o, to_install);
  } else {
    printf("native: already matches lock\n");
  }

  const salt_toml *strata = salt_toml_get(root, "stratum");
  if (strata && salt_toml_typeof(strata) == SALT_TOML_ARRAY) {
    size_t n = salt_toml_array_len(strata);
    for (size_t i = 0; i < n; i++) {
      const salt_toml *st = salt_toml_array_at(strata, i);
      const char *nm = salt_toml_string(st, "name", nullptr);
      if (!nm) continue;
      printf("stratum: ensuring %s\n", nm);
      if (ensure_stratum(o, nm) != 0) {
        rc = 1;
        continue;
      }
      std::vector<std::string> pkgs;
      const salt_toml *parr = salt_toml_get(st, "package");
      if (parr && salt_toml_typeof(parr) == SALT_TOML_ARRAY) {
        size_t pn = salt_toml_array_len(parr);
        for (size_t j = 0; j < pn; j++) {
          const char *pnm = salt_toml_string(salt_toml_array_at(parr, j), "name", nullptr);
          if (pnm) pkgs.push_back(pnm);
        }
      }
      if (!pkgs.empty() && stratum_install(o, nm, pkgs) != 0) rc = 1;
    }
  }

  salt_toml_free(root);
  fprintf(stderr,
          "note: apply converges to the locked package set; exact-version and "
          "repo-snapshot pinning and convergent removal are not yet enforced\n");
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
  if (sub == "gc") {
    fprintf(stderr, "salt config gc: not yet implemented\n");
    return 3;
  }
  config_usage();
  return 2;
}

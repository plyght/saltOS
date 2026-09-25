#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/toml.h"
#include "salt/hash.h"
#include "salt/stratum.h"
#include "salt/run.h"
}

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::string system_config_path(const Options &o) {
  std::string lua = path_join(o.root, "etc/salt/system.lua");
  std::string legacy = path_join(o.root, "etc/salt/system.toml");
  if (access(lua.c_str(), F_OK) != 0 && access(legacy.c_str(), F_OK) == 0) return legacy;
  return lua;
}

static void config_usage() {
  fprintf(stderr,
          "usage: salt config <subcommand>\n"
          "  show              print the resolved declarative config\n"
          "  check             validate the config without changing anything\n"
          "  apply [--relock] [--dry-run] [--download-only] [--allow-unverified]\n"
          "                    converge the system to config + lock\n"
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

struct StratumConfig {
  std::string name;
  std::string recipe;
  std::vector<std::string> packages;
};

struct ExposeConfig {
  std::string stratum;
  std::string command;
  std::string alias;
  bool desktop = false;
};

struct SystemConfig {
  bool have_native = false;
  std::string repo;
  std::vector<std::string> packages;
  std::map<std::string, std::string> pins;
  bool have_strata = false;
  std::vector<StratumConfig> strata;
  bool have_expose = false;
  std::vector<ExposeConfig> expose;
  bool require_signed_native = false;
  bool allow_unverified_repro = false;
  std::string on_missing_artifact = "fail";
};

static bool toml_strings(const salt_toml *arr, const std::string &what,
                         std::vector<std::string> &out, std::string &err) {
  if (salt_toml_typeof(arr) != SALT_TOML_ARRAY) {
    err = what + " must be an array of strings";
    return false;
  }
  std::set<std::string> seen;
  for (size_t i = 0; i < salt_toml_array_len(arr); i++) {
    const salt_toml *v = salt_toml_array_at(arr, i);
    const char *s = salt_toml_as_string(v);
    if (!s || !s[0]) {
      err = what + " must contain only non-empty strings";
      return false;
    }
    if (!seen.insert(s).second) {
      err = what + " lists " + s + " twice";
      return false;
    }
    out.push_back(s);
  }
  return true;
}

static bool config_check_keys(const salt_toml *tab, const std::string &what,
                              const std::set<std::string> &allowed, std::string &err) {
  for (size_t i = 0; i < salt_toml_table_len(tab); i++) {
    const char *k = salt_toml_table_key(tab, i);
    if (!allowed.count(k)) {
      err = what + " has unknown key '" + k + "'";
      return false;
    }
  }
  return true;
}

static bool config_load(const std::string &path, SystemConfig &cfg, std::string &err) {
  salt_toml *t = salt_toml_parse_file(path.c_str());
  if (!t) {
    err = path + ": " + salt_last_error();
    return false;
  }
  bool ok = false;
  do {
    if (!config_check_keys(
            t, "system.lua",
            {"schema", "system", "kernel", "stratum", "native", "strata", "expose", "policy"}, err))
      break;
    if (salt_toml_get(t, "schema") && salt_toml_int(t, "schema", 0) != 1) {
      err = "unsupported schema " + std::to_string(salt_toml_int(t, "schema", 0)) +
            " (this salt understands schema = 1)";
      break;
    }
    const salt_toml *nat = salt_toml_get(t, "native");
    if (nat) {
      if (salt_toml_typeof(nat) != SALT_TOML_TABLE) {
        err = "[native] must be a table";
        break;
      }
      if (!config_check_keys(nat, "[native]", {"repo", "packages", "pin"}, err)) break;
      cfg.have_native = true;
      cfg.repo = salt_toml_string(nat, "repo", "");
      const salt_toml *pk = salt_toml_get(nat, "packages");
      if (!pk) {
        err = "[native] must declare packages = [...]";
        break;
      }
      if (!toml_strings(pk, "native.packages", cfg.packages, err)) break;
      const salt_toml *pin = salt_toml_get(nat, "pin");
      if (pin) {
        if (salt_toml_typeof(pin) != SALT_TOML_TABLE) {
          err = "[native.pin] must be a table of name = \"version[-release]\"";
          break;
        }
        bool bad = false;
        for (size_t i = 0; i < salt_toml_table_len(pin); i++) {
          const char *k = salt_toml_table_key(pin, i);
          const char *v = salt_toml_as_string(salt_toml_table_val(pin, i));
          if (!v || !v[0]) {
            err = std::string("[native.pin] ") + k + " must be a non-empty version string";
            bad = true;
            break;
          }
          cfg.pins[k] = v;
        }
        if (bad) break;
      }
    }
    const salt_toml *strata = salt_toml_get(t, "strata");
    if (strata) {
      if (salt_toml_typeof(strata) != SALT_TOML_ARRAY) {
        err = "[[strata]] must be an array of tables";
        break;
      }
      cfg.have_strata = true;
      bool bad = false;
      std::set<std::string> names;
      for (size_t i = 0; i < salt_toml_array_len(strata); i++) {
        const salt_toml *s = salt_toml_array_at(strata, i);
        std::string what = "[[strata]] #" + std::to_string(i + 1);
        if (salt_toml_typeof(s) != SALT_TOML_TABLE) {
          err = what + " must be a table";
          bad = true;
          break;
        }
        if (!config_check_keys(s, what, {"name", "recipe", "packages"}, err)) {
          bad = true;
          break;
        }
        StratumConfig sc;
        sc.name = salt_toml_string(s, "name", "");
        if (sc.name.empty()) {
          err = what + " needs name = \"...\"";
          bad = true;
          break;
        }
        if (!names.insert(sc.name).second) {
          err = "[[strata]] declares " + sc.name + " twice";
          bad = true;
          break;
        }
        sc.recipe = salt_toml_string(s, "recipe", sc.name.c_str());
        const salt_toml *pk = salt_toml_get(s, "packages");
        if (pk && !toml_strings(pk, "strata." + sc.name + ".packages", sc.packages, err)) {
          bad = true;
          break;
        }
        cfg.strata.push_back(sc);
      }
      if (bad) break;
    }
    const salt_toml *ex = salt_toml_get(t, "expose");
    if (ex) {
      if (salt_toml_typeof(ex) != SALT_TOML_TABLE) {
        err = "[expose] must be a table of \"stratum/command\" = alias | { ... }";
        break;
      }
      cfg.have_expose = true;
      bool bad = false;
      std::set<std::string> aliases;
      for (size_t i = 0; i < salt_toml_table_len(ex); i++) {
        std::string key = salt_toml_table_key(ex, i);
        const salt_toml *v = salt_toml_table_val(ex, i);
        size_t slash = key.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= key.size()) {
          err = "[expose] key '" + key + "' must be \"stratum/command\"";
          bad = true;
          break;
        }
        ExposeConfig ec;
        ec.stratum = key.substr(0, slash);
        ec.command = key.substr(slash + 1);
        if (salt_toml_typeof(v) == SALT_TOML_STRING) {
          ec.alias = salt_toml_as_string(v);
        } else if (salt_toml_typeof(v) == SALT_TOML_TABLE) {
          if (!config_check_keys(v, "[expose] " + key, {"alias", "desktop"}, err)) {
            bad = true;
            break;
          }
          ec.alias = salt_toml_string(v, "alias", "");
          const salt_toml *d = salt_toml_get(v, "desktop");
          if (d && salt_toml_typeof(d) != SALT_TOML_BOOL) {
            err = "[expose] " + key + ": desktop must be true or false";
            bad = true;
            break;
          }
          ec.desktop = salt_toml_as_bool(d, false);
        } else {
          err = "[expose] " + key + " must be an alias string or an inline table";
          bad = true;
          break;
        }
        if (ec.alias.empty()) ec.alias = ec.command;
        if (ec.alias.find('/') != std::string::npos) {
          err = "[expose] " + key + ": alias '" + ec.alias + "' must be a bare command name";
          bad = true;
          break;
        }
        if (!aliases.insert(ec.alias).second) {
          err = "[expose] exposes alias '" + ec.alias + "' twice";
          bad = true;
          break;
        }
        cfg.expose.push_back(ec);
      }
      if (bad) break;
    }
    const salt_toml *pol = salt_toml_get(t, "policy");
    if (pol) {
      if (salt_toml_typeof(pol) != SALT_TOML_TABLE) {
        err = "[policy] must be a table";
        break;
      }
      if (!config_check_keys(
              pol, "[policy]",
              {"require_signed_native", "allow_unverified_repro", "on_missing_artifact"}, err))
        break;
      const salt_toml *b = salt_toml_get(pol, "require_signed_native");
      if (b && salt_toml_typeof(b) != SALT_TOML_BOOL) {
        err = "[policy] require_signed_native must be true or false";
        break;
      }
      cfg.require_signed_native = salt_toml_as_bool(b, false);
      b = salt_toml_get(pol, "allow_unverified_repro");
      if (b && salt_toml_typeof(b) != SALT_TOML_BOOL) {
        err = "[policy] allow_unverified_repro must be true or false";
        break;
      }
      cfg.allow_unverified_repro = salt_toml_as_bool(b, false);
      cfg.on_missing_artifact = salt_toml_string(pol, "on_missing_artifact", "fail");
      if (cfg.on_missing_artifact != "fail" && cfg.on_missing_artifact != "skip") {
        err = "[policy] on_missing_artifact must be \"fail\" or \"skip\" (got \"" +
              cfg.on_missing_artifact + "\")";
        break;
      }
    }
    ok = true;
  } while (false);
  salt_toml_free(t);
  return ok;
}

static const salt_repo_entry *find_pinned(const salt_repo_index &idx, const std::string &name,
                                          const std::map<std::string, std::string> &pins,
                                          std::string &why) {
  auto it = pins.find(name);
  if (it == pins.end()) {
    const salt_repo_entry *e = salt_repo_index_find(&idx, name.c_str());
    if (!e) why = "not in the repository index";
    return e;
  }
  std::string ver = it->second;
  int rel = -1;
  size_t dash = ver.rfind('-');
  if (dash != std::string::npos && dash + 1 < ver.size()) {
    bool digits = true;
    for (size_t i = dash + 1; i < ver.size(); i++)
      digits = digits && isdigit((unsigned char)ver[i]);
    if (digits) {
      rel = atoi(ver.c_str() + dash + 1);
      ver.resize(dash);
    }
  }
  const salt_repo_entry *best = nullptr;
  for (size_t i = 0; i < idx.len; i++) {
    const salt_repo_entry *e = &idx.items[i];
    if (strcmp(e->name, name.c_str()) != 0 || strcmp(e->version, ver.c_str()) != 0) continue;
    if (rel >= 0 && e->release != rel) continue;
    if (!best || e->release > best->release) best = e;
  }
  if (!best) why = "pinned to " + it->second + ", which the repository index does not offer";
  return best;
}

static void resolve_cfg(const salt_repo_index &idx, const SystemConfig &cfg,
                        const std::string &name, const std::string &wanted_by,
                        std::map<std::string, const salt_repo_entry *> &desired,
                        std::vector<std::string> &problems) {
  if (desired.count(name)) return;
  std::string why;
  const salt_repo_entry *e = find_pinned(idx, name, cfg.pins, why);
  if (!e) {
    if (wanted_by.empty())
      problems.push_back("native package " + name + " is " + why);
    else
      problems.push_back("dependency " + name + " of " + wanted_by + " is " + why);
    return;
  }
  desired[name] = e;
  for (size_t i = 0; i < e->deps.len; i++)
    resolve_cfg(idx, cfg, e->deps.items[i], name, desired, problems);
}

static int apply_native(const Options &o, const SystemConfig &cfg, const TxnFlags &f) {
  RepoConf c = load_repo_conf(o);
  if (!cfg.repo.empty()) c.name = cfg.repo;
  salt_repo_index idx;
  if (salt_repo_index_load(index_path_for(o).c_str(), &idx) != SALT_OK) {
    fprintf(stderr, "salt: no repository index; run 'salt sync' first\n");
    return 1;
  }
  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = nullptr;
  if (salt_db_open(ctx.db_path, &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }

  std::map<std::string, const salt_repo_entry *> desired;
  bool ok = true;
  for (const auto &root : cfg.packages) {
    std::map<std::string, const salt_repo_entry *> sub;
    std::vector<std::string> problems;
    resolve_cfg(idx, cfg, root, "", sub, problems);
    if (!problems.empty()) {
      for (auto &p : problems)
        fprintf(stderr, "salt: %s%s\n", cfg.on_missing_artifact == "skip" ? "skipping: " : "",
                p.c_str());
      if (cfg.on_missing_artifact != "skip") ok = false;
      continue;
    }
    desired.insert(sub.begin(), sub.end());
  }
  for (const auto &kv : cfg.pins)
    if (!desired.count(kv.first) && ok) {
      fprintf(stderr,
              "salt: [native.pin] %s = \"%s\" pins a package that is not part of the "
              "declared native set\n",
              kv.first.c_str(), kv.second.c_str());
      ok = false;
    }
  if (!ok) {
    fprintf(stderr, "salt: cannot resolve [native] (policy on_missing_artifact = \"%s\")\n",
            cfg.on_missing_artifact.c_str());
    salt_db_close(db);
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }

  NativePlan plan;
  size_t matched = 0;
  for (const auto &kv : desired) {
    salt_db_pkg cur;
    bool have = salt_db_get_pkg(db, kv.first.c_str(), &cur) == SALT_OK;
    bool same =
        have && strcmp(cur.version, kv.second->version) == 0 && cur.release == kv.second->release;
    if (have) salt_db_pkg_free_fields(&cur);
    if (same)
      matched++;
    else
      plan.install.push_back(kv.second);
  }
  order_by_deps(idx, plan.install);
  salt_db_pkglist inst;
  salt_db_pkglist_init(&inst);
  salt_db_list_installed(db, &inst);
  for (size_t i = 0; i < inst.len; i++)
    if (!desired.count(inst.items[i].name)) plan.remove.push_back(inst.items[i].name);
  salt_db_pkglist_free(&inst);

  int rc = 0;
  if (plan.install.empty() && plan.remove.empty())
    printf("native: already matches system.lua (%zu package%s)\n", matched,
           matched == 1 ? "" : "s");
  else
    rc = native_transaction(o, c, &ctx, db, plan, "config-apply", f);
  salt_db_close(db);
  salt_repo_index_free(&idx);
  salt_ctx_free(&ctx);
  return rc;
}

static int apply_strata_cfg(const Options &o, const SystemConfig &cfg, const TxnFlags &f) {
  int rc = 0;
  for (const auto &sc : cfg.strata) {
    salt_strata_db *db = nullptr;
    if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    salt_stratum s;
    memset(&s, 0, sizeof(s));
    bool present = salt_stratum_get(db, sc.name.c_str(), &s) == SALT_OK;
    salt_strata_db_close(db);
    if (!present) {
      salt_stratum_free_fields(&s);
      if (f.dry_run) {
        printf("stratum %s: would bootstrap from recipe %s\n", sc.name.c_str(), sc.recipe.c_str());
        for (const auto &p : sc.packages)
          printf("stratum %s: would install %s\n", sc.name.c_str(), p.c_str());
        continue;
      }
      if (ensure_stratum(o, sc.name, sc.recipe) != 0) {
        rc = 1;
        continue;
      }
      if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK ||
          salt_stratum_get(db, sc.name.c_str(), &s) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        if (db) salt_strata_db_close(db);
        rc = 1;
        continue;
      }
      salt_strata_db_close(db);
    }
    salt_foreign_pkg_list live;
    salt_foreign_pkg_list_init(&live);
    int q = salt_stratum_pkg_query(&s, &live);
    salt_stratum_free_fields(&s);
    if (q != SALT_OK) {
      fprintf(stderr, "salt: stratum %s: %s\n", sc.name.c_str(), salt_last_error());
      salt_foreign_pkg_list_free(&live);
      rc = 1;
      continue;
    }
    std::vector<std::string> missing;
    for (const auto &p : sc.packages)
      if (!salt_foreign_pkg_list_find(&live, p.c_str())) missing.push_back(p);
    salt_foreign_pkg_list_free(&live);
    if (missing.empty()) {
      printf("stratum %s: already has %zu declared package%s\n", sc.name.c_str(),
             sc.packages.size(), sc.packages.size() == 1 ? "" : "s");
      continue;
    }
    if (f.dry_run) {
      for (const auto &p : missing)
        printf("stratum %s: would install %s\n", sc.name.c_str(), p.c_str());
      continue;
    }
    if (stratum_install(o, sc.name, missing) != 0) rc = 1;
  }
  return rc;
}

static int apply_expose(const Options &o, const SystemConfig &cfg, const TxnFlags &f) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_exposed_list cur;
  salt_exposed_list_init(&cur);
  salt_expose_list(db, &cur);
  int rc = 0;
  size_t matched = 0;
  std::set<std::string> declared;
  for (const auto &ec : cfg.expose) {
    declared.insert(ec.alias);
    const salt_exposed *have = nullptr;
    for (size_t i = 0; i < cur.len; i++)
      if (ec.alias == cur.items[i].alias) have = &cur.items[i];
    bool same = have && have->stratum && have->command && ec.stratum == have->stratum &&
                ec.command == have->command && have->kind && strcmp(have->kind, "config") == 0 &&
                have->shim_path && salt_path_exists(have->shim_path);
    std::string desktop_rel =
        "usr/local/share/applications/" + ec.stratum + "-" + ec.command + ".desktop";
    if (ec.desktop && same) same = salt_path_exists(path_join(o.root, desktop_rel).c_str());
    if (same) {
      matched++;
      continue;
    }
    if (f.dry_run) {
      printf("expose: would expose %s/%s as %s%s\n", ec.stratum.c_str(), ec.command.c_str(),
             ec.alias.c_str(), ec.desktop ? " (+desktop entry)" : "");
      continue;
    }
    salt_stratum s;
    memset(&s, 0, sizeof(s));
    if (salt_stratum_get(db, ec.stratum.c_str(), &s) != SALT_OK) {
      fprintf(stderr, "salt: [expose] %s/%s: unknown stratum %s\n", ec.stratum.c_str(),
              ec.command.c_str(), ec.stratum.c_str());
      rc = 1;
      continue;
    }
    if (ec.desktop && salt_expose_desktop(db, &s, o.root.c_str(), ec.command.c_str()) != SALT_OK) {
      fprintf(stderr, "salt: [expose] %s/%s: %s\n", ec.stratum.c_str(), ec.command.c_str(),
              salt_last_error());
      rc = 1;
    } else if (salt_expose_add(db, o.root.c_str(), ec.stratum.c_str(), ec.command.c_str(),
                               ec.alias.c_str(), "config") != SALT_OK) {
      fprintf(stderr, "salt: [expose] %s/%s: %s\n", ec.stratum.c_str(), ec.command.c_str(),
              salt_last_error());
      rc = 1;
    } else {
      printf("exposed %s/%s as %s%s\n", ec.stratum.c_str(), ec.command.c_str(), ec.alias.c_str(),
             ec.desktop ? " (+desktop entry)" : "");
    }
    salt_stratum_free_fields(&s);
  }
  for (size_t i = 0; i < cur.len; i++) {
    const salt_exposed &e = cur.items[i];
    if (!e.kind || strcmp(e.kind, "config") != 0 || declared.count(e.alias)) continue;
    if (f.dry_run) {
      printf("expose: would unexpose %s (no longer in [expose])\n", e.alias);
      continue;
    }
    if (salt_expose_remove(db, o.root.c_str(), e.alias) != SALT_OK) {
      fprintf(stderr, "salt: unexpose %s: %s\n", e.alias, salt_last_error());
      rc = 1;
      continue;
    }
    std::string desktop = path_join(o.root, std::string("usr/local/share/applications/") +
                                                e.stratum + "-" + e.command + ".desktop");
    if (salt_path_exists(desktop.c_str())) remove(desktop.c_str());
    printf("unexposed %s (no longer in [expose])\n", e.alias);
  }
  if (rc == 0 && matched == cfg.expose.size() && !f.dry_run)
    printf("expose: already matches system.lua (%zu alias%s)\n", matched, matched == 1 ? "" : "es");
  salt_exposed_list_free(&cur);
  salt_strata_db_close(db);
  return rc;
}

static int config_check(const Options &o) {
  SystemConfig cfg;
  std::string err;
  std::string cp = system_config_path(o);
  if (!salt_path_exists(cp.c_str())) {
    fprintf(stderr, "salt: no system config at %s\n", cp.c_str());
    return 1;
  }
  if (!config_load(cp, cfg, err)) {
    fprintf(stderr, "salt: %s: %s\n", cp.c_str(), err.c_str());
    return 1;
  }
  printf(
      "%s: ok (%s%zu native root%s, %zu pin%s, %zu strat%s, %zu exposure%s, policy "
      "require_signed_native=%s allow_unverified_repro=%s on_missing_artifact=%s)\n",
      cp.c_str(), cfg.have_native ? "" : "native unmanaged, ", cfg.packages.size(),
      cfg.packages.size() == 1 ? "" : "s", cfg.pins.size(), cfg.pins.size() == 1 ? "" : "s",
      cfg.strata.size(), cfg.strata.size() == 1 ? "um" : "a", cfg.expose.size(),
      cfg.expose.size() == 1 ? "" : "s", cfg.require_signed_native ? "true" : "false",
      cfg.allow_unverified_repro ? "true" : "false", cfg.on_missing_artifact.c_str());
  return 0;
}

static int config_apply(const Options &o, const std::vector<std::string> &in_args) {
  std::vector<std::string> args = in_args;
  bool relock = false;
  TxnFlags f;
  static const char *usage =
      "usage: salt config apply [--relock] [--dry-run] [--download-only] [--allow-unverified]\n";
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
  std::string cp = system_config_path(o);
  std::string lp = lock_path_for(o, "");
  bool have_cfg = salt_path_exists(cp.c_str());
  bool have_lock = salt_path_exists(lp.c_str());
  if (!have_cfg && !have_lock) {
    fprintf(stderr, "salt: no system config at %s and no lockfile at %s\n", cp.c_str(), lp.c_str());
    return 1;
  }
  SystemConfig cfg;
  std::string err;
  if (have_cfg && !config_load(cp, cfg, err)) {
    fprintf(stderr, "salt: %s: %s\n", cp.c_str(), err.c_str());
    return 1;
  }
  if (cfg.allow_unverified_repro) f.allow_unverified = true;
  if (cfg.require_signed_native) {
    RepoConf c = load_repo_conf(o);
    if (!index_signed_ok(o, c)) {
      fprintf(
          stderr,
          "salt: [policy] require_signed_native = true but the repository index at %s has no "
          "valid signature for the trusted key; run 'salt sync' or fix the trust configuration\n",
          index_path_for(o).c_str());
      return 1;
    }
  }

  bool use_lock = have_lock && !relock;
  if (use_lock && !config_matches_lock(o, lp)) {
    fprintf(stderr,
            "salt: %s changed since %s was generated; pass --relock to apply and regenerate\n",
            cp.c_str(), lp.c_str());
    return 1;
  }

  int rc = 0;
  if (use_lock) {
    rc = lock_apply(o, lp, f);
  } else {
    if (cfg.have_native) rc = apply_native(o, cfg, f);
    if (rc == 0 && !f.download_only && cfg.have_strata) rc = apply_strata_cfg(o, cfg, f);
  }
  if (rc == 0 && !f.download_only && cfg.have_expose) rc = apply_expose(o, cfg, f);
  if (rc == 0 && !use_lock && !f.dry_run && !f.download_only) rc = lock_write(o, lp, false);
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
  if (sub == "check") return config_check(o);
  if (sub == "diff") return config_diff(o);
  if (sub == "apply") return config_apply(o, rest);
  if (sub == "history") return cmd_deployments(o, rest);
  if (sub == "rollback") return cmd_rollback(o, rest);
  if (sub == "gc") return cmd_gc(o, rest);
  config_usage();
  return 2;
}

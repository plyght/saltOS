#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/repo.h"
#include "salt/hash.h"
#include "salt/toml.h"
#include "salt/txn.h"
#include "salt/stratum.h"
}

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::string iso_now() {
  time_t t = time(nullptr);
  struct tm g {};
  gmtime_r(&t, &g);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
  return std::string(buf);
}

std::string lock_path_for(const Options &o, const std::string &override_path) {
  if (!override_path.empty()) return override_path;
  return path_join(o.root, "etc/salt/system.lock.toml");
}

static std::string system_config_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.toml");
}

static std::string strip_sha_prefix(const std::string &s) {
  if (s.rfind("sha256:", 0) == 0) return s.substr(7);
  return s;
}

static int emit_native(const Options &o, salt_buf *out, int *count) {
  salt_db *db = nullptr;
  if (salt_db_open(db_path_for(o).c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }

  salt_repo_index idx;
  bool have_index = salt_repo_index_load(index_path_for(o).c_str(), &idx) == SALT_OK;

  char index_hash[SALT_SHA256_HEXLEN + 1] = {0};
  bool have_index_hash = salt_sha256_file(index_path_for(o).c_str(), index_hash) == SALT_OK;
  if (have_index_hash) salt_buf_printf(out, "repo_index_sha256 = \"sha256:%s\"\n\n", index_hash);

  salt_db_pkglist installed;
  salt_db_pkglist_init(&installed);
  salt_db_list_installed(db, &installed);

  int rc = 0;
  for (size_t i = 0; i < installed.len; i++) {
    const salt_db_pkg &p = installed.items[i];
    const salt_repo_entry *e =
        have_index ? salt_repo_index_find_exact(&idx, p.name, p.version, p.release) : nullptr;
    std::string sha, filename;
    if (salt_sha256_hex_valid(p.sha256)) {
      sha = p.sha256;
      filename = p.filename ? p.filename : "";
    } else if (e && salt_repo_entry_hash_ok(e)) {
      sha = e->sha256;
      filename = e->filename;
    } else {
      fprintf(stderr,
              "salt: cannot lock %s %s-%d: no verified sha256 is recorded for it and the "
              "repository index does not carry that exact version\n",
              p.name, p.version ? p.version : "", p.release);
      rc = 1;
      continue;
    }

    salt_buf_printf(out, "[[native]]\nname = \"%s\"\n", p.name);
    salt_buf_printf(out, "version = \"%s\"\nrelease = %d\n", p.version ? p.version : "", p.release);
    salt_buf_printf(out, "arch = \"%s\"\n", p.arch ? p.arch : "");
    salt_buf_printf(out, "grain_sha256 = \"sha256:%s\"\n", sha.c_str());
    if (!filename.empty()) salt_buf_printf(out, "filename = \"%s\"\n", filename.c_str());
    salt_buf_printf(out, "repo = \"%s\"\n", p.repo ? p.repo : "current");
    salt_strlist deps;
    salt_strlist_init(&deps);
    salt_db_pkg_deps(db, p.name, &deps);
    if (deps.len) {
      salt_buf_append_str(out, "deps = [");
      for (size_t d = 0; d < deps.len; d++)
        salt_buf_printf(out, "%s\"%s\"", d ? ", " : "", deps.items[d]);
      salt_buf_append_str(out, "]\n");
    }
    salt_strlist_free(&deps);
    salt_buf_append_str(out, "\n");
    (*count)++;
  }

  salt_db_pkglist_free(&installed);
  if (have_index) salt_repo_index_free(&idx);
  salt_db_close(db);
  return rc;
}

static void emit_strata(const Options &o, salt_buf *out, int *count) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) return;

  salt_stratum_list list;
  salt_stratum_list_init(&list);
  if (salt_stratum_list_all(db, &list) != SALT_OK) {
    salt_stratum_list_free(&list);
    salt_strata_db_close(db);
    return;
  }

  for (size_t i = 0; i < list.len; i++) {
    const salt_stratum &s = list.items[i];
    salt_buf_printf(out, "[[stratum]]\nname = \"%s\"\n", s.name ? s.name : "");
    if (s.family) salt_buf_printf(out, "family = \"%s\"\n", s.family);
    if (s.package_manager) salt_buf_printf(out, "package_manager = \"%s\"\n", s.package_manager);

    std::string rp = resolve_stratum_recipe(o, s.name ? s.name : "");
    if (!rp.empty()) {
      salt_stratum_recipe r;
      salt_stratum_recipe_init(&r);
      if (salt_stratum_recipe_load(rp.c_str(), &r) == SALT_OK) {
        if (r.rootfs_url && r.rootfs_url[0])
          salt_buf_printf(out, "bootstrap_url = \"%s\"\n", r.rootfs_url);
        if (r.rootfs_sha256 && r.rootfs_sha256[0])
          salt_buf_printf(out, "bootstrap_sha256 = \"%s\"\n", r.rootfs_sha256);
        for (size_t u = 0; u < r.repo_urls.len; u++)
          salt_buf_printf(out, "repo_snapshot = \"%s\"\n", r.repo_urls.items[u]);
      }
      salt_stratum_recipe_free(&r);
    }
    salt_buf_append_str(out, "\n");
    (*count)++;
  }

  salt_stratum_list_free(&list);
  salt_strata_db_close(db);
}

int lock_write(const Options &o, const std::string &out_path, bool update_existing) {
  if (update_existing && !salt_path_exists(out_path.c_str())) {
    fprintf(stderr, "salt: --update: no existing lockfile at %s\n", out_path.c_str());
    return 1;
  }
  salt_buf lock;
  salt_buf_init(&lock);
  salt_buf_printf(&lock, "schema = 2\ngenerated = \"%s\"\narch = \"%s\"\n", iso_now().c_str(),
                  arch_detect().c_str());

  char cfg_hash[SALT_SHA256_HEXLEN + 1] = {0};
  if (salt_sha256_file(system_config_path(o).c_str(), cfg_hash) == SALT_OK)
    salt_buf_printf(&lock, "config_hash = \"sha256:%s\"\n", cfg_hash);

  int native = 0, strata = 0;
  if (emit_native(o, &lock, &native) != 0) {
    fprintf(stderr, "salt: lockfile not written: not every installed package can be pinned\n");
    salt_buf_free(&lock);
    return 1;
  }
  emit_strata(o, &lock, &strata);

  size_t slash = out_path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) salt_mkdirs(out_path.substr(0, slash).c_str(), 0755);
  if (salt_write_file(out_path.c_str(), lock.data ? lock.data : "", lock.len, 0644) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_buf_free(&lock);
    return 1;
  }
  salt_buf_free(&lock);

  printf("locked %d native package(s), %d stratum(s) -> %s\n", native, strata, out_path.c_str());
  return 0;
}

bool lock_load(const std::string &path, std::vector<LockEntry> &out, std::string &err) {
  salt_toml *root = salt_toml_parse_file(path.c_str());
  if (!root) {
    err = "no lockfile at " + path + " (run 'salt lock')";
    return false;
  }
  const salt_toml *arr = salt_toml_get(root, "native");
  if (arr && salt_toml_typeof(arr) != SALT_TOML_ARRAY) {
    err = path + ": 'native' must be an array of tables";
    salt_toml_free(root);
    return false;
  }
  std::set<std::string> seen;
  size_t n = arr ? salt_toml_array_len(arr) : 0;
  for (size_t i = 0; i < n; i++) {
    const salt_toml *t = salt_toml_array_at(arr, i);
    LockEntry le;
    const char *nm = salt_toml_string(t, "name", nullptr);
    const char *ver = salt_toml_string(t, "version", nullptr);
    const char *sha = salt_toml_string(t, "grain_sha256", nullptr);
    if (!sha) sha = salt_toml_string(t, "sha256", nullptr);
    if (!nm || !nm[0]) {
      err = path + ": native entry " + std::to_string(i + 1) + " has no name";
      salt_toml_free(root);
      return false;
    }
    le.name = nm;
    if (!ver || !ver[0]) {
      err = path + ": " + le.name + " has no version";
      salt_toml_free(root);
      return false;
    }
    le.version = ver;
    le.release = (int)salt_toml_int(t, "release", 1);
    le.sha256 = sha ? strip_sha_prefix(sha) : "";
    if (!salt_sha256_hex_valid(le.sha256.c_str())) {
      err = path + ": " + le.name + " has no valid grain_sha256";
      salt_toml_free(root);
      return false;
    }
    le.arch = salt_toml_string(t, "arch", "");
    le.repo = salt_toml_string(t, "repo", "current");
    le.filename = salt_toml_string(t, "filename", "");
    if (!seen.insert(le.name).second) {
      err = path + ": " + le.name + " is pinned twice";
      salt_toml_free(root);
      return false;
    }
    out.push_back(le);
  }
  salt_toml_free(root);
  return true;
}

struct LockCompare {
  std::vector<std::string> extra;
  std::vector<LockEntry> missing;
  std::vector<std::pair<LockEntry, std::string>> changed;
  size_t matched = 0;
};

static void compare_lock(salt_db *db, const std::vector<LockEntry> &lock, LockCompare &c) {
  std::set<std::string> locked;
  for (const auto &le : lock) {
    locked.insert(le.name);
    salt_db_pkg p;
    if (salt_db_get_pkg(db, le.name.c_str(), &p) != SALT_OK) {
      c.missing.push_back(le);
      continue;
    }
    std::string cur = std::string(p.version ? p.version : "") + "-" + std::to_string(p.release);
    bool same_ver = p.version && le.version == p.version && le.release == p.release;
    bool same_sha = !salt_sha256_hex_valid(p.sha256) || le.sha256 == p.sha256;
    if (same_ver && same_sha)
      c.matched++;
    else if (!same_ver)
      c.changed.push_back({le, cur});
    else
      c.changed.push_back({le, cur + " (sha256 " + std::string(p.sha256) + ")"});
    salt_db_pkg_free_fields(&p);
  }
  salt_db_pkglist l;
  salt_db_pkglist_init(&l);
  salt_db_list_installed(db, &l);
  for (size_t i = 0; i < l.len; i++)
    if (!locked.count(l.items[i].name)) c.extra.push_back(l.items[i].name);
  salt_db_pkglist_free(&l);
}

int lock_diff(const Options &o, const std::string &path, bool quiet) {
  std::vector<LockEntry> lock;
  std::string err;
  if (!lock_load(path, lock, err)) {
    fprintf(stderr, "salt: %s\n", err.c_str());
    return 2;
  }
  salt_db *db = nullptr;
  if (salt_db_open(db_path_for(o).c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 2;
  }
  LockCompare c;
  compare_lock(db, lock, c);
  salt_db_close(db);
  size_t diffs = c.extra.size() + c.missing.size() + c.changed.size();
  if (!quiet) {
    for (const auto &le : c.missing)
      printf("+ %s %s-%d (in lock, not installed)\n", le.name.c_str(), le.version.c_str(),
             le.release);
    for (const auto &ch : c.changed)
      printf("~ %s installed %s, lock pins %s-%d\n", ch.first.name.c_str(), ch.second.c_str(),
             ch.first.version.c_str(), ch.first.release);
    for (const auto &n : c.extra) printf("- %s (installed, not in lock)\n", n.c_str());
    if (!diffs)
      printf("system matches the lockfile (%zu native package%s)\n", c.matched,
             c.matched == 1 ? "" : "s");
    else
      printf("%zu difference%s from %s\n", diffs, diffs == 1 ? "" : "s", path.c_str());
  }
  return diffs ? 1 : 0;
}

static int apply_strata(const Options &o, const std::string &path) {
  salt_toml *root = salt_toml_parse_file(path.c_str());
  if (!root) return 0;
  int rc = 0;
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
  return rc;
}

int lock_apply(const Options &o, const std::string &path, const TxnFlags &f) {
  std::vector<LockEntry> lock;
  std::string err;
  if (!lock_load(path, lock, err)) {
    fprintf(stderr, "salt: %s\n", err.c_str());
    return 1;
  }
  RepoConf c = load_repo_conf(o);
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

  LockCompare cmp;
  compare_lock(db, lock, cmp);
  NativePlan plan;
  bool ok = true;
  std::vector<const LockEntry *> wanted;
  for (const auto &le : cmp.missing) wanted.push_back(&le);
  for (const auto &ch : cmp.changed) wanted.push_back(&ch.first);
  for (const LockEntry *le : wanted) {
    const salt_repo_entry *e =
        salt_repo_index_find_exact(&idx, le->name.c_str(), le->version.c_str(), le->release);
    if (!e) {
      fprintf(stderr, "salt: lock pins %s %s-%d but the repository index does not offer it\n",
              le->name.c_str(), le->version.c_str(), le->release);
      ok = false;
      continue;
    }
    if (!salt_repo_entry_hash_ok(e) || le->sha256 != e->sha256) {
      fprintf(stderr,
              "salt: HASH MISMATCH for %s %s-%d: lock pins sha256 %s, repository index says %s\n",
              le->name.c_str(), le->version.c_str(), le->release, le->sha256.c_str(),
              e->sha256 ? e->sha256 : "(none)");
      ok = false;
      continue;
    }
    plan.install.push_back(e);
  }
  if (!ok) {
    fprintf(stderr, "salt: refusing to apply %s\n", path.c_str());
    salt_db_close(db);
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }
  order_by_deps(idx, plan.install);
  plan.remove = cmp.extra;

  int rc = 0;
  if (plan.install.empty() && plan.remove.empty()) {
    printf("native: already matches %s (%zu package%s)\n", path.c_str(), cmp.matched,
           cmp.matched == 1 ? "" : "s");
  } else {
    TxnFlags lf = f;
    lf.allow_unverified = false;
    rc = native_transaction(o, c, &ctx, db, plan, "lock-apply", lf);
  }
  salt_db_close(db);
  salt_repo_index_free(&idx);
  salt_ctx_free(&ctx);
  if (rc != 0 || f.dry_run || f.download_only) return rc;
  if (apply_strata(o, path) != 0) rc = 1;
  return rc;
}

static const char *LOCK_USAGE =
    "usage: salt lock [--output FILE]\n"
    "       salt lock apply [FILE] [--dry-run] [--download-only]\n"
    "       salt lock diff [FILE] [--quiet]\n";

int cmd_lock(const Options &o, const std::vector<std::string> &args) {
  if (!args.empty() && args[0] == "apply") {
    std::vector<std::string> rest(args.begin() + 1, args.end());
    TxnFlags f;
    if (!parse_txn_flags(rest, f, LOCK_USAGE)) return 2;
    if (rest.size() > 1) {
      fprintf(stderr, "%s", LOCK_USAGE);
      return 2;
    }
    return lock_apply(o, lock_path_for(o, rest.empty() ? "" : rest[0]), f);
  }
  if (!args.empty() && args[0] == "diff") {
    std::string file;
    bool quiet = false;
    for (size_t i = 1; i < args.size(); i++) {
      if (args[i] == "--quiet" || args[i] == "-q")
        quiet = true;
      else if (!args[i].empty() && args[i][0] == '-') {
        fprintf(stderr, "%s", LOCK_USAGE);
        return 2;
      } else if (file.empty())
        file = args[i];
      else {
        fprintf(stderr, "%s", LOCK_USAGE);
        return 2;
      }
    }
    return lock_diff(o, lock_path_for(o, file), quiet);
  }
  std::string out_path;
  bool update = false;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--output" && i + 1 < args.size())
      out_path = args[++i];
    else if (args[i] == "--update")
      update = true;
    else {
      fprintf(stderr, "%s", LOCK_USAGE);
      return 2;
    }
  }
  return lock_write(o, lock_path_for(o, out_path), update);
}

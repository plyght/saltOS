#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/repo.h"
#include "salt/archive.h"
#include "salt/txn.h"
#include "salt/hash.h"
#include "salt/sign.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <map>

static std::string repo_url(const RepoConf &c, const std::string &rel) {
  std::string s = c.source;
  if (!s.empty() && s.back() == '/') s.pop_back();
  return s + "/" + arch_detect() + "/" + rel;
}

static std::string read_key(const std::string &k) {
  if (k.empty()) return "";
  if (salt_path_exists(k.c_str())) {
    salt_buf b;
    if (salt_read_file(k.c_str(), &b) == SALT_OK) {
      std::string s(b.data, b.len);
      salt_buf_free(&b);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
      return s;
    }
  }
  return k;
}

int cmd_sync(const Options &o, const std::vector<std::string> &args) {
  (void)args;
  RepoConf c = load_repo_conf(o);
  if (c.source.empty()) {
    fprintf(stderr, "salt: no repository source configured (etc/salt/repo.conf or --repo)\n");
    return 1;
  }
  std::string idxp = index_path_for(o);
  std::string sigp = idxp + ".sig";
  if (salt_fetch_to_file(repo_url(c, "index.toml").c_str(), idxp.c_str()) != SALT_OK) {
    fprintf(stderr, "salt: sync failed: %s\n", salt_last_error());
    return 1;
  }
  bool have_sig = salt_fetch_to_file(repo_url(c, "index.toml.sig").c_str(), sigp.c_str()) == SALT_OK;
  std::string key = read_key(c.key);
  if (!key.empty()) {
    if (!have_sig) {
      fprintf(stderr, "salt: trusted key configured but repository index is unsigned\n");
      return 1;
    }
    salt_buf sig;
    salt_read_file(sigp.c_str(), &sig);
    std::string sigstr(sig.data, sig.len);
    salt_buf_free(&sig);
    while (!sigstr.empty() && (sigstr.back() == '\n' || sigstr.back() == '\r')) sigstr.pop_back();
    if (salt_verify_file(idxp.c_str(), sigstr.c_str(), key.c_str()) != SALT_OK) {
      fprintf(stderr, "salt: SIGNATURE VERIFICATION FAILED for repository index\n");
      return 1;
    }
    printf("repository index verified with trusted key\n");
  } else {
    fprintf(stderr, "warning: no trusted key configured; index signature not verified\n");
  }
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) == SALT_OK) {
    printf("synced %zu packages from %s\n", idx.len, c.source.c_str());
    salt_repo_index_free(&idx);
  }
  return 0;
}

static bool index_signed_ok(const Options &o, const RepoConf &c) {
  std::string key = read_key(c.key);
  if (key.empty()) return false;
  std::string idxp = index_path_for(o);
  std::string sigp = idxp + ".sig";
  if (!salt_path_exists(sigp.c_str())) return false;
  salt_buf sig;
  if (salt_read_file(sigp.c_str(), &sig) != SALT_OK) return false;
  std::string sigstr(sig.data, sig.len);
  salt_buf_free(&sig);
  while (!sigstr.empty() && (sigstr.back() == '\n' || sigstr.back() == '\r')) sigstr.pop_back();
  return salt_verify_file(idxp.c_str(), sigstr.c_str(), key.c_str()) == SALT_OK;
}

static void resolve(const salt_repo_index &idx, salt_db *db, const std::string &name,
                    std::set<std::string> &seen, std::vector<std::string> &order, bool force) {
  if (seen.count(name)) return;
  seen.insert(name);
  const salt_repo_entry *e = salt_repo_index_find(&idx, name.c_str());
  if (!e) return;
  for (size_t i = 0; i < e->deps.len; i++) {
    if (!salt_db_is_installed(db, e->deps.items[i]))
      resolve(idx, db, e->deps.items[i], seen, order, false);
  }
  if (force || !salt_db_is_installed(db, name.c_str())) order.push_back(name);
}

static int install_one(const Options &o, const RepoConf &c, salt_ctx *ctx, salt_db *db,
                       const salt_repo_entry *e, bool signed_ok, int64_t txn_id) {
  std::string cache = cache_dir_for(o);
  salt_mkdirs(cache.c_str(), 0755);
  std::string dest = path_join(cache, e->filename);
  if (salt_fetch_to_file(repo_url(c, std::string("packages/") + e->filename).c_str(),
                         dest.c_str()) != SALT_OK) {
    fprintf(stderr, "salt: download failed for %s: %s\n", e->name, salt_last_error());
    return SALT_ERR_IO;
  }
  if (e->sha256 && e->sha256[0] && strcmp(e->sha256, "TODO-sha256") != 0) {
    char hex[SALT_SHA256_HEXLEN + 1];
    if (salt_sha256_file(dest.c_str(), hex) != SALT_OK || strcmp(hex, e->sha256) != 0) {
      fprintf(stderr, "salt: HASH MISMATCH for %s (expected %s)\n", e->name, e->sha256);
      return SALT_ERR_VERIFY;
    }
  }
  salt_archive ar;
  if (salt_archive_open(dest.c_str(), &ar) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return SALT_ERR_FORMAT;
  }
  std::string arch = arch_detect();
  if (ar.meta.arch && strcmp(ar.meta.arch, arch.c_str()) != 0 && strcmp(ar.meta.arch, "any") != 0) {
    fprintf(stderr, "salt: %s is for %s, not %s\n", e->name, ar.meta.arch, arch.c_str());
    salt_archive_free(&ar);
    return SALT_ERR;
  }
  const char *sig_status = signed_ok ? "signed" : "unsigned";
  int rc = salt_install_archive(ctx, db, &ar, c.name.c_str(), sig_status, txn_id);
  if (rc == SALT_OK)
    printf("installed %s %s-%d (%s)\n", ar.meta.name, ar.meta.version, ar.meta.release, sig_status);
  salt_archive_free(&ar);
  return rc;
}

static int do_install(const Options &o, const std::vector<std::string> &names, bool is_update) {
  RepoConf c = load_repo_conf(o);
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) != SALT_OK) {
    fprintf(stderr, "salt: no repository index; run 'salt sync' first\n");
    return 1;
  }
  bool signed_ok = index_signed_ok(o, c);

  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = nullptr;
  if (salt_db_open(ctx.db_path, &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }

  std::vector<std::string> order;
  std::set<std::string> seen;
  if (is_update) {
    salt_db_pkglist inst;
    salt_db_pkglist_init(&inst);
    salt_db_list_installed(db, &inst);
    for (size_t i = 0; i < inst.len; i++) {
      const salt_repo_entry *e = salt_repo_index_find(&idx, inst.items[i].name);
      if (!e) continue;
      /* Only update to a STRICTLY newer version/release -- never silently
       * downgrade just because the repo's newest differs from what's installed. */
      int vc = salt_vercmp(e->version, inst.items[i].version);
      bool newer = vc > 0 || (vc == 0 && e->release > inst.items[i].release);
      if (newer) order.push_back(inst.items[i].name);
    }
    salt_db_pkglist_free(&inst);
    if (order.empty()) {
      printf("everything is up to date\n");
      salt_db_close(db);
      salt_repo_index_free(&idx);
      salt_ctx_free(&ctx);
      return 0;
    }
  } else {
    for (auto &n : names) {
      if (!salt_repo_index_find(&idx, n.c_str())) {
        fprintf(stderr, "salt: package not found: %s\n", n.c_str());
        salt_db_close(db);
        salt_repo_index_free(&idx);
        salt_ctx_free(&ctx);
        return 1;
      }
      resolve(idx, db, n, seen, order, true);
    }
  }

  printf("transaction plan (%zu package%s):\n", order.size(), order.size() == 1 ? "" : "s");
  for (auto &n : order) printf("  %s\n", n.c_str());
  if (!confirm(o, "proceed?")) {
    salt_db_close(db);
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }

  int64_t txn_id = 0;
  salt_db_txn_new(db, is_update ? "update" : "install", &txn_id);
  char *snap = nullptr;
  salt_snapshot_create(&ctx, db, txn_id, &snap);
  if (snap) salt_db_txn_set_snapshot(db, txn_id, snap);

  salt_db_sql_begin(db);
  int rc = SALT_OK;
  for (auto &n : order) {
    const salt_repo_entry *e = salt_repo_index_find(&idx, n.c_str());
    if (!e) continue;
    rc = install_one(o, c, &ctx, db, e, signed_ok, txn_id);
    if (rc != SALT_OK) break;
  }
  if (rc == SALT_OK) {
    salt_db_txn_finish(db, txn_id, "ok");
    salt_db_sql_commit(db);
    printf("transaction %lld complete\n", (long long)txn_id);
  } else {
    fprintf(stderr, "salt: transaction failed, rolling back\n");
    salt_db_sql_rollback(db);
    salt_txn_revert_files(&ctx, txn_id);
    salt_db_txn_finish(db, txn_id, "failed");
  }
  free(snap);
  salt_db_close(db);
  salt_repo_index_free(&idx);
  salt_ctx_free(&ctx);
  return rc == SALT_OK ? 0 : 1;
}

bool native_index_has(const Options &o, const std::string &name) {
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) != SALT_OK) return false;
  bool found = salt_repo_index_find(&idx, name.c_str()) != nullptr;
  salt_repo_index_free(&idx);
  return found;
}

int cmd_install(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt install <pkg>... | <stratum>/<pkg>...\n");
    return 2;
  }
  std::vector<std::string> native;
  std::map<std::string, std::vector<std::string>> foreign;
  std::vector<std::string> fallback;
  for (const auto &a : args) {
    PkgRef r = parse_pkgref(a);
    if (r.foreign)
      foreign[r.stratum].push_back(r.pkg);
    else if (native_index_has(o, a))
      native.push_back(a);
    else
      fallback.push_back(a);
  }

  int rc = 0;
  if (!native.empty()) {
    int n = do_install(o, native, false);
    if (n) rc = n;
  }
  for (auto &kv : foreign) {
    int n = stratum_install(o, kv.first, kv.second);
    if (n) rc = n;
  }
  for (const auto &name : fallback) {
    std::string st = choose_stratum_for(o, name);
    if (st.empty()) {
      rc = 1;
      continue;
    }
    int n = stratum_install(o, st, {name});
    if (n) rc = n;
  }
  return rc;
}

int cmd_update(const Options &o, const std::vector<std::string> &args) {
  if (!args.empty()) {
    int rc = 0;
    for (const auto &a : args) {
      PkgRef r = parse_pkgref(a);
      int n = stratum_update(o, r.foreign ? r.stratum : a);
      if (n) rc = n;
    }
    return rc;
  }
  return do_install(o, {}, true);
}

static int native_remove(const Options &o, const std::vector<std::string> &args) {
  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = nullptr;
  if (salt_db_open(ctx.db_path, &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_ctx_free(&ctx);
    return 1;
  }
  for (auto &n : args) {
    if (!salt_db_is_installed(db, n.c_str())) {
      fprintf(stderr, "salt: not installed: %s\n", n.c_str());
      salt_db_close(db);
      salt_ctx_free(&ctx);
      return 1;
    }
    salt_strlist rd;
    salt_strlist_init(&rd);
    salt_db_revdeps(db, n.c_str(), &rd);
    bool blocked = false;
    for (size_t i = 0; i < rd.len; i++) {
      bool in_set = false;
      for (auto &m : args)
        if (m == rd.items[i]) in_set = true;
      if (!in_set && salt_db_is_installed(db, rd.items[i])) {
        fprintf(stderr, "salt: %s is required by %s\n", n.c_str(), rd.items[i]);
        blocked = true;
      }
    }
    salt_strlist_free(&rd);
    if (blocked) {
      salt_db_close(db);
      salt_ctx_free(&ctx);
      return 1;
    }
  }
  printf("removing %zu package%s:\n", args.size(), args.size() == 1 ? "" : "s");
  for (auto &n : args) printf("  %s\n", n.c_str());
  if (!confirm(o, "proceed?")) {
    salt_db_close(db);
    salt_ctx_free(&ctx);
    return 1;
  }
  int64_t txn_id = 0;
  salt_db_txn_new(db, "remove", &txn_id);
  char *snap = nullptr;
  salt_snapshot_create(&ctx, db, txn_id, &snap);
  if (snap) salt_db_txn_set_snapshot(db, txn_id, snap);
  salt_db_sql_begin(db);
  int rc = SALT_OK;
  for (auto &n : args) {
    rc = salt_remove_pkg(&ctx, db, n.c_str(), txn_id);
    if (rc != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      break;
    }
    printf("removed %s\n", n.c_str());
  }
  if (rc == SALT_OK) {
    salt_db_txn_finish(db, txn_id, "ok");
    salt_db_sql_commit(db);
  } else {
    salt_db_sql_rollback(db);
    salt_txn_revert_files(&ctx, txn_id);
    salt_db_txn_finish(db, txn_id, "failed");
  }
  free(snap);
  salt_db_close(db);
  salt_ctx_free(&ctx);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_remove(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt remove <pkg>... | <stratum>/<pkg>...\n");
    return 2;
  }
  std::vector<std::string> native;
  std::map<std::string, std::vector<std::string>> foreign;
  for (const auto &a : args) {
    PkgRef r = parse_pkgref(a);
    if (r.foreign)
      foreign[r.stratum].push_back(r.pkg);
    else
      native.push_back(a);
  }
  int rc = 0;
  if (!native.empty()) {
    int n = native_remove(o, native);
    if (n) rc = n;
  }
  for (auto &kv : foreign) {
    int n = stratum_remove(o, kv.first, kv.second);
    if (n) rc = n;
  }
  return rc;
}

int cmd_rollback(const Options &o, const std::vector<std::string> &args) {
  (void)args;
  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = nullptr;
  if (salt_db_open(ctx.db_path, &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_ctx_free(&ctx);
    return 1;
  }
  int rc = salt_rollback_last(&ctx, db);
  if (rc == SALT_OK)
    printf("rolled back to the previous deployment\n");
  else
    fprintf(stderr, "salt: %s\n", salt_last_error());
  salt_db_close(db);
  salt_ctx_free(&ctx);
  return rc == SALT_OK ? 0 : 1;
}

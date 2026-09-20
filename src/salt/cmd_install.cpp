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
#include <functional>
#include <unistd.h>

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

bool parse_txn_flags(std::vector<std::string> &args, TxnFlags &f, const char *usage) {
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); i++) {
    const std::string &a = args[i];
    if (a == "--allow-unverified") {
      f.allow_unverified = true;
    } else if (a == "--download-only") {
      f.download_only = true;
    } else if (a == "--locked") {
      f.locked = true;
    } else if (a == "--lockfile" && i + 1 < args.size()) {
      f.locked = true;
      f.lockfile = args[++i];
    } else if (a == "--cascade") {
      f.cascade = true;
    } else if (a == "--dry-run" || a == "-n") {
      f.dry_run = true;
    } else if (!a.empty() && a[0] == '-') {
      fprintf(stderr, "salt: unknown option '%s'\n%s", a.c_str(), usage);
      return false;
    } else {
      rest.push_back(a);
    }
  }
  args = rest;
  return true;
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
  std::string tmp = idxp + ".new";
  std::string tmpsig = sigp + ".new";
  if (salt_fetch_to_file(repo_url(c, "index.toml").c_str(), tmp.c_str()) != SALT_OK) {
    fprintf(stderr, "salt: sync failed: %s\n", salt_last_error());
    return 1;
  }
  bool have_sig =
      salt_fetch_to_file(repo_url(c, "index.toml.sig").c_str(), tmpsig.c_str()) == SALT_OK;
  std::string key = read_key(c.key);
  if (!key.empty()) {
    if (!have_sig) {
      fprintf(stderr, "salt: trusted key configured but repository index is unsigned\n");
      unlink(tmp.c_str());
      return 1;
    }
    salt_buf sig;
    if (salt_read_file(tmpsig.c_str(), &sig) != SALT_OK) {
      fprintf(stderr, "salt: cannot read index signature\n");
      unlink(tmp.c_str());
      unlink(tmpsig.c_str());
      return 1;
    }
    std::string sigstr(sig.data, sig.len);
    salt_buf_free(&sig);
    while (!sigstr.empty() && (sigstr.back() == '\n' || sigstr.back() == '\r')) sigstr.pop_back();
    if (salt_verify_file(tmp.c_str(), sigstr.c_str(), key.c_str()) != SALT_OK) {
      fprintf(stderr, "salt: SIGNATURE VERIFICATION FAILED for repository index\n");
      unlink(tmp.c_str());
      unlink(tmpsig.c_str());
      return 1;
    }
    printf("repository index verified with trusted key\n");
  } else {
    fprintf(stderr, "warning: no trusted key configured; index signature not verified\n");
  }
  salt_repo_index idx;
  if (salt_repo_index_load(tmp.c_str(), &idx) != SALT_OK) {
    fprintf(stderr, "salt: sync failed: repository index is not parseable: %s\n",
            salt_last_error());
    unlink(tmp.c_str());
    unlink(tmpsig.c_str());
    return 1;
  }
  salt_strlist problems;
  salt_strlist_init(&problems);
  int vrc = salt_repo_index_verify(&idx, &problems);
  if (vrc != SALT_OK) {
    fprintf(stderr, "salt: sync failed: repository index rejected (%zu problem%s):\n", problems.len,
            problems.len == 1 ? "" : "s");
    for (size_t i = 0; i < problems.len; i++) fprintf(stderr, "  %s\n", problems.items[i]);
    salt_strlist_free(&problems);
    salt_repo_index_free(&idx);
    unlink(tmp.c_str());
    unlink(tmpsig.c_str());
    return 1;
  }
  salt_strlist_free(&problems);
  if (rename(tmp.c_str(), idxp.c_str()) != 0) {
    fprintf(stderr, "salt: cannot install repository index at %s\n", idxp.c_str());
    salt_repo_index_free(&idx);
    unlink(tmp.c_str());
    unlink(tmpsig.c_str());
    return 1;
  }
  if (have_sig)
    rename(tmpsig.c_str(), sigp.c_str());
  else
    unlink(sigp.c_str());
  printf("synced %zu packages from %s\n", idx.len, c.source.c_str());
  salt_repo_index_free(&idx);
  return 0;
}

bool index_signed_ok(const Options &o, const RepoConf &c) {
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
                    const std::string &wanted_by, const std::set<std::string> &targets,
                    std::set<std::string> &seen, std::vector<const salt_repo_entry *> &order,
                    std::vector<std::string> &problems) {
  if (seen.count(name)) return;
  seen.insert(name);
  const salt_repo_entry *e = salt_repo_index_find(&idx, name.c_str());
  if (!e) {
    if (wanted_by.empty())
      problems.push_back("package not found: " + name);
    else
      problems.push_back("dependency " + name + " of " + wanted_by +
                         " is not available in the repository");
    return;
  }
  for (size_t i = 0; i < e->deps.len; i++) {
    std::string dep = e->deps.items[i];
    if (targets.count(dep) || !salt_db_is_installed(db, dep.c_str()))
      resolve(idx, db, dep, name, targets, seen, order, problems);
  }
  if (targets.count(name) || !salt_db_is_installed(db, name.c_str())) order.push_back(e);
}

void order_by_deps(const salt_repo_index &idx, std::vector<const salt_repo_entry *> &items) {
  std::map<std::string, const salt_repo_entry *> byname;
  for (auto *e : items) byname[e->name] = e;
  std::vector<const salt_repo_entry *> out;
  std::set<std::string> done;
  std::function<void(const salt_repo_entry *)> visit = [&](const salt_repo_entry *e) {
    if (done.count(e->name)) return;
    done.insert(e->name);
    for (size_t i = 0; i < e->deps.len; i++) {
      auto it = byname.find(e->deps.items[i]);
      if (it != byname.end()) visit(it->second);
    }
    out.push_back(e);
  };
  (void)idx;
  for (auto *e : items) visit(e);
  items = out;
}

static bool check_conflicts(salt_db *db, const NativePlan &plan) {
  std::set<std::string> leaving(plan.remove.begin(), plan.remove.end());
  std::map<std::string, const salt_repo_entry *> arriving;
  for (auto *e : plan.install) {
    arriving[e->name] = e;
    leaving.insert(e->name);
  }
  bool ok = true;
  for (auto *e : plan.install) {
    for (size_t i = 0; i < e->conflicts.len; i++) {
      std::string c = e->conflicts.items[i];
      if (c == e->name) continue;
      if (arriving.count(c)) {
        fprintf(stderr, "salt: %s conflicts with %s, which is also being installed\n", e->name,
                c.c_str());
        ok = false;
      } else if (salt_db_is_installed(db, c.c_str()) && !leaving.count(c)) {
        fprintf(stderr, "salt: %s conflicts with installed package %s (remove it first)\n", e->name,
                c.c_str());
        ok = false;
      }
    }
    salt_strlist rc;
    salt_strlist_init(&rc);
    salt_db_conflicts_with(db, e->name, &rc);
    for (size_t i = 0; i < rc.len; i++) {
      std::string who = rc.items[i];
      if (who == e->name || leaving.count(who)) continue;
      fprintf(stderr, "salt: installed package %s conflicts with %s (remove it first)\n",
              who.c_str(), e->name);
      ok = false;
    }
    salt_strlist_free(&rc);
  }
  return ok;
}

static bool check_dangling(salt_db *db, const NativePlan &plan) {
  std::set<std::string> gone(plan.remove.begin(), plan.remove.end());
  std::set<std::string> present;
  for (auto *e : plan.install) {
    present.insert(e->name);
    gone.erase(e->name);
  }
  bool ok = true;
  for (auto &n : gone) {
    salt_strlist rd;
    salt_strlist_init(&rd);
    salt_db_revdeps(db, n.c_str(), &rd);
    for (size_t i = 0; i < rd.len; i++) {
      std::string who = rd.items[i];
      if (gone.count(who) || present.count(who)) continue;
      if (!salt_db_is_installed(db, who.c_str())) continue;
      fprintf(stderr, "salt: %s is required by %s\n", n.c_str(), who.c_str());
      ok = false;
    }
    salt_strlist_free(&rd);
  }
  return ok;
}

static int fetch_and_verify(const Options &o, const RepoConf &c, const salt_repo_entry *e,
                            const TxnFlags &f, std::string &dest_out, std::string &sha_out) {
  std::string cache = cache_dir_for(o);
  salt_mkdirs(cache.c_str(), 0755);
  std::string dest = path_join(cache, e->filename);
  bool have_hash = salt_repo_entry_hash_ok(e);
  if (!have_hash && !f.allow_unverified) {
    fprintf(stderr,
            "salt: REFUSING to install %s: the repository index carries no verifiable sha256 "
            "for %s (%s)\n"
            "salt: pass --allow-unverified to install it anyway (NOT recommended)\n",
            e->name, e->filename, e->sha256 && e->sha256[0] ? e->sha256 : "missing");
    return SALT_ERR_VERIFY;
  }
  bool cached = false;
  if (have_hash && salt_path_exists(dest.c_str())) {
    char hex[SALT_SHA256_HEXLEN + 1];
    cached = salt_sha256_file(dest.c_str(), hex) == SALT_OK && strcmp(hex, e->sha256) == 0;
  }
  if (!cached && salt_fetch_to_file(repo_url(c, std::string("packages/") + e->filename).c_str(),
                                    dest.c_str()) != SALT_OK) {
    fprintf(stderr, "salt: download failed for %s: %s\n", e->name, salt_last_error());
    return SALT_ERR_IO;
  }
  char hex[SALT_SHA256_HEXLEN + 1];
  if (salt_sha256_file(dest.c_str(), hex) != SALT_OK) {
    fprintf(stderr, "salt: cannot hash %s\n", dest.c_str());
    return SALT_ERR_IO;
  }
  if (have_hash) {
    if (strcmp(hex, e->sha256) != 0) {
      fprintf(stderr, "salt: HASH MISMATCH for %s (expected %s, got %s)\n", e->name, e->sha256,
              hex);
      unlink(dest.c_str());
      return SALT_ERR_VERIFY;
    }
  } else {
    fprintf(stderr,
            "salt: WARNING: installing %s WITHOUT hash verification (--allow-unverified); "
            "the artifact's integrity is NOT proven; sha256 of the downloaded bytes is %s\n",
            e->name, hex);
  }
  dest_out = dest;
  sha_out = hex;
  return SALT_OK;
}

static int install_one(const RepoConf &c, salt_ctx *ctx, salt_db *db, const salt_repo_entry *e,
                       const std::string &dest, const std::string &sha, bool signed_ok,
                       bool verified, int64_t txn_id) {
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
  if (strcmp(ar.meta.name, e->name) != 0) {
    fprintf(stderr, "salt: %s contains package %s, not %s\n", e->filename, ar.meta.name, e->name);
    salt_archive_free(&ar);
    return SALT_ERR_VERIFY;
  }
  const char *sig_status = !verified ? "unverified" : signed_ok ? "signed" : "unsigned";
  int rc = salt_install_archive(ctx, db, &ar, c.name.c_str(), sig_status, txn_id);
  if (rc == SALT_OK) rc = salt_db_set_pkg_artifact(db, ar.meta.name, e->filename, sha.c_str());
  if (rc == SALT_OK)
    printf("installed %s %s-%d (%s)\n", ar.meta.name, ar.meta.version, ar.meta.release, sig_status);
  else
    fprintf(stderr, "salt: %s\n", salt_last_error());
  salt_archive_free(&ar);
  return rc;
}

int native_transaction(const Options &o, const RepoConf &c, salt_ctx *ctx, salt_db *db,
                       const NativePlan &plan, const char *op, const TxnFlags &f) {
  if (plan.remove.empty() && plan.install.empty()) {
    printf("nothing to do\n");
    return 0;
  }
  if (!check_conflicts(db, plan)) return 1;
  if (!check_dangling(db, plan)) return 1;

  printf("transaction plan (%zu to install, %zu to remove):\n", plan.install.size(),
         plan.remove.size());
  for (auto &n : plan.remove) printf("  - %s\n", n.c_str());
  for (auto *e : plan.install) {
    salt_db_pkg cur;
    bool have = salt_db_get_pkg(db, e->name, &cur) == SALT_OK;
    if (have) {
      printf("  ~ %s %s-%d -> %s-%d\n", e->name, cur.version, cur.release, e->version, e->release);
      salt_db_pkg_free_fields(&cur);
    } else {
      printf("  + %s %s-%d\n", e->name, e->version, e->release);
    }
  }
  if (f.dry_run) {
    printf("dry run: no changes made\n");
    return 0;
  }
  if (!f.download_only && !confirm(o, "proceed?")) {
    fprintf(stderr, "salt: aborted\n");
    return 1;
  }

  bool signed_ok = index_signed_ok(o, c);
  std::vector<std::string> paths, shas;
  for (auto *e : plan.install) {
    std::string dest, sha;
    int rc = fetch_and_verify(o, c, e, f, dest, sha);
    if (rc != SALT_OK) return 1;
    paths.push_back(dest);
    shas.push_back(sha);
  }
  if (f.download_only) {
    printf("downloaded %zu package%s to %s\n", plan.install.size(),
           plan.install.size() == 1 ? "" : "s", cache_dir_for(o).c_str());
    return 0;
  }

  int64_t txn_id = 0;
  if (salt_db_txn_new(db, op, &txn_id) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  char *snap = nullptr;
  salt_snapshot_create(ctx, db, txn_id, &snap);
  if (snap) salt_db_txn_set_snapshot(db, txn_id, snap);

  int rc = salt_db_sql_begin(db);
  for (size_t i = 0; rc == SALT_OK && i < plan.remove.size(); i++) {
    rc = salt_remove_pkg(ctx, db, plan.remove[i].c_str(), txn_id);
    if (rc != SALT_OK)
      fprintf(stderr, "salt: %s\n", salt_last_error());
    else
      printf("removed %s\n", plan.remove[i].c_str());
  }
  for (size_t i = 0; rc == SALT_OK && i < plan.install.size(); i++) {
    const salt_repo_entry *e = plan.install[i];
    rc = install_one(c, ctx, db, e, paths[i], shas[i], signed_ok, salt_repo_entry_hash_ok(e),
                     txn_id);
  }
  if (rc == SALT_OK) rc = salt_db_txn_finish(db, txn_id, "ok");
  if (rc == SALT_OK) rc = salt_db_sql_commit(db);
  if (rc == SALT_OK) {
    printf("transaction %lld complete\n", (long long)txn_id);
  } else {
    fprintf(stderr, "salt: transaction %lld failed, rolling back\n", (long long)txn_id);
    salt_db_sql_rollback(db);
    if (salt_txn_revert_files(ctx, txn_id) != SALT_OK)
      fprintf(stderr, "salt: WARNING: file rollback incomplete: %s\n", salt_last_error());
    salt_db_txn_finish(db, txn_id, "failed");
  }
  free(snap);
  return rc == SALT_OK ? 0 : 1;
}

static int do_install(const Options &o, const std::vector<std::string> &names, bool is_update,
                      const TxnFlags &f) {
  RepoConf c = load_repo_conf(o);
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) != SALT_OK) {
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

  std::set<std::string> targets;
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
      if (newer) targets.insert(inst.items[i].name);
    }
    salt_db_pkglist_free(&inst);
    if (targets.empty()) {
      printf("everything is up to date\n");
      salt_db_close(db);
      salt_repo_index_free(&idx);
      salt_ctx_free(&ctx);
      return 0;
    }
  } else {
    targets.insert(names.begin(), names.end());
  }

  NativePlan plan;
  std::set<std::string> seen;
  std::vector<std::string> problems;
  for (auto &n : targets) resolve(idx, db, n, "", targets, seen, plan.install, problems);
  if (!problems.empty()) {
    for (auto &p : problems) fprintf(stderr, "salt: %s\n", p.c_str());
    fprintf(stderr, "salt: cannot resolve transaction\n");
    salt_db_close(db);
    salt_repo_index_free(&idx);
    salt_ctx_free(&ctx);
    return 1;
  }

  int rc = native_transaction(o, c, &ctx, db, plan, is_update ? "update" : "install", f);
  salt_db_close(db);
  salt_repo_index_free(&idx);
  salt_ctx_free(&ctx);
  return rc;
}

bool native_index_has(const Options &o, const std::string &name) {
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) != SALT_OK) return false;
  bool found = salt_repo_index_find(&idx, name.c_str()) != nullptr;
  salt_repo_index_free(&idx);
  return found;
}

static const char *INSTALL_USAGE =
    "usage: salt install [--allow-unverified] [--download-only] [--dry-run]\n"
    "                    <pkg>... | <stratum>/<pkg>...\n"
    "       salt install --locked [--lockfile FILE] [--allow-unverified] [--dry-run]\n";

int cmd_install(const Options &o, const std::vector<std::string> &in_args) {
  std::vector<std::string> args = in_args;
  TxnFlags f;
  if (!parse_txn_flags(args, f, INSTALL_USAGE)) return 2;
  if (f.locked) {
    if (!args.empty()) {
      fprintf(stderr, "salt: --locked takes no package arguments\n%s", INSTALL_USAGE);
      return 2;
    }
    return lock_apply(o, lock_path_for(o, f.lockfile), f);
  }
  if (args.empty()) {
    fprintf(stderr, "%s", INSTALL_USAGE);
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
    int n = do_install(o, native, false, f);
    if (n) rc = n;
  }
  for (auto &kv : foreign) {
    int n = stratum_install(o, kv.first, kv.second);
    if (n) rc = n;
  }
  for (const auto &name : fallback) {
    std::string st = choose_stratum_for(o, name);
    if (st.empty()) {
      fprintf(stderr, "salt: package not found: %s\n", name.c_str());
      rc = 1;
      continue;
    }
    int n = stratum_install(o, st, {name});
    if (n) rc = n;
  }
  return rc;
}

static const char *UPDATE_USAGE =
    "usage: salt update [--download-only] [--allow-unverified] [--dry-run] [stratum...]\n";

int cmd_update(const Options &o, const std::vector<std::string> &in_args) {
  std::vector<std::string> args = in_args;
  TxnFlags f;
  if (!parse_txn_flags(args, f, UPDATE_USAGE)) return 2;
  if (!args.empty()) {
    int rc = 0;
    for (const auto &a : args) {
      PkgRef r = parse_pkgref(a);
      int n = stratum_update(o, r.foreign ? r.stratum : a);
      if (n) rc = n;
    }
    return rc;
  }
  return do_install(o, {}, true, f);
}

static int native_remove(const Options &o, const std::vector<std::string> &args,
                         const TxnFlags &f) {
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
  }

  std::set<std::string> set(args.begin(), args.end());
  if (f.cascade) {
    std::vector<std::string> work(args.begin(), args.end());
    while (!work.empty()) {
      std::string n = work.back();
      work.pop_back();
      salt_strlist rd;
      salt_strlist_init(&rd);
      salt_db_revdeps(db, n.c_str(), &rd);
      for (size_t i = 0; i < rd.len; i++) {
        std::string who = rd.items[i];
        if (!set.count(who) && salt_db_is_installed(db, who.c_str())) {
          set.insert(who);
          work.push_back(who);
        }
      }
      salt_strlist_free(&rd);
    }
  }
  std::vector<std::string> order;
  std::set<std::string> done;
  std::function<void(const std::string &)> visit = [&](const std::string &n) {
    if (done.count(n)) return;
    done.insert(n);
    salt_strlist rd;
    salt_strlist_init(&rd);
    salt_db_revdeps(db, n.c_str(), &rd);
    for (size_t i = 0; i < rd.len; i++)
      if (set.count(rd.items[i])) visit(rd.items[i]);
    salt_strlist_free(&rd);
    order.push_back(n);
  };
  for (auto &n : set) visit(n);

  NativePlan plan;
  plan.remove = order;
  RepoConf c = load_repo_conf(o);
  int rc = native_transaction(o, c, &ctx, db, plan, "remove", f);
  if (rc != 0 && !f.cascade) {
    bool blocked = false;
    for (auto &n : args) {
      salt_strlist rd;
      salt_strlist_init(&rd);
      salt_db_revdeps(db, n.c_str(), &rd);
      for (size_t i = 0; i < rd.len; i++)
        if (!set.count(rd.items[i]) && salt_db_is_installed(db, rd.items[i])) blocked = true;
      salt_strlist_free(&rd);
    }
    if (blocked) fprintf(stderr, "salt: use --cascade to remove dependent packages as well\n");
  }
  salt_db_close(db);
  salt_ctx_free(&ctx);
  return rc;
}

static const char *REMOVE_USAGE =
    "usage: salt remove [--cascade] [--dry-run] <pkg>... | <stratum>/<pkg>...\n";

int cmd_remove(const Options &o, const std::vector<std::string> &in_args) {
  std::vector<std::string> args = in_args;
  TxnFlags f;
  if (!parse_txn_flags(args, f, REMOVE_USAGE)) return 2;
  if (args.empty()) {
    fprintf(stderr, "%s", REMOVE_USAGE);
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
    int n = native_remove(o, native, f);
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

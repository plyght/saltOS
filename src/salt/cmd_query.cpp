#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/repo.h"
#include "salt/txn.h"
#include "salt/hash.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <strings.h>
#include <unistd.h>

static bool contains_nocase(const char *hay, const std::string &needle) {
  if (!hay) return false;
  return strcasestr(hay, needle.c_str()) != nullptr;
}

static salt_db *open_db(const Options &o) {
  salt_db *db = nullptr;
  std::string p = db_path_for(o);
  if (salt_db_open(p.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return nullptr;
  }
  return db;
}

int cmd_search(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt search <term> | <stratum>/<term>\n");
    return 2;
  }
  PkgRef sref = parse_pkgref(args[0]);
  if (sref.foreign) return stratum_search(o, sref.stratum, sref.pkg);
  const std::string &term = args[0];
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  bool have_index = salt_repo_index_load(idxp.c_str(), &idx) == SALT_OK;
  salt_db *db = open_db(o);
  int matches = 0;
  if (have_index) {
    for (size_t i = 0; i < idx.len; i++) {
      const salt_repo_entry &e = idx.items[i];
      if (term == "*" || contains_nocase(e.name, term) || contains_nocase(e.summary, term)) {
        bool inst = db && salt_db_is_installed(db, e.name);
        printf("%-24s %s-%d%s%s%s\n", e.name, e.version, e.release, inst ? "  [installed]" : "",
               e.summary && e.summary[0] ? "  " : "", e.summary ? e.summary : "");
        matches++;
      }
    }
    salt_repo_index_free(&idx);
  } else if (db) {
    salt_db_pkglist l;
    salt_db_pkglist_init(&l);
    salt_db_search(db, term.c_str(), &l);
    for (size_t i = 0; i < l.len; i++) {
      printf("%-24s %s-%d  [installed]%s%s\n", l.items[i].name, l.items[i].version,
             l.items[i].release, l.items[i].summary && l.items[i].summary[0] ? "  " : "",
             l.items[i].summary ? l.items[i].summary : "");
      matches++;
    }
    salt_db_pkglist_free(&l);
  }
  if (db) salt_db_close(db);
  if (!matches) fprintf(stderr, "no matches for '%s'\n", term.c_str());
  return matches ? 0 : 1;
}

int cmd_query(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt info <pkg>\n");
    return 2;
  }
  salt_db *db = open_db(o);
  if (!db) return 1;
  salt_db_pkg p;
  int rc = salt_db_get_pkg(db, args[0].c_str(), &p);
  if (rc == SALT_OK) {
    char ts[64];
    time_t t = (time_t)p.install_time;
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("name        : %s\n", p.name);
    printf("version     : %s-%d\n", p.version, p.release);
    printf("arch        : %s\n", p.arch);
    if (p.summary && p.summary[0]) printf("summary     : %s\n", p.summary);
    if (p.license && p.license[0]) printf("license     : %s\n", p.license);
    printf("repo        : %s\n", p.repo[0] ? p.repo : "(local)");
    printf("signature   : %s\n", p.sig_status);
    if (p.sha256 && p.sha256[0]) printf("sha256      : %s\n", p.sha256);
    if (p.filename && p.filename[0]) printf("artifact    : %s\n", p.filename);
    printf("installed   : %s\n", ts);
    printf("transaction : %lld\n", (long long)p.txn_id);
    salt_strlist files;
    salt_strlist_init(&files);
    salt_db_pkg_files(db, p.name, &files);
    printf("files       : %zu\n", files.len);
    salt_strlist_free(&files);
    salt_strlist deps;
    salt_strlist_init(&deps);
    salt_db_pkg_deps(db, p.name, &deps);
    if (deps.len) {
      printf("depends on  :");
      for (size_t i = 0; i < deps.len; i++) printf(" %s", deps.items[i]);
      printf("\n");
    }
    salt_strlist_free(&deps);
    salt_strlist rd;
    salt_strlist_init(&rd);
    salt_db_revdeps(db, p.name, &rd);
    if (rd.len) {
      printf("required by :");
      for (size_t i = 0; i < rd.len; i++) printf(" %s", rd.items[i]);
      printf("\n");
    }
    salt_db_pkg_free_fields(&p);
    salt_db_close(db);
    return 0;
  }
  salt_db_close(db);
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  if (salt_repo_index_load(idxp.c_str(), &idx) == SALT_OK) {
    const salt_repo_entry *e = salt_repo_index_find(&idx, args[0].c_str());
    if (e) {
      printf("name        : %s\n", e->name);
      printf("version     : %s-%d\n", e->version, e->release);
      printf("arch        : %s\n", e->arch ? e->arch : "");
      if (e->summary && e->summary[0]) printf("summary     : %s\n", e->summary);
      printf("status      : available (not installed)\n");
      printf("artifact    : %s (%llu bytes)\n", e->filename, (unsigned long long)e->size);
      printf("sha256      : %s\n", salt_repo_entry_hash_ok(e) ? e->sha256 : "(unverifiable)");
      if (e->deps.len) {
        printf("depends on  :");
        for (size_t i = 0; i < e->deps.len; i++) printf(" %s", e->deps.items[i]);
        printf("\n");
      }
      if (e->conflicts.len) {
        printf("conflicts   :");
        for (size_t i = 0; i < e->conflicts.len; i++) printf(" %s", e->conflicts.items[i]);
        printf("\n");
      }
      salt_repo_index_free(&idx);
      return 0;
    }
    salt_repo_index_free(&idx);
  }
  fprintf(stderr, "salt: no such package: %s\n", args[0].c_str());
  return 1;
}

int cmd_files(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt files <pkg>\n");
    return 2;
  }
  salt_db *db = open_db(o);
  if (!db) return 1;
  if (!salt_db_is_installed(db, args[0].c_str())) {
    salt_db_close(db);
    fprintf(stderr, "package not installed: %s\n", args[0].c_str());
    return 1;
  }
  salt_strlist files;
  salt_strlist_init(&files);
  salt_db_pkg_files(db, args[0].c_str(), &files);
  for (size_t i = 0; i < files.len; i++) printf("/%s\n", files.items[i]);
  salt_strlist_free(&files);
  salt_db_close(db);
  return 0;
}

int cmd_owner(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt owner <path>\n");
    return 2;
  }
  salt_db *db = open_db(o);
  if (!db) return 1;
  char *owner = nullptr;
  int rc = salt_db_owner(db, args[0].c_str(), &owner);
  if (rc == SALT_OK) {
    printf("%s is owned by %s\n", args[0].c_str(), owner);
    free(owner);
    salt_db_close(db);
    return 0;
  }
  salt_db_close(db);
  fprintf(stderr, "no package owns %s\n", args[0].c_str());
  return 1;
}

int cmd_list(const Options &o, const std::vector<std::string> &args) {
  bool upgradable = false, available = false;
  for (const auto &a : args) {
    if (a == "--installed") continue;
    if (a == "--upgradable" || a == "--upgradeable")
      upgradable = true;
    else if (a == "--available")
      available = true;
    else {
      fprintf(stderr, "usage: salt list [--installed | --upgradable | --available]\n");
      return 2;
    }
  }
  salt_db *db = open_db(o);
  if (!db) return 1;
  if (available) {
    salt_repo_index idx;
    if (salt_repo_index_load(index_path_for(o).c_str(), &idx) != SALT_OK) {
      fprintf(stderr, "salt: no repository index; run 'salt sync' first\n");
      salt_db_close(db);
      return 1;
    }
    for (size_t i = 0; i < idx.len; i++)
      printf("%-24s %s-%d %s%s\n", idx.items[i].name, idx.items[i].version, idx.items[i].release,
             idx.items[i].arch ? idx.items[i].arch : "",
             salt_db_is_installed(db, idx.items[i].name) ? "  [installed]" : "");
    printf("%zu packages available\n", idx.len);
    salt_repo_index_free(&idx);
    salt_db_close(db);
    return 0;
  }
  salt_db_pkglist l;
  salt_db_pkglist_init(&l);
  salt_db_list_installed(db, &l);
  if (upgradable) {
    salt_repo_index idx;
    if (salt_repo_index_load(index_path_for(o).c_str(), &idx) != SALT_OK) {
      fprintf(stderr, "salt: no repository index; run 'salt sync' first\n");
      salt_db_pkglist_free(&l);
      salt_db_close(db);
      return 1;
    }
    size_t n = 0;
    for (size_t i = 0; i < l.len; i++) {
      const salt_repo_entry *e = salt_repo_index_find(&idx, l.items[i].name);
      if (!e) continue;
      int vc = salt_vercmp(e->version, l.items[i].version);
      if (vc > 0 || (vc == 0 && e->release > l.items[i].release)) {
        printf("%-24s %s-%d -> %s-%d\n", l.items[i].name, l.items[i].version, l.items[i].release,
               e->version, e->release);
        n++;
      }
    }
    printf("%zu package%s upgradable\n", n, n == 1 ? "" : "s");
    salt_repo_index_free(&idx);
  } else {
    for (size_t i = 0; i < l.len; i++)
      printf("%-24s %s-%d %s\n", l.items[i].name, l.items[i].version, l.items[i].release,
             l.items[i].arch);
    printf("%zu packages installed\n", l.len);
  }
  salt_db_pkglist_free(&l);
  salt_db_close(db);
  return 0;
}

int cmd_verify(const Options &o, const std::vector<std::string> &args) {
  salt_db *db = open_db(o);
  if (!db) return 1;
  salt_db_pkglist l;
  salt_db_pkglist_init(&l);
  if (!args.empty()) {
    salt_db_pkg p;
    if (salt_db_get_pkg(db, args[0].c_str(), &p) != SALT_OK) {
      salt_db_close(db);
      fprintf(stderr, "package not installed: %s\n", args[0].c_str());
      return 1;
    }
    salt_db_pkg_free_fields(&p);
  }
  salt_db_list_installed(db, &l);
  int problems = 0;
  int checked = 0;
  for (size_t i = 0; i < l.len; i++) {
    if (!args.empty() && args[0] != l.items[i].name) continue;
    salt_manifest m;
    salt_manifest_init(&m);
    salt_db_pkg_manifest(db, l.items[i].name, &m);
    for (size_t j = 0; j < m.len; j++) {
      std::string full = path_join(o.root, m.items[j].path);
      if (m.items[j].typeflag == '2') {
        char target[4096];
        ssize_t n = readlink(full.c_str(), target, sizeof(target) - 1);
        checked++;
        if (n < 0) {
          printf("MISSING  %s (%s)\n", full.c_str(), l.items[i].name);
          problems++;
        } else {
          target[n] = '\0';
          if (!m.items[j].linkname || strcmp(target, m.items[j].linkname) != 0) {
            printf("MODIFIED %s (%s) -> %s\n", full.c_str(), l.items[i].name, target);
            problems++;
          }
        }
        continue;
      }
      if (m.items[j].typeflag != '0' && m.items[j].typeflag != 0) continue;
      if (!m.items[j].sha256 || !m.items[j].sha256[0]) continue;
      char hex[SALT_SHA256_HEXLEN + 1];
      checked++;
      if (salt_sha256_file(full.c_str(), hex) != SALT_OK) {
        printf("MISSING  %s (%s)\n", full.c_str(), l.items[i].name);
        problems++;
      } else if (strcmp(hex, m.items[j].sha256) != 0) {
        printf("MODIFIED %s (%s)\n", full.c_str(), l.items[i].name);
        problems++;
      }
    }
    salt_manifest_free(&m);
  }
  salt_db_pkglist_free(&l);
  salt_db_close(db);
  printf("verified %d files, %d problem(s)\n", checked, problems);
  return problems ? 1 : 0;
}

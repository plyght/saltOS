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
    fprintf(stderr, "usage: salt search <term>\n");
    return 2;
  }
  const std::string &term = args[0];
  std::string idxp = index_path_for(o);
  salt_repo_index idx;
  bool have_index = salt_repo_index_load(idxp.c_str(), &idx) == SALT_OK;
  salt_db *db = open_db(o);
  int matches = 0;
  if (have_index) {
    for (size_t i = 0; i < idx.len; i++) {
      if (term == "*" || strstr(idx.items[i].name, term.c_str())) {
        bool inst = db && salt_db_is_installed(db, idx.items[i].name);
        printf("%-24s %s-%d%s\n", idx.items[i].name, idx.items[i].version, idx.items[i].release,
               inst ? "  [installed]" : "");
        matches++;
      }
    }
    salt_repo_index_free(&idx);
  } else if (db) {
    salt_db_pkglist l;
    salt_db_pkglist_init(&l);
    salt_db_search(db, term.c_str(), &l);
    for (size_t i = 0; i < l.len; i++) {
      printf("%-24s %s-%d  [installed]\n", l.items[i].name, l.items[i].version, l.items[i].release);
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
    fprintf(stderr, "usage: salt query <pkg>\n");
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
    printf("repo        : %s\n", p.repo[0] ? p.repo : "(local)");
    printf("signature   : %s\n", p.sig_status);
    printf("installed   : %s\n", ts);
    printf("transaction : %lld\n", (long long)p.txn_id);
    salt_strlist files;
    salt_strlist_init(&files);
    salt_db_pkg_files(db, p.name, &files);
    printf("files       : %zu\n", files.len);
    salt_strlist_free(&files);
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
  fprintf(stderr, "package not installed: %s\n", args[0].c_str());
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
  (void)args;
  salt_db *db = open_db(o);
  if (!db) return 1;
  salt_db_pkglist l;
  salt_db_pkglist_init(&l);
  salt_db_list_installed(db, &l);
  for (size_t i = 0; i < l.len; i++)
    printf("%-24s %s-%d %s\n", l.items[i].name, l.items[i].version, l.items[i].release,
           l.items[i].arch);
  printf("%zu packages installed\n", l.len);
  salt_db_pkglist_free(&l);
  salt_db_close(db);
  return 0;
}

int cmd_deployments(const Options &o, const std::vector<std::string> &args) {
  (void)args;
  salt_db *db = open_db(o);
  if (!db) return 1;
  salt_deployment_list l;
  salt_deployment_list_init(&l);
  salt_db_deployments(db, &l);
  for (size_t i = 0; i < l.len; i++) {
    char ts[64];
    time_t t = (time_t)l.items[i].time;
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("#%-5lld %-10s %-9s %s%s%s\n", (long long)l.items[i].id, l.items[i].op,
           l.items[i].status, ts, l.items[i].snapshot ? "  snapshot=" : "",
           l.items[i].snapshot ? l.items[i].snapshot : "");
  }
  salt_deployment_list_free(&l);
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
      if (m.items[j].typeflag != '0' && m.items[j].typeflag != 0) continue;
      if (!m.items[j].sha256 || !m.items[j].sha256[0]) continue;
      std::string full = path_join(o.root, m.items[j].path);
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

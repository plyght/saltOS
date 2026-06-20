#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string basename_of(const std::string &p) {
  size_t pos = p.find_last_of('/');
  return pos == std::string::npos ? p : p.substr(pos + 1);
}

int cmd_expose(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt expose <stratum> <command> [as <alias>]\n");
    return 2;
  }
  const std::string &stratum = args[0];
  const std::string &command = args[1];
  std::string alias;
  for (size_t i = 2; i + 1 < args.size(); i++) {
    if (args[i] == "as") {
      alias = args[i + 1];
      break;
    }
  }
  if (alias.empty()) alias = basename_of(command);

  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  int rc = salt_expose_add(db, o.root.c_str(), stratum.c_str(), command.c_str(), alias.c_str(),
                           "cli");
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("exposed %s/%s as %s\n", stratum.c_str(), command.c_str(), alias.c_str());
  salt_strata_db_close(db);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_unexpose(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt unexpose <alias>\n");
    return 2;
  }
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  int rc = salt_expose_remove(db, o.root.c_str(), args[0].c_str());
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("unexposed %s\n", args[0].c_str());
  salt_strata_db_close(db);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_exposed(const Options &o, const std::vector<std::string> &args) {
  (void)args;
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_exposed_list l;
  salt_exposed_list_init(&l);
  int rc = salt_expose_list(db, &l);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
  } else {
    printf("%-16s %-16s %-24s %s\n", "ALIAS", "STRATUM", "COMMAND", "KIND");
    for (size_t i = 0; i < l.len; i++) {
      const salt_exposed &e = l.items[i];
      printf("%-16s %-16s %-24s %s\n", e.alias ? e.alias : "", e.stratum ? e.stratum : "",
             e.command ? e.command : "", e.kind ? e.kind : "");
    }
  }
  salt_exposed_list_free(&l);
  salt_strata_db_close(db);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_expose_desktop(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt expose-desktop <stratum> <app>\n");
    return 2;
  }
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, args[0].c_str(), &s) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_strata_db_close(db);
    return 1;
  }
  int rc = salt_expose_desktop(db, &s, o.root.c_str(), args[1].c_str());
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("exposed desktop entry for %s/%s\n", args[0].c_str(), args[1].c_str());
  salt_stratum_free_fields(&s);
  salt_strata_db_close(db);
  return rc == SALT_OK ? 0 : 1;
}

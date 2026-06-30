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

// `salt which <cmd>`: show which stratum the bare name currently routes to, and
// every stratum that provides the command -- so cross-stratum name collisions
// (first-installed wins) are discoverable and you know what `salt <stratum>/<cmd>`
// alternatives exist.
int cmd_which(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt which <command>\n");
    return 2;
  }
  const std::string &name = args[0];
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open_ro(o.root.c_str(), &db) != SALT_OK &&
      salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }

  std::string owner, kind;
  salt_exposed_list el;
  salt_exposed_list_init(&el);
  if (salt_expose_list(db, &el) == SALT_OK) {
    for (size_t i = 0; i < el.len; i++) {
      if (el.items[i].alias && name == el.items[i].alias) {
        owner = el.items[i].stratum ? el.items[i].stratum : "";
        kind = el.items[i].kind ? el.items[i].kind : "";
        break;
      }
    }
  }
  salt_exposed_list_free(&el);

  std::vector<std::string> providers;
  salt_stratum_list sl;
  salt_stratum_list_init(&sl);
  if (salt_stratum_list_all(db, &sl) == SALT_OK) {
    static const char *bindirs[] = {"usr/bin", "bin", "usr/local/bin", nullptr};
    for (size_t i = 0; i < sl.len; i++) {
      const salt_stratum &s = sl.items[i];
      if (!s.root) continue;
      char *base = (o.root != "/") ? salt_join_path(o.root.c_str(), s.root) : salt_strdup(s.root);
      for (int j = 0; bindirs[j]; j++) {
        char *d = salt_join_path(base, bindirs[j]);
        char *p = salt_join_path(d, name.c_str());
        bool ok = salt_path_exists(p);
        free(d);
        free(p);
        if (ok) {
          providers.push_back(s.name ? s.name : "");
          break;
        }
      }
      free(base);
    }
  }
  salt_stratum_list_free(&sl);
  salt_strata_db_close(db);

  if (!owner.empty() && !kind.empty())
    printf("%s -> %s (%s)\n", name.c_str(), owner.c_str(), kind.c_str());
  else if (!owner.empty())
    printf("%s -> %s\n", name.c_str(), owner.c_str());
  else
    printf("%s -> not exposed (a host command, or not installed in any stratum)\n", name.c_str());

  if (!providers.empty()) {
    printf("provided by:");
    for (auto &p : providers) printf(" %s", p.c_str());
    printf("\n");
    printf("run a specific one with: salt <stratum>/%s\n", name.c_str());
  }
  return 0;
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
  // Read-only: listing exposed commands must work for any user without sudo.
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open_ro(o.root.c_str(), &db) != SALT_OK &&
      salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
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

int cmd_expose_all(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 1) {
    fprintf(stderr, "usage: salt expose-all <stratum>\n");
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
  int count = 0;
  int rc = salt_expose_all(db, o.root.c_str(), &s, &count);
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("exposed %d command(s) from %s as host commands\n", count, args[0].c_str());
  salt_stratum_free_fields(&s);
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

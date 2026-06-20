#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
#include "salt/run.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int cmd_run(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt run <stratum> <cmd> [args...]\n");
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
    fprintf(stderr, "salt: unknown stratum '%s' (try 'salt stratum list')\n", args[0].c_str());
    salt_strata_db_close(db);
    return 1;
  }
  std::vector<std::string> hold(args.begin() + 1, args.end());
  std::vector<char *> argv;
  for (auto &t : hold) argv.push_back(const_cast<char *>(t.c_str()));
  argv.push_back(nullptr);

  salt_run_opts opts;
  salt_run_opts_default(&opts);
  int st = 0;
  int rc = salt_stratum_run(&s, &opts, argv.data(), &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  salt_stratum_free_fields(&s);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

int cmd_pkg(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt pkg <stratum> <install|remove|update|search> [pkg...]\n");
    return 2;
  }
  const std::string &name = args[0];
  const std::string &op = args[1];

  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_strata_ctx c;
  salt_strata_ctx_init(&c, o.root.c_str());

  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, name.c_str(), &s) != SALT_OK) {
    fprintf(stderr, "salt: unknown stratum '%s' (try 'salt stratum list')\n", name.c_str());
    salt_strata_ctx_free(&c);
    salt_strata_db_close(db);
    return 1;
  }

  if (op != "search") {
    std::string label = "pre-" + op;
    if (salt_stratum_snapshot_create(&c, db, name.c_str(), label.c_str(), nullptr) != SALT_OK)
      fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());
  }

  std::vector<std::string> hold(args.begin() + 2, args.end());
  std::vector<char *> pkgs;
  for (auto &t : hold) pkgs.push_back(const_cast<char *>(t.c_str()));
  int npkgs = (int)hold.size();

  int st = 0;
  int rc = salt_stratum_pkg(&s, op.c_str(), pkgs.empty() ? nullptr : pkgs.data(), npkgs, &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

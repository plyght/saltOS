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
#include <unistd.h>

// Kernel denied unprivileged user namespaces (salt_run_child exit 126). Fall
// back to the transparent-sudo path so `run` still works -- sudo sets up the
// stratum as root and salt drops back to the caller. Re-execs; only returns on
// failure to exec sudo.
static void run_fallback_sudo(const Options &o, const std::vector<std::string> &args) {
  if (geteuid() == 0) return;  // already root, nothing to escalate
  std::vector<char *> a;
  a.push_back(const_cast<char *>("sudo"));
  a.push_back(const_cast<char *>("-n"));
  a.push_back(const_cast<char *>("salt"));
  if (o.root != "/") {
    a.push_back(const_cast<char *>("--root"));
    a.push_back(const_cast<char *>(o.root.c_str()));
  }
  a.push_back(const_cast<char *>("run"));
  for (auto &t : args) a.push_back(const_cast<char *>(t.c_str()));
  a.push_back(nullptr);
  execvp("sudo", a.data());
}

int cmd_run(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt run <stratum> <cmd> [args...]\n");
    return 2;
  }
  // Read-only open: `run` only needs to look up the stratum's root, and an
  // unprivileged user must be able to read the root-owned DB without writing it.
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open_ro(o.root.c_str(), &db) != SALT_OK &&
      salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
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
  salt_strata_db_close(db);  // done reading; the run child needs no DB handle

  std::vector<std::string> hold(args.begin() + 1, args.end());
  // Allow the conventional `salt run <stratum> -- <cmd>` separator: drop a single
  // leading "--" so the command isn't exec'd as the literal program "--".
  if (!hold.empty() && hold.front() == "--") hold.erase(hold.begin());
  std::vector<char *> argv;
  for (auto &t : hold) argv.push_back(const_cast<char *>(t.c_str()));
  argv.push_back(nullptr);

  salt_run_opts opts;
  salt_run_opts_default(&opts);
  // Run in the caller's current directory (bound into the stratum by run.c) so
  // `cd ~/code/app && bun dev` behaves like a normal shell instead of starting
  // in $HOME. Falls back to the default (home) if cwd is somehow unavailable.
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd))) opts.workdir = cwd;
  int st = 0;
  int rc = salt_stratum_run(&s, &opts, argv.data(), &st);
  salt_stratum_free_fields(&s);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  // Unprivileged user namespaces unavailable; retry transparently w/ sudo. This
  // is signaled out-of-band (not a real exit code), so a command that genuinely
  // exits 126 is passed through untouched instead of being re-run under sudo.
  if (st == SALT_RUN_USERNS_DENIED) {
    run_fallback_sudo(o, args);
    fprintf(stderr, "salt run: needs unprivileged user namespaces or sudo "
                    "(could not escalate)\n");
    return 126;
  }
  // A command may have *installed* new binaries into the stratum (npm i -g,
  // pip install, cargo install, make install, anything). Re-expose so they
  // become host commands immediately -- no package-manager list, no manual
  // `salt expose`. This is best-effort: expose_all_for opens the strata DB for
  // write and silently returns if it can't (the common unprivileged path), so
  // it only does real work when we had the privilege to change the stratum --
  // exactly the case where something could have been installed. expose_all is
  // idempotent and prints only when it actually adds commands.
  if (st == 0 && expose_all_enabled(o)) expose_all_for(o, args[0]);
  return st;
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

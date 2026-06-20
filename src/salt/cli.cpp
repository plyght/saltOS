#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/toml.h"
#include "salt/sign.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/utsname.h>

std::string arch_detect() {
  struct utsname u;
  if (uname(&u) == 0) {
    std::string m = u.machine;
    if (m == "arm64" || m == "aarch64") return "aarch64";
    if (m == "amd64" || m == "x86_64") return "x86_64";
    return m;
  }
  return "x86_64";
}

std::string path_join(const std::string &a, const std::string &b) {
  char *p = salt_join_path(a.c_str(), b.c_str());
  std::string r = p ? p : "";
  free(p);
  return r;
}

std::string db_path_for(const Options &o) {
  return path_join(o.root, "var/lib/salt/db.sqlite");
}

std::string cache_dir_for(const Options &o) {
  return path_join(o.root, "var/lib/salt/cache/" + arch_detect());
}

std::string index_path_for(const Options &o) {
  return path_join(o.root, "var/lib/salt/repo/" + arch_detect() + "/index.toml");
}

std::string trustdb_for(const Options &o) {
  const char *env = getenv("SALT_TRUSTDB");
  if (env && env[0]) return env;
  return path_join(o.root, "var/lib/salt/trust.toml");
}

std::string strata_db_path_for(const Options &o) {
  return path_join(o.root, "var/lib/salt/strata.sqlite");
}

RepoConf load_repo_conf(const Options &o) {
  RepoConf rc;
  std::string conf = path_join(o.root, "etc/salt/repo.conf");
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    rc.name = salt_toml_string(t, "repo", "current");
    rc.source = salt_toml_string(t, "source", "");
    rc.key = salt_toml_string(t, "key", "");
    salt_toml_free(t);
  }
  if (!o.repo.empty()) rc.source = o.repo;
  if (!o.key.empty()) rc.key = o.key;
  return rc;
}

bool confirm(const Options &o, const std::string &prompt) {
  if (o.yes) return true;
  fprintf(stderr, "%s [y/N] ", prompt.c_str());
  int c = getchar();
  return c == 'y' || c == 'Y';
}

static void usage() {
  fprintf(stderr,
          "salt - the saltOS package manager\n\n"
          "usage: salt [--root DIR] [--repo URL] [--key HEX|FILE] [--yes] <command> [args]\n\n"
          "package commands:\n"
          "  sync                 refresh the repository index\n"
          "  search <term>        search available packages\n"
          "  install <pkg>...     install packages\n"
          "  remove <pkg>...      remove packages\n"
          "  update               upgrade all packages\n"
          "  rollback             roll back the last transaction\n"
          "  deployments          list transactions/deployments\n"
          "  verify [pkg]         verify installed files against the database\n"
          "  query <pkg>          show package details\n"
          "  files <pkg>          list files owned by a package\n"
          "  owner <path>         show which package owns a path\n"
          "  list                 list installed packages\n\n"
          "maintainer commands:\n"
          "  build <recipe-dir>   build a package from a recipe\n"
          "  lint <recipe-dir>    lint a recipe\n"
          "  sign <file>          sign a file with the repo secret key\n"
          "  repo publish <dir>   build and sign a repository index\n"
          "  keygen <dir> <name>  generate a signing keypair\n"
          "  trust <subcommand>   manage the contributor trust model\n\n"
          "stratum commands:\n"
          "  stratum list                  list managed strata\n"
          "  stratum add <recipe|name>     bootstrap a foreign distro stratum\n"
          "  stratum remove <name>         destroy a stratum\n"
          "  stratum status <name>         show stratum details\n"
          "  stratum snapshot <name>       snapshot a stratum\n"
          "  stratum rollback <name> [id]  roll a stratum back to a snapshot\n"
          "  stratum shell <name>          open a shell inside a stratum\n"
          "  stratum lint <recipe>         validate a stratum recipe\n\n"
          "integration commands:\n"
          "  run <stratum> <cmd> [args]    run a command inside a stratum\n"
          "  pkg <stratum> <op> [pkg]      foreign package manager passthrough\n"
          "  expose <stratum> <cmd> [as A] expose a stratum command on the host\n"
          "  unexpose <alias>              remove an exposed command\n"
          "  exposed                       list exposed commands\n"
          "  expose-desktop <stratum> <a>  expose a stratum desktop app\n"
          "  provider <subcommand>         manage component providers\n"
          "  service <subcommand>          integrate stratum daemons under runit\n");
}

static int dispatch(const Options &o, const std::string &cmd,
                    const std::vector<std::string> &args) {
  if (cmd == "sync") return cmd_sync(o, args);
  if (cmd == "search") return cmd_search(o, args);
  if (cmd == "install") return cmd_install(o, args);
  if (cmd == "remove") return cmd_remove(o, args);
  if (cmd == "update") return cmd_update(o, args);
  if (cmd == "rollback") return cmd_rollback(o, args);
  if (cmd == "deployments") return cmd_deployments(o, args);
  if (cmd == "verify") return cmd_verify(o, args);
  if (cmd == "query") return cmd_query(o, args);
  if (cmd == "files") return cmd_files(o, args);
  if (cmd == "owner") return cmd_owner(o, args);
  if (cmd == "list") return cmd_list(o, args);
  if (cmd == "build") return cmd_build(o, args);
  if (cmd == "lint") return cmd_lint(o, args);
  if (cmd == "sign") return cmd_sign(o, args);
  if (cmd == "repo") return cmd_repo(o, args);
  if (cmd == "keygen") return cmd_keygen(o, args);
  if (cmd == "trust") return cmd_trust(o, args);
  if (cmd == "stratum") return cmd_stratum(o, args);
  if (cmd == "run") return cmd_run(o, args);
  if (cmd == "pkg") return cmd_pkg(o, args);
  if (cmd == "expose") return cmd_expose(o, args);
  if (cmd == "unexpose") return cmd_unexpose(o, args);
  if (cmd == "exposed") return cmd_exposed(o, args);
  if (cmd == "expose-desktop") return cmd_expose_desktop(o, args);
  if (cmd == "provider") return cmd_provider(o, args);
  if (cmd == "service") return cmd_service(o, args);
  fprintf(stderr, "salt: unknown command '%s'\n", cmd.c_str());
  usage();
  return 2;
}

int cli_main(int argc, char **argv) {
  salt_sign_init();
  Options o;
  std::vector<std::string> rest;
  std::string cmd;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (!cmd.empty()) {
      rest.push_back(a);
      continue;
    }
    if (a == "--root" && i + 1 < argc) {
      o.root = argv[++i];
    } else if (a == "--repo" && i + 1 < argc) {
      o.repo = argv[++i];
    } else if (a == "--key" && i + 1 < argc) {
      o.key = argv[++i];
    } else if (a == "--yes" || a == "-y") {
      o.yes = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "--version" || a == "-V") {
      printf("salt 0.1.0 (%s)\n", arch_detect().c_str());
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      fprintf(stderr, "salt: unknown flag '%s'\n", a.c_str());
      return 2;
    } else {
      cmd = a;
    }
  }
  if (cmd.empty()) {
    usage();
    return 2;
  }
  return dispatch(o, cmd, rest);
}

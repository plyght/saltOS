#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/toml.h"
#include "salt/sign.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/utsname.h>
#include <unistd.h>

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

PkgRef parse_pkgref(const std::string &arg) {
  PkgRef r;
  size_t p = arg.find('/');
  if (p == std::string::npos || p == 0 || p + 1 >= arg.size()) {
    r.pkg = arg;
    return r;
  }
  r.foreign = true;
  r.stratum = arg.substr(0, p);
  r.pkg = arg.substr(p + 1);
  return r;
}

std::string auto_expose_mode(const Options &o) {
  if (!o.expose_mode.empty()) return o.expose_mode;
  std::string conf = path_join(o.root, "etc/salt/salt.conf");
  std::string mode = "prompt";
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    const char *v = salt_toml_string(t, "install.auto_expose", "prompt");
    if (v && v[0]) mode = v;
    salt_toml_free(t);
  }
  if (mode != "always" && mode != "never" && mode != "prompt") mode = "prompt";
  return mode;
}

bool expose_pm_enabled(const Options &o) {
  if (o.expose_mode == "never") return false;
  std::string conf = path_join(o.root, "etc/salt/salt.conf");
  bool en = true;
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    en = salt_toml_bool(t, "strata.expose_pm", true);
    salt_toml_free(t);
  }
  return en;
}

bool expose_all_enabled(const Options &o) {
  if (o.expose_mode == "never") return false;
  if (o.expose_mode == "always") return true;
  std::string conf = path_join(o.root, "etc/salt/salt.conf");
  bool en = false;  // off by default; saltOS images opt in via salt.conf
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    en = salt_toml_bool(t, "strata.expose_all", false);
    salt_toml_free(t);
  }
  return en;
}

bool auto_service_enabled(const Options &o) {
  if (o.expose_mode == "never") return false;
  std::string conf = path_join(o.root, "etc/salt/salt.conf");
  bool en = true;
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    en = salt_toml_bool(t, "strata.auto_service", true);
    salt_toml_free(t);
  }
  return en;
}

static void usage() {
  fprintf(stderr,
          "salt - the saltOS package manager\n\n"
          "usage: salt [--root DIR] [--repo URL] [--key HEX|FILE] [--yes]\n"
          "            [--expose|--no-expose] <command> [args]\n\n"
          "shortcuts (stratum/package):\n"
          "  install <stratum>/<pkg>   install foreign software and offer to expose it\n"
          "  remove  <stratum>/<pkg>   remove foreign software and its host shims\n"
          "  search  <stratum>/<term>  search a stratum's repositories\n"
          "  update  <stratum>         upgrade packages inside a stratum\n"
          "  <stratum>/<cmd> [args]    run a stratum command (e.g. salt alpine/nvim file)\n\n"
          "package commands:\n"
          "  sync                 refresh the repository index\n"
          "  search <term>        search available packages\n"
          "  install <pkg>...     install packages (use <stratum>/<pkg> for foreign)\n"
          "  remove <pkg>...      remove packages\n"
          "  update [stratum...]  upgrade the host, or named strata\n"
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
          "  pm <stratum> <pm> [args]      run a stratum's package manager as root\n"
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
  if (cmd == "pm") return cmd_pm(o, args);
  if (cmd == "expose") return cmd_expose(o, args);
  if (cmd == "unexpose") return cmd_unexpose(o, args);
  if (cmd == "exposed") return cmd_exposed(o, args);
  if (cmd == "expose-desktop") return cmd_expose_desktop(o, args);
  if (cmd == "expose-all") return cmd_expose_all(o, args);
  if (cmd == "provider") return cmd_provider(o, args);
  if (cmd == "service") return cmd_service(o, args);
  if (cmd == "config") return cmd_config(o, args);
  if (cmd == "lock") return cmd_lock(o, args);
  if (cmd.find('/') != std::string::npos && cmd.find('/') != 0) {
    PkgRef r = parse_pkgref(cmd);
    std::vector<std::string> ra = {r.stratum, r.pkg};
    ra.insert(ra.end(), args.begin(), args.end());
    return cmd_run(o, ra);
  }
  fprintf(stderr, "salt: unknown command '%s'\n", cmd.c_str());
  usage();
  return 2;
}

// Commands that need root because they set up a mount namespace / chroot, write
// the system or strata databases, or manage host services. For `run` (and the
// pm/pkg wrappers that call it) salt drops back to the calling user via SUDO_UID
// after the namespace is built -- see salt_run_child() -- so an exposed `nvim`
// still runs AS YOU, not as root, even though it transited sudo.
static bool cmd_needs_root(const std::string &cmd) {
  static const char *root_cmds[] = {
      "run",     "pkg",    "pm",       "stratum", "expose",  "unexpose",
      "expose-desktop", "expose-all", "exposed", "service", "install",
      "remove", "update", "sync", "rollback", "lock", nullptr};
  for (int i = 0; root_cmds[i]; i++)
    if (cmd == root_cmds[i]) return true;
  return false;
}

// Transparently re-exec the whole salt invocation under `sudo -n` when a
// root-needing command is run by an unprivileged user. This is what lets the
// exposed shims (which just call `salt run <stratum> <cmd>`) work from any user
// without the user ever typing sudo. The euid==0 check after sudo is the loop
// guard. Requires a passwordless sudoers rule for /usr/bin/salt (the images
// install one); if sudo is missing or denies, we fall through and the command's
// own "needs root" error explains the situation.
static void reexec_root_if_needed(const std::string &cmd, char **argv) {
#if defined(__linux__)
  if (geteuid() == 0) return;
  if (!cmd_needs_root(cmd)) return;
  std::vector<char *> a;
  a.push_back(const_cast<char *>("sudo"));
  a.push_back(const_cast<char *>("-n"));
  a.push_back(const_cast<char *>("salt"));
  for (int i = 1; argv[i] != nullptr; i++) a.push_back(argv[i]);
  a.push_back(nullptr);
  execvp("sudo", a.data());
  // execvp only returns on failure (e.g. sudo not installed); fall through.
#else
  (void)cmd;
  (void)argv;
#endif
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
    } else if (a == "--expose") {
      o.expose_mode = "always";
    } else if (a == "--no-expose") {
      o.expose_mode = "never";
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
  reexec_root_if_needed(cmd, argv);
  return dispatch(o, cmd, rest);
}

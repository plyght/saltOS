#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
#include "salt/run.h"
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

static int open_stratum(const Options &o, const std::string &name, salt_strata_db **db,
                        salt_strata_ctx *c, salt_stratum *s);

static const char *kBinDirs[] = {"usr/bin", "bin", "usr/local/bin",
                                 "usr/sbin", "sbin", "usr/local/sbin"};

static void scan_bins(const std::string &root, std::set<std::string> &out) {
  if (root.empty()) return;
  for (const char *d : kBinDirs) {
    std::string p = path_join(root, d);
    DIR *dh = opendir(p.c_str());
    if (!dh) continue;
    struct dirent *de;
    while ((de = readdir(dh)) != nullptr) {
      if (de->d_name[0] == '.') continue;
      out.insert(de->d_name);
    }
    closedir(dh);
  }
}

static void scan_desktops(const std::string &root, std::set<std::string> &out) {
  if (root.empty()) return;
  std::string p = path_join(root, "usr/share/applications");
  DIR *dh = opendir(p.c_str());
  if (!dh) return;
  struct dirent *de;
  while ((de = readdir(dh)) != nullptr) {
    std::string n = de->d_name;
    if (n.size() > 8 && n.compare(n.size() - 8, 8, ".desktop") == 0)
      out.insert(n.substr(0, n.size() - 8));
  }
  closedir(dh);
}

static std::string parse_execstart(const std::string &path) {
  salt_buf b;
  salt_buf_init(&b);
  if (salt_read_file(path.c_str(), &b) != SALT_OK) {
    salt_buf_free(&b);
    return "";
  }
  std::string content(b.data ? b.data : "", b.len);
  salt_buf_free(&b);
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    std::string line =
        content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? content.size() : eol + 1;
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos) continue;
    line = line.substr(s);
    if (line.rfind("ExecStart=", 0) == 0) {
      std::string v = line.substr(10);
      size_t k = 0;
      while (k < v.size() &&
             (v[k] == '-' || v[k] == '+' || v[k] == '!' || v[k] == '@' || v[k] == ':'))
        k++;
      v = v.substr(k);
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
      return v;
    }
  }
  return "";
}

static void scan_services(const std::string &root, std::map<std::string, std::string> &out) {
  if (root.empty()) return;
  const char *dirs[] = {"usr/lib/systemd/system", "lib/systemd/system", "etc/systemd/system"};
  for (const char *d : dirs) {
    std::string p = path_join(root, d);
    DIR *dh = opendir(p.c_str());
    if (!dh) continue;
    struct dirent *de;
    while ((de = readdir(dh)) != nullptr) {
      std::string n = de->d_name;
      if (n.size() <= 8 || n.compare(n.size() - 8, 8, ".service") != 0) continue;
      std::string base = n.substr(0, n.size() - 8);
      if (base.find('@') != std::string::npos) continue;
      if (out.count(base)) continue;
      std::string exec = parse_execstart(path_join(p, n));
      if (!exec.empty()) out[base] = exec;
    }
    closedir(dh);
  }
}

static int import_and_enable_service(const Options &o, const std::string &stratum,
                                     const std::string &name, const std::string &execline) {
  std::string svc = stratum + "-" + name;
  std::string dir = path_join(o.root, "etc/sv/" + svc);
  if (salt_mkdirs(dir.c_str(), 0755) != SALT_OK) return 1;
  std::string run = dir + "/run";
  std::string content = "#!/bin/sh\nexec 2>&1\nexec salt run " + stratum + " " + execline + "\n";
  if (salt_write_file(run.c_str(), content.data(), content.size(), 0755) != SALT_OK) return 1;
  std::string servicedir = path_join(o.root, "var/service");
  salt_mkdirs(servicedir.c_str(), 0755);
  std::string link = servicedir + "/" + svc;
  if (symlink(dir.c_str(), link.c_str()) != 0 && errno != EEXIST) return 1;
  return 0;
}

std::vector<std::string> list_strata_names(const Options &o) {
  std::vector<std::string> names;
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) return names;
  salt_stratum_list l;
  salt_stratum_list_init(&l);
  if (salt_stratum_list_all(db, &l) == SALT_OK)
    for (size_t i = 0; i < l.len; i++)
      if (l.items[i].name) names.push_back(l.items[i].name);
  salt_stratum_list_free(&l);
  salt_strata_db_close(db);
  return names;
}

std::string choose_stratum_for(const Options &o, const std::string &name) {
  std::vector<std::string> names = list_strata_names(o);
  if (names.empty()) {
    fprintf(stderr, "salt: '%s' is not in the native repository and no strata exist.\n",
            name.c_str());
    fprintf(stderr, "      try 'salt stratum add alpine' then 'salt install alpine/%s'\n",
            name.c_str());
    return "";
  }
  if (names.size() == 1) {
    fprintf(stderr, "salt: '%s' is not native; using the only stratum '%s'\n", name.c_str(),
            names[0].c_str());
    return names[0];
  }
  fprintf(stderr, "salt: '%s' is not in the native repository. choose a stratum:\n", name.c_str());
  for (size_t i = 0; i < names.size(); i++)
    fprintf(stderr, "  %zu) %s\n", i + 1, names[i].c_str());
  if (o.yes) {
    fprintf(stderr, "  (--yes) using %s\n", names[0].c_str());
    return names[0];
  }
  fprintf(stderr, "select [1-%zu, or q to skip]: ", names.size());
  char buf[64];
  if (!fgets(buf, sizeof(buf), stdin)) return "";
  int idx = atoi(buf);
  if (idx < 1 || idx > (int)names.size()) return "";
  return names[idx - 1];
}

static std::string pm_kind(const salt_stratum &s) {
  std::string v = s.package_manager ? s.package_manager : "";
  if (v.empty() && s.family) v = s.family;
  if (v == "pacman" || v == "arch") return "pacman";
  if (v == "apt" || v == "apt-get" || v == "debian") return "apt";
  if (v == "apk" || v == "alpine") return "apk";
  if (v == "dnf" || v == "yum" || v == "fedora") return "dnf";
  if (v == "zypper" || v == "opensuse" || v == "suse") return "zypper";
  return "xbps";
}

static std::vector<std::string> pm_binaries(const std::string &kind) {
  if (kind == "pacman") return {"pacman"};
  if (kind == "apt") return {"apt", "apt-get"};
  if (kind == "apk") return {"apk"};
  if (kind == "dnf") return {"dnf"};
  if (kind == "zypper") return {"zypper"};
  return {"xbps-install", "xbps-remove", "xbps-query"};
}

static bool pm_is_mutating(const std::string &kind, const std::string &binary,
                           const std::vector<std::string> &rest) {
  std::string first = rest.empty() ? std::string() : rest[0];
  if (binary == "xbps-query") return false;
  if (binary == "xbps-install" || binary == "xbps-remove") return true;
  if (kind == "pacman") {
    if (first.size() >= 2 && first[0] == '-') {
      if (first.find('Q') != std::string::npos) return false;
      if (first.find('S') != std::string::npos) {
        if (first.find('y') != std::string::npos || first.find('u') != std::string::npos ||
            first.find('w') != std::string::npos)
          return true;
        if (first.find('s') != std::string::npos || first.find('i') != std::string::npos ||
            first.find('l') != std::string::npos || first.find('g') != std::string::npos)
          return false;
        return true;
      }
      if (first.find('T') != std::string::npos || first.find('V') != std::string::npos) return false;
    }
    return true;
  }
  static const std::set<std::string> ro = {
      "search",   "se",     "info",    "if",   "show",  "list",  "ls",
      "policy",   "depends", "rdepends", "provides", "what-provides", "wp",
      "repoquery", "version", "stats",  "help", "--help", "--version", "-V",
      "-l",       "-L",     "-s"};
  static const std::set<std::string> mut = {
      "install",      "in",    "add",    "remove", "rm",   "del",   "erase",
      "purge",        "upgrade", "up",   "update", "dist-upgrade", "dup",
      "downgrade",    "autoremove", "fix", "reinstall"};
  if (ro.count(first)) return false;
  if (mut.count(first)) return true;
  return true;
}

void expose_pm_for(const Options &o, const std::string &stratum) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) return;
  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, stratum.c_str(), &s) == SALT_OK) {
    for (const auto &b : pm_binaries(pm_kind(s)))
      if (salt_expose_pm(db, o.root.c_str(), stratum.c_str(), b.c_str()) == SALT_OK)
        printf("exposed %s as a host command (runs in the %s stratum)\n", b.c_str(),
               stratum.c_str());
    salt_stratum_free_fields(&s);
  }
  salt_strata_db_close(db);
}

void expose_all_for(const Options &o, const std::string &stratum) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) return;
  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, stratum.c_str(), &s) == SALT_OK) {
    int count = 0;
    if (salt_expose_all(db, o.root.c_str(), &s, &count) == SALT_OK && count > 0)
      printf("exposed %d command(s) from %s as host commands\n", count, stratum.c_str());
    salt_stratum_free_fields(&s);
  }
  salt_strata_db_close(db);
}

int cmd_pm(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt pm <stratum> <pm-binary> [args...]\n");
    return 2;
  }
  const std::string &stratum = args[0];
  const std::string &binary = args[1];
  std::vector<std::string> rest(args.begin() + 2, args.end());

  salt_strata_db *db = nullptr;
  salt_strata_ctx c;
  salt_stratum s;
  if (open_stratum(o, stratum, &db, &c, &s)) return 1;

  if (pm_is_mutating(pm_kind(s), binary, rest)) {
    if (salt_stratum_snapshot_create(&c, db, stratum.c_str(), "pre-pm", nullptr) != SALT_OK)
      fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());
  }

  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(binary.c_str()));
  for (auto &a : rest) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);

  salt_run_opts opts;
  salt_run_opts_default(&opts);
  opts.graphics = false;
  opts.interactive = true;
  opts.user = "root";
  opts.workdir = "/";
  int st = 0;
  bool mutating = pm_is_mutating(pm_kind(s), binary, rest);
  int rc = salt_stratum_run(&s, &opts, argv.data(), &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  // After a successful install/upgrade, pick up any newly added binaries so
  // freshly installed tools are immediately runnable as host commands.
  if (rc == SALT_OK && st == 0 && mutating && expose_all_enabled(o))
    expose_all_for(o, stratum);
  return rc != SALT_OK ? 1 : st;
}

int ensure_stratum(const Options &o, const std::string &name) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, name.c_str(), &s) == SALT_OK) {
    salt_stratum_free_fields(&s);
    salt_strata_db_close(db);
    return 0;
  }

  std::string path = resolve_stratum_recipe(o, name);
  if (path.empty()) {
    fprintf(stderr, "salt: no stratum '%s' and no built-in recipe to bootstrap it from.\n",
            name.c_str());
    fprintf(stderr, "      add one explicitly with 'salt stratum add <recipe.toml>'\n");
    salt_strata_db_close(db);
    return 1;
  }
  salt_stratum_recipe r;
  salt_stratum_recipe_init(&r);
  if (salt_stratum_recipe_load(path.c_str(), &r) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_stratum_recipe_free(&r);
    salt_strata_db_close(db);
    return 1;
  }
  fprintf(stderr, "salt: the '%s' stratum is not set up yet.\n", name.c_str());
  if (!confirm(o, std::string("bootstrap ") + name + " (" + (r.family ? r.family : "?") +
                      ") now? this downloads a base userspace")) {
    salt_stratum_recipe_free(&r);
    salt_strata_db_close(db);
    return 1;
  }
  salt_strata_ctx c;
  salt_strata_ctx_init(&c, o.root.c_str());
  int rc = salt_stratum_bootstrap(&c, db, &r);
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("bootstrapped %s\n", name.c_str());
  salt_stratum_recipe_free(&r);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  if (rc == SALT_OK && expose_pm_enabled(o)) expose_pm_for(o, name);
  return rc == SALT_OK ? 0 : 1;
}

static int open_stratum(const Options &o, const std::string &name, salt_strata_db **db,
                        salt_strata_ctx *c, salt_stratum *s) {
  if (salt_strata_db_open(o.root.c_str(), db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_strata_ctx_init(c, o.root.c_str());
  memset(s, 0, sizeof(*s));
  if (salt_stratum_get(*db, name.c_str(), s) != SALT_OK) {
    fprintf(stderr, "salt: unknown stratum '%s' (try 'salt stratum list')\n", name.c_str());
    salt_strata_ctx_free(c);
    salt_strata_db_close(*db);
    *db = nullptr;
    return 1;
  }
  return 0;
}

int stratum_install(const Options &o, const std::string &stratum,
                    const std::vector<std::string> &pkgs) {
  if (pkgs.empty()) {
    fprintf(stderr, "usage: salt install %s/<pkg>...\n", stratum.c_str());
    return 2;
  }
  if (ensure_stratum(o, stratum)) return 1;
  salt_strata_db *db = nullptr;
  salt_strata_ctx c;
  salt_stratum s;
  if (open_stratum(o, stratum, &db, &c, &s)) return 1;

  if (salt_stratum_snapshot_create(&c, db, stratum.c_str(), "pre-install", nullptr) != SALT_OK)
    fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());

  std::string root = s.root ? s.root : "";
  std::set<std::string> before, before_apps;
  std::map<std::string, std::string> before_svcs;
  scan_bins(root, before);
  scan_desktops(root, before_apps);
  scan_services(root, before_svcs);

  std::vector<char *> argv;
  for (auto &p : pkgs) argv.push_back(const_cast<char *>(p.c_str()));
  int st = 0;
  int rc = salt_stratum_pkg(&s, "install", argv.data(), (int)argv.size(), &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  if (rc == SALT_OK && st == 0) {
    std::string mode = auto_expose_mode(o);
    if (mode != "never") {
      std::set<std::string> after, after_apps;
      scan_bins(root, after);
      scan_desktops(root, after_apps);
      std::vector<std::string> newbins, newapps;
      for (const auto &n : after)
        if (!before.count(n)) newbins.push_back(n);
      for (const auto &n : after_apps)
        if (!before_apps.count(n)) newapps.push_back(n);
      if (!newbins.empty() || !newapps.empty()) {
        bool doit = true;
        if (mode != "always") {
          if (!newbins.empty()) {
            fprintf(stderr, "new command%s in %s:\n", newbins.size() == 1 ? "" : "s",
                    stratum.c_str());
            for (auto &n : newbins) fprintf(stderr, "  %s\n", n.c_str());
          }
          if (!newapps.empty()) {
            fprintf(stderr, "new app%s in %s:\n", newapps.size() == 1 ? "" : "s", stratum.c_str());
            for (auto &n : newapps) fprintf(stderr, "  %s\n", n.c_str());
          }
          doit = confirm(o, "expose these on the host (PATH + menu)?");
        }
        if (doit) {
          for (auto &n : newbins) {
            if (salt_expose_add(db, o.root.c_str(), stratum.c_str(), n.c_str(), n.c_str(), "cli") ==
                SALT_OK)
              printf("exposed command %s\n", n.c_str());
            else
              fprintf(stderr, "salt: could not expose %s: %s\n", n.c_str(), salt_last_error());
          }
          for (auto &n : newapps) {
            if (salt_expose_desktop(db, &s, o.root.c_str(), n.c_str()) == SALT_OK)
              printf("exposed app %s\n", n.c_str());
            else
              fprintf(stderr, "salt: could not expose app %s: %s\n", n.c_str(), salt_last_error());
          }
        }
      }
    }
  }

  if (rc == SALT_OK && st == 0 && auto_expose_mode(o) != "never" && auto_service_enabled(o)) {
    std::map<std::string, std::string> after_svcs;
    scan_services(root, after_svcs);
    std::vector<std::pair<std::string, std::string>> newsvcs;
    for (auto &kv : after_svcs)
      if (!before_svcs.count(kv.first)) newsvcs.push_back(kv);
    if (!newsvcs.empty()) {
      bool doit = true;
      if (auto_expose_mode(o) != "always") {
        fprintf(stderr, "new service%s in %s:\n", newsvcs.size() == 1 ? "" : "s", stratum.c_str());
        for (auto &sv : newsvcs) fprintf(stderr, "  %s\n", sv.first.c_str());
        doit = confirm(o, "import and enable these to start under runit?");
      }
      if (doit)
        for (auto &sv : newsvcs) {
          if (import_and_enable_service(o, stratum, sv.first, sv.second) == 0)
            printf("enabled service %s-%s\n", stratum.c_str(), sv.first.c_str());
          else
            fprintf(stderr, "salt: could not import service %s\n", sv.first.c_str());
        }
    }
  }

  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

int stratum_remove(const Options &o, const std::string &stratum,
                   const std::vector<std::string> &pkgs) {
  if (pkgs.empty()) {
    fprintf(stderr, "usage: salt remove %s/<pkg>...\n", stratum.c_str());
    return 2;
  }
  salt_strata_db *db = nullptr;
  salt_strata_ctx c;
  salt_stratum s;
  if (open_stratum(o, stratum, &db, &c, &s)) return 1;

  if (salt_stratum_snapshot_create(&c, db, stratum.c_str(), "pre-remove", nullptr) != SALT_OK)
    fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());

  std::string root = s.root ? s.root : "";
  std::set<std::string> before, before_apps;
  std::map<std::string, std::string> before_svcs;
  scan_bins(root, before);
  scan_desktops(root, before_apps);
  scan_services(root, before_svcs);

  std::vector<char *> argv;
  for (auto &p : pkgs) argv.push_back(const_cast<char *>(p.c_str()));
  int st = 0;
  int rc = salt_stratum_pkg(&s, "remove", argv.data(), (int)argv.size(), &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  if (rc == SALT_OK && st == 0) {
    std::set<std::string> after, after_apps;
    scan_bins(root, after);
    scan_desktops(root, after_apps);
    salt_exposed_list l;
    salt_exposed_list_init(&l);
    if (salt_expose_list(db, &l) == SALT_OK)
      for (size_t i = 0; i < l.len; i++) {
        const salt_exposed &e = l.items[i];
        if (!e.alias || !e.stratum || !e.command) continue;
        if (stratum != e.stratum) continue;
        bool bin_gone = before.count(e.command) && !after.count(e.command);
        bool app_gone = before_apps.count(e.command) && !after_apps.count(e.command);
        if (bin_gone || app_gone) {
          if (salt_expose_remove(db, o.root.c_str(), e.alias) == SALT_OK)
            printf("unexposed %s\n", e.alias);
        }
      }
    salt_exposed_list_free(&l);
    for (const auto &n : before_apps) {
      if (after_apps.count(n)) continue;
      std::string entry =
          path_join(o.root, "usr/local/share/applications/" + stratum + "-" + n + ".desktop");
      remove(entry.c_str());
    }
    std::map<std::string, std::string> after_svcs;
    scan_services(root, after_svcs);
    for (const auto &kv : before_svcs) {
      if (after_svcs.count(kv.first)) continue;
      std::string svc = stratum + "-" + kv.first;
      std::string link = path_join(o.root, "var/service/" + svc);
      remove(link.c_str());
      std::string dir = path_join(o.root, "etc/sv/" + svc);
      salt_remove_recursive(dir.c_str());
      printf("disabled service %s\n", svc.c_str());
    }
  }

  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

int stratum_search(const Options &o, const std::string &stratum, const std::string &term) {
  salt_strata_db *db = nullptr;
  salt_strata_ctx c;
  salt_stratum s;
  if (open_stratum(o, stratum, &db, &c, &s)) return 1;
  char *pkgs[1] = {const_cast<char *>(term.c_str())};
  int st = 0;
  int rc = salt_stratum_pkg(&s, "search", pkgs, 1, &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());
  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

int stratum_update(const Options &o, const std::string &stratum) {
  salt_strata_db *db = nullptr;
  salt_strata_ctx c;
  salt_stratum s;
  if (open_stratum(o, stratum, &db, &c, &s)) return 1;
  if (salt_stratum_snapshot_create(&c, db, stratum.c_str(), "pre-update", nullptr) != SALT_OK)
    fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());
  int st = 0;
  int rc = salt_stratum_pkg(&s, "update", nullptr, 0, &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());
  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

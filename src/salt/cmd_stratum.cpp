#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
#include "salt/run.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void stratum_usage() {
  fprintf(stderr,
          "usage: salt stratum <subcommand>\n"
          "  list                          list all strata\n"
          "  add <recipe|name>             bootstrap a stratum from a recipe\n"
          "  remove <name>                 destroy a stratum\n"
          "  status <name>                 show a stratum and its snapshots\n"
          "  snapshot <name> [label]       create a snapshot\n"
          "  rollback <name> [id]          roll a stratum back\n"
          "  shell <name>                  open an interactive shell in a stratum\n"
          "  lint <recipe>                 lint a stratum recipe\n");
}

static std::string resolve_recipe(const Options &o, const std::string &arg) {
  if (arg.find('/') != std::string::npos ||
      (arg.size() >= 5 && arg.compare(arg.size() - 5, 5, ".toml") == 0))
    return arg;
  std::vector<std::string> cands = {
      path_join(o.root, "etc/salt/strata/" + arg + ".toml"),
      path_join(o.root, "usr/share/salt/strata/" + arg + ".toml"),
      "strata/" + arg + ".toml",
  };
  for (auto &c : cands)
    if (salt_path_exists(c.c_str())) return c;
  return "";
}

int cmd_stratum(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    stratum_usage();
    return 2;
  }
  const std::string &sub = args[0];

  if (sub == "lint") {
    if (args.size() < 2) {
      stratum_usage();
      return 2;
    }
    salt_buf report;
    salt_buf_init(&report);
    int rc = salt_stratum_recipe_lint(args[1].c_str(), &report);
    if (report.data && report.len) fwrite(report.data, 1, report.len, stdout);
    salt_buf_free(&report);
    return rc == SALT_OK ? 0 : 1;
  }

  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_strata_ctx c;
  salt_strata_ctx_init(&c, o.root.c_str());

  int ret = 0;

  if (sub == "list") {
    salt_stratum_list l;
    salt_stratum_list_init(&l);
    if (salt_stratum_list_all(db, &l) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      ret = 1;
    } else {
      printf("%-16s %-10s %-8s %-10s %s\n", "NAME", "FAMILY", "ARCH", "STATE", "ROOT");
      for (size_t i = 0; i < l.len; i++) {
        const salt_stratum &s = l.items[i];
        printf("%-16s %-10s %-8s %-10s %s\n", s.name ? s.name : "", s.family ? s.family : "",
               s.arch ? s.arch : "", s.state ? s.state : "", s.root ? s.root : "");
      }
    }
    salt_stratum_list_free(&l);
  } else if (sub == "add") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else {
      std::string path = resolve_recipe(o, args[1]);
      if (path.empty()) {
        fprintf(stderr, "salt: recipe not found: %s\n", args[1].c_str());
        ret = 1;
      } else {
        salt_stratum_recipe r;
        salt_stratum_recipe_init(&r);
        if (salt_stratum_recipe_load(path.c_str(), &r) != SALT_OK) {
          fprintf(stderr, "salt: %s\n", salt_last_error());
          ret = 1;
        } else {
          printf("about to bootstrap %s (%s) into %s\n", r.name ? r.name : "?",
                 r.family ? r.family : "?", r.root ? r.root : "?");
          if (!confirm(o, "proceed?")) {
            ret = 1;
          } else if (salt_stratum_bootstrap(&c, db, &r) != SALT_OK) {
            fprintf(stderr, "salt: %s\n", salt_last_error());
            ret = 1;
          } else {
            printf("bootstrapped %s\n", r.name ? r.name : args[1].c_str());
          }
        }
        salt_stratum_recipe_free(&r);
      }
    }
  } else if (sub == "remove") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else if (!confirm(o, std::string("destroy ") + args[1] + "?")) {
      ret = 1;
    } else if (salt_stratum_destroy(&c, db, args[1].c_str()) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      ret = 1;
    } else {
      printf("destroyed %s\n", args[1].c_str());
    }
  } else if (sub == "status") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else {
      salt_stratum s;
      memset(&s, 0, sizeof(s));
      if (salt_stratum_get(db, args[1].c_str(), &s) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        printf("name:            %s\n", s.name ? s.name : "");
        printf("family:          %s\n", s.family ? s.family : "");
        printf("arch:            %s\n", s.arch ? s.arch : "");
        printf("root:            %s\n", s.root ? s.root : "");
        printf("package_manager: %s\n", s.package_manager ? s.package_manager : "");
        printf("bootstrap:       %s\n", s.bootstrap ? s.bootstrap : "");
        printf("trust:           %s\n", s.trust ? s.trust : "");
        printf("state:           %s\n", s.state ? s.state : "");
        printf("graphics:        %s\n", s.graphics ? "yes" : "no");
        printf("audio:           %s\n", s.audio ? "yes" : "no");
        printf("dbus:            %s\n", s.dbus ? "yes" : "no");
        printf("created:         %lld\n", (long long)s.created);
        salt_stratum_snapshot_list sl;
        salt_stratum_snapshot_list_init(&sl);
        if (salt_stratum_snapshot_list_all(db, args[1].c_str(), &sl) == SALT_OK) {
          printf("snapshots (%zu):\n", sl.len);
          for (size_t i = 0; i < sl.len; i++) {
            const salt_stratum_snapshot &sn = sl.items[i];
            printf("  %lld  %-16s %-8s %s\n", (long long)sn.id, sn.label ? sn.label : "",
                   sn.kind ? sn.kind : "", sn.path ? sn.path : "");
          }
        }
        salt_stratum_snapshot_list_free(&sl);
        salt_stratum_free_fields(&s);
      }
    }
  } else if (sub == "snapshot") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else {
      const char *label = args.size() > 2 ? args[2].c_str() : "manual";
      int64_t id = 0;
      if (salt_stratum_snapshot_create(&c, db, args[1].c_str(), label, &id) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        printf("created snapshot %lld\n", (long long)id);
      }
    }
  } else if (sub == "rollback") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else {
      int64_t id = args.size() > 2 ? atoll(args[2].c_str()) : 0;
      if (salt_stratum_rollback(&c, db, args[1].c_str(), id) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        printf("rolled back %s\n", args[1].c_str());
      }
    }
  } else if (sub == "shell") {
    if (args.size() < 2) {
      stratum_usage();
      ret = 2;
    } else {
      salt_stratum s;
      memset(&s, 0, sizeof(s));
      if (salt_stratum_get(db, args[1].c_str(), &s) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        salt_run_opts opts;
        salt_run_opts_default(&opts);
        opts.interactive = true;
        char *argv[] = {(char *)"/bin/sh", (char *)"-l", nullptr};
        int st = 0;
        salt_stratum_run(&s, &opts, argv, &st);
        salt_stratum_free_fields(&s);
        ret = st;
      }
    }
  } else {
    stratum_usage();
    ret = 2;
  }

  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return ret;
}

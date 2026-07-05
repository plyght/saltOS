#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void provider_usage() {
  fprintf(stderr,
          "usage: salt provider <subcommand>\n"
          "  list                              list component providers\n"
          "  set <component> <source> [trust]  set a provider for a component\n"
          "  status [component]                show provider status\n"
          "  rollback <component>              roll a component back to its prior provider\n");
}

static void print_provider_header() {
  printf("%-18s %-14s %-24s %s\n", "COMPONENT", "PROVIDER", "SOURCE", "TRUST");
}

static void print_provider_row(const salt_provider &p) {
  printf("%-18s %-14s %-24s %s\n", p.component ? p.component : "", p.provider ? p.provider : "",
         p.source ? p.source : "", p.trust ? p.trust : "");
}

static int provider_list(salt_strata_db *db) {
  salt_provider_list l;
  salt_provider_list_init(&l);
  int rc = salt_provider_list_all(db, &l);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
  } else {
    print_provider_header();
    for (size_t i = 0; i < l.len; i++) print_provider_row(l.items[i]);
  }
  salt_provider_list_free(&l);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_provider(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    provider_usage();
    return 2;
  }
  const std::string &sub = args[0];

  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }

  int ret = 0;

  if (sub == "list") {
    ret = provider_list(db);
  } else if (sub == "set") {
    if (args.size() < 3) {
      provider_usage();
      ret = 2;
    } else {
      const std::string &component = args[1];
      const std::string &source = args[2];
      std::string provider;
      size_t slash = source.find('/');
      provider = slash == std::string::npos ? source : source.substr(0, slash);
      std::string trust;
      if (args.size() > 3)
        trust = args[3];
      else
        trust = source == "native" ? "native" : "foreign";
      if (provider != "native")
        fprintf(stderr,
                "warning: adopting a foreign provider (%s) crosses a trust boundary\n",
                provider.c_str());
      if (!confirm(o, "adopt this provider?")) {
        ret = 1;
      } else if (salt_provider_set(db, component.c_str(), provider.c_str(), source.c_str(),
                                   trust.c_str()) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        printf("set %s -> %s (%s, %s)\n", component.c_str(), provider.c_str(), source.c_str(),
               trust.c_str());
      }
    }
  } else if (sub == "status") {
    if (args.size() > 1) {
      salt_provider p;
      memset(&p, 0, sizeof(p));
      if (salt_provider_get(db, args[1].c_str(), &p) != SALT_OK) {
        fprintf(stderr, "salt: %s\n", salt_last_error());
        ret = 1;
      } else {
        print_provider_header();
        print_provider_row(p);
        salt_provider_free_fields(&p);
      }
    } else {
      ret = provider_list(db);
    }
  } else if (sub == "rollback") {
    if (args.size() < 2) {
      provider_usage();
      ret = 2;
    } else if (salt_provider_rollback(db, args[1].c_str()) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      ret = 1;
    } else {
      printf("rolled back provider for %s\n", args[1].c_str());
    }
  } else {
    provider_usage();
    ret = 2;
  }

  salt_strata_db_close(db);
  return ret;
}

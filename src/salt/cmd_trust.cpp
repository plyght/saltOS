#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/trust.h"
#include "salt/toml.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *sev_name(salt_risk_severity s) {
  switch (s) {
    case SALT_RISK_BLOCK: return "BLOCK";
    case SALT_RISK_WARN: return "WARN";
    default: return "INFO";
  }
}

static void trust_usage() {
  fprintf(stderr,
          "usage: salt trust <subcommand>\n"
          "  list                          list contributors and trust levels\n"
          "  show <name>                   show a contributor's trust level\n"
          "  set <name> <level> [reason]   set a trust level "
          "(unknown|vouched|maintainer|denounced)\n"
          "  vouch <voucher> <name> [why]  vouch for a contributor\n"
          "  scan <recipe-dir>             run the supply-chain scanner\n"
          "  admit <recipe-dir>            run the repository admission check\n");
}

int cmd_trust(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    trust_usage();
    return 2;
  }
  std::string td = trustdb_for(o);
  const std::string &sub = args[0];

  if (sub == "list") {
    salt_toml *t = salt_toml_parse_file(td.c_str());
    if (!t) {
      printf("no contributors recorded\n");
      return 0;
    }
    const salt_toml *arr = salt_toml_get(t, "contributor");
    size_t n = salt_toml_array_len(arr);
    for (size_t i = 0; i < n; i++) {
      const salt_toml *c = salt_toml_array_at(arr, i);
      printf("%-20s %-12s %s\n", salt_toml_string(c, "name", "?"),
             salt_toml_string(c, "level", "unknown"), salt_toml_string(c, "reason", ""));
    }
    salt_toml_free(t);
    return 0;
  }

  if (sub == "show") {
    if (args.size() < 2) {
      trust_usage();
      return 2;
    }
    salt_trust_level lvl = salt_trust_lookup(td.c_str(), args[1].c_str());
    printf("%s: %s\n", args[1].c_str(), salt_trust_level_name(lvl));
    return 0;
  }

  if (sub == "set") {
    if (args.size() < 3) {
      trust_usage();
      return 2;
    }
    salt_trust_level lvl = salt_trust_level_parse(args[2].c_str());
    const char *by = getenv("USER");
    const char *reason = args.size() > 3 ? args[3].c_str() : "";
    if (salt_trust_set(td.c_str(), args[1].c_str(), lvl, by ? by : "root", reason) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    printf("%s is now %s\n", args[1].c_str(), salt_trust_level_name(lvl));
    return 0;
  }

  if (sub == "vouch") {
    if (args.size() < 3) {
      trust_usage();
      return 2;
    }
    const char *reason = args.size() > 3 ? args[3].c_str() : "";
    if (salt_trust_vouch(td.c_str(), args[1].c_str(), args[2].c_str(), reason) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    printf("%s vouched for %s\n", args[1].c_str(), args[2].c_str());
    return 0;
  }

  if (sub == "scan" || sub == "admit") {
    if (args.size() < 2) {
      trust_usage();
      return 2;
    }
    salt_findings f;
    salt_findings_init(&f);
    if (sub == "admit") {
      salt_recipe_admission_check(args[1].c_str(), &f);
    } else {
      salt_scan_input in;
      memset(&in, 0, sizeof(in));
      in.recipe_path = (char *)args[1].c_str();
      const char *author = getenv("SALT_AUTHOR");
      in.author_level =
          author ? salt_trust_lookup(td.c_str(), author) : SALT_TRUST_UNKNOWN;
      salt_supplychain_scan(&in, &f);
    }
    for (size_t i = 0; i < f.len; i++)
      printf("[%-5s] %s: %s\n", sev_name(f.items[i].severity), f.items[i].code, f.items[i].message);
    bool blocked = salt_findings_has_block(&f);
    if (f.len == 0) printf("clean\n");
    salt_findings_free(&f);
    return blocked ? 1 : 0;
  }

  trust_usage();
  return 2;
}

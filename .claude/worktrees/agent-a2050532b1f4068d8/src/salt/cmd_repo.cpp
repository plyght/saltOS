#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/sign.h"
#include "salt/repo.h"
}

#include <cstdio>
#include <cstring>
#include <string>

static std::string read_secret(const std::string &k) {
  if (k.empty()) return "";
  if (salt_path_exists(k.c_str())) {
    salt_buf b;
    if (salt_read_file(k.c_str(), &b) == SALT_OK) {
      std::string s(b.data, b.len);
      salt_buf_free(&b);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
      return s;
    }
  }
  return k;
}

int cmd_keygen(const Options &o, const std::vector<std::string> &args) {
  (void)o;
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt keygen <dir> <name>\n");
    return 2;
  }
  if (salt_keypair_write(args[0].c_str(), args[1].c_str()) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  printf("wrote %s/%s.pub and %s/%s.sec\n", args[0].c_str(), args[1].c_str(), args[0].c_str(),
         args[1].c_str());
  return 0;
}

int cmd_sign(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt --key <sec> sign <file>\n");
    return 2;
  }
  std::string sec = read_secret(o.key);
  if (sec.empty()) {
    fprintf(stderr, "salt: no secret key (use --key)\n");
    return 1;
  }
  char sig[SALT_SIG_HEXLEN + 1];
  if (salt_sign_file(args[0].c_str(), sec.c_str(), sig) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  std::string out = args[0] + ".sig";
  salt_write_file(out.c_str(), sig, strlen(sig), 0644);
  printf("wrote %s\n", out.c_str());
  return 0;
}

int cmd_repo(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2 || args[0] != "publish") {
    fprintf(stderr, "usage: salt [--key <sec>] repo publish <dir>\n");
    return 2;
  }
  const std::string &dir = args[1];
  RepoConf c = load_repo_conf(o);
  std::string sec = read_secret(o.key);
  if (sec.empty())
    fprintf(stderr, "warning: no secret key; index will not be signed\n");
  if (salt_repo_publish(dir.c_str(), c.name.c_str(), arch_detect().c_str(), sec.c_str()) !=
      SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  printf("published %s index for %s%s\n", c.name.c_str(), arch_detect().c_str(),
         sec.empty() ? "" : " (signed)");
  return 0;
}

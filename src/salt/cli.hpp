#ifndef SALT_CLI_HPP
#define SALT_CLI_HPP

#include <string>
#include <vector>

struct Options {
  std::string root = "/";
  std::string repo;
  std::string key;
  bool yes = false;
};

struct RepoConf {
  std::string name = "current";
  std::string source;
  std::string key;
};

std::string arch_detect();
std::string path_join(const std::string &a, const std::string &b);
RepoConf load_repo_conf(const Options &o);
std::string db_path_for(const Options &o);
std::string cache_dir_for(const Options &o);
std::string index_path_for(const Options &o);
std::string trustdb_for(const Options &o);
std::string strata_db_path_for(const Options &o);
bool confirm(const Options &o, const std::string &prompt);

int cmd_sync(const Options &o, const std::vector<std::string> &args);
int cmd_install(const Options &o, const std::vector<std::string> &args);
int cmd_remove(const Options &o, const std::vector<std::string> &args);
int cmd_update(const Options &o, const std::vector<std::string> &args);
int cmd_rollback(const Options &o, const std::vector<std::string> &args);
int cmd_deployments(const Options &o, const std::vector<std::string> &args);
int cmd_verify(const Options &o, const std::vector<std::string> &args);

int cmd_search(const Options &o, const std::vector<std::string> &args);
int cmd_query(const Options &o, const std::vector<std::string> &args);
int cmd_files(const Options &o, const std::vector<std::string> &args);
int cmd_owner(const Options &o, const std::vector<std::string> &args);
int cmd_list(const Options &o, const std::vector<std::string> &args);

int cmd_build(const Options &o, const std::vector<std::string> &args);
int cmd_lint(const Options &o, const std::vector<std::string> &args);
int cmd_sign(const Options &o, const std::vector<std::string> &args);
int cmd_repo(const Options &o, const std::vector<std::string> &args);
int cmd_keygen(const Options &o, const std::vector<std::string> &args);

int cmd_trust(const Options &o, const std::vector<std::string> &args);

int cmd_stratum(const Options &o, const std::vector<std::string> &args);
int cmd_run(const Options &o, const std::vector<std::string> &args);
int cmd_pkg(const Options &o, const std::vector<std::string> &args);
int cmd_expose(const Options &o, const std::vector<std::string> &args);
int cmd_unexpose(const Options &o, const std::vector<std::string> &args);
int cmd_exposed(const Options &o, const std::vector<std::string> &args);
int cmd_expose_desktop(const Options &o, const std::vector<std::string> &args);
int cmd_provider(const Options &o, const std::vector<std::string> &args);
int cmd_service(const Options &o, const std::vector<std::string> &args);

#endif

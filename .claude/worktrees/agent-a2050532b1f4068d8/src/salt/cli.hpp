#ifndef SALT_CLI_HPP
#define SALT_CLI_HPP

#include <string>
#include <vector>

struct Options {
  std::string root = "/";
  std::string repo;
  std::string key;
  bool yes = false;
  std::string expose_mode;
};

struct PkgRef {
  std::string stratum;
  std::string pkg;
  bool foreign = false;
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

PkgRef parse_pkgref(const std::string &arg);
std::string auto_expose_mode(const Options &o);
bool native_index_has(const Options &o, const std::string &name);

int stratum_install(const Options &o, const std::string &stratum,
                    const std::vector<std::string> &pkgs);
int stratum_remove(const Options &o, const std::string &stratum,
                   const std::vector<std::string> &pkgs);
int stratum_search(const Options &o, const std::string &stratum, const std::string &term);
int stratum_update(const Options &o, const std::string &stratum);
std::vector<std::string> list_strata_names(const Options &o);
std::string choose_stratum_for(const Options &o, const std::string &name);
std::string resolve_stratum_recipe(const Options &o, const std::string &arg);
int ensure_stratum(const Options &o, const std::string &name);
bool expose_pm_enabled(const Options &o);
bool expose_all_enabled(const Options &o);
bool auto_service_enabled(const Options &o);
void expose_pm_for(const Options &o, const std::string &stratum);
void expose_all_for(const Options &o, const std::string &stratum);

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
int cmd_pm(const Options &o, const std::vector<std::string> &args);
int cmd_expose(const Options &o, const std::vector<std::string> &args);
int cmd_unexpose(const Options &o, const std::vector<std::string> &args);
int cmd_exposed(const Options &o, const std::vector<std::string> &args);
int cmd_expose_desktop(const Options &o, const std::vector<std::string> &args);
int cmd_expose_all(const Options &o, const std::vector<std::string> &args);
int cmd_provider(const Options &o, const std::vector<std::string> &args);
int cmd_service(const Options &o, const std::vector<std::string> &args);

int cmd_config(const Options &o, const std::vector<std::string> &args);
int cmd_lock(const Options &o, const std::vector<std::string> &args);

#endif

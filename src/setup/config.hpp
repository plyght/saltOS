#pragma once

#include <string>
#include <vector>

namespace setup {

struct Config {
  std::string hostname = "saltos";
  std::string locale = "en_US.UTF-8";
  std::string timezone = "UTC";
  std::string keymap = "us";
  std::string xkb_layout;
  std::string xkb_variant;

  std::string disk;
  std::string mode = "erase";
  std::string target;
  std::string filesystem = "btrfs";
  bool encrypt = false;
  std::string passphrase;
  std::string passphrase_file;
  std::string swap = "none";
  std::string swap_size = "auto";
  std::string root_size;
  std::string desktop = "auto";

  std::string firmware = "auto";
  bool os_prober = true;
  std::string cmdline;
  std::string shim = "auto";

  std::string username = "salt";
  std::string password;
  std::string password_hash;
  std::string shell = "/bin/bash";
  bool sudo = true;
  bool autologin = false;
  bool create_user = true;

  std::string network = "dhcp";
  std::string wifi_ssid;
  std::string wifi_psk;

  std::string kernel = "native";
  std::string distro;
};

struct CliOptions {
  std::string from;
  std::vector<std::string> profiles;
  std::vector<std::string> sets;
  std::string mnt = "/mnt/saltos-target";
  bool dump_config = false;
  bool yes = false;
  bool help = false;
};

int parse_cli(int argc, char **argv, CliOptions &opts, std::string &err);
std::string usage_text();

bool load_toml(const std::string &text, Config &cfg, std::string &err);
bool load_toml_file(const std::string &path, Config &cfg, std::string &err);
bool set_value(Config &cfg, const std::string &dotted, const std::string &value, std::string &err);
bool validate(const Config &cfg, bool interactive, std::string &err);
std::string to_toml(const Config &cfg, bool include_secrets);

bool parse_size_mib(const std::string &s, unsigned long long &mib);
bool parse_bool(const std::string &s, bool &out);
std::string default_swap_size(unsigned long long mem_kib);

}

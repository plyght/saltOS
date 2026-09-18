#include "config.hpp"

extern "C" {
#include "salt/toml.h"
#include "salt/util.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace setup {

namespace {

enum class Kind { Str, Bool };

struct Field {
  const char *key;
  Kind kind;
  bool secret;
  std::string Config::*s;
  bool Config::*b;
};

const std::vector<Field> &fields() {
  static const std::vector<Field> f = {
      {"system.hostname", Kind::Str, false, &Config::hostname, nullptr},
      {"system.locale", Kind::Str, false, &Config::locale, nullptr},
      {"system.timezone", Kind::Str, false, &Config::timezone, nullptr},
      {"system.keymap", Kind::Str, false, &Config::keymap, nullptr},
      {"system.xkb_layout", Kind::Str, false, &Config::xkb_layout, nullptr},
      {"system.xkb_variant", Kind::Str, false, &Config::xkb_variant, nullptr},
      {"install.disk", Kind::Str, false, &Config::disk, nullptr},
      {"install.mode", Kind::Str, false, &Config::mode, nullptr},
      {"install.target", Kind::Str, false, &Config::target, nullptr},
      {"install.filesystem", Kind::Str, false, &Config::filesystem, nullptr},
      {"install.encrypt", Kind::Bool, false, nullptr, &Config::encrypt},
      {"install.passphrase", Kind::Str, true, &Config::passphrase, nullptr},
      {"install.passphrase_file", Kind::Str, false, &Config::passphrase_file, nullptr},
      {"install.swap", Kind::Str, false, &Config::swap, nullptr},
      {"install.swap_size", Kind::Str, false, &Config::swap_size, nullptr},
      {"install.swap_device", Kind::Str, false, &Config::swap_device, nullptr},
      {"install.root_size", Kind::Str, false, &Config::root_size, nullptr},
      {"install.desktop", Kind::Str, false, &Config::desktop, nullptr},
      {"boot.firmware", Kind::Str, false, &Config::firmware, nullptr},
      {"boot.os_prober", Kind::Bool, false, nullptr, &Config::os_prober},
      {"boot.cmdline", Kind::Str, false, &Config::cmdline, nullptr},
      {"boot.shim", Kind::Str, false, &Config::shim, nullptr},
      {"user.name", Kind::Str, false, &Config::username, nullptr},
      {"user.password", Kind::Str, true, &Config::password, nullptr},
      {"user.password_hash", Kind::Str, true, &Config::password_hash, nullptr},
      {"user.root_password", Kind::Str, true, &Config::root_password, nullptr},
      {"user.root_password_hash", Kind::Str, true, &Config::root_password_hash, nullptr},
      {"user.shell", Kind::Str, false, &Config::shell, nullptr},
      {"user.sudo", Kind::Bool, false, nullptr, &Config::sudo},
      {"user.autologin", Kind::Bool, false, nullptr, &Config::autologin},
      {"user.create", Kind::Bool, false, nullptr, &Config::create_user},
      {"network.mode", Kind::Str, false, &Config::network, nullptr},
      {"network.wifi_ssid", Kind::Str, false, &Config::wifi_ssid, nullptr},
      {"network.wifi_psk", Kind::Str, true, &Config::wifi_psk, nullptr},
      {"kernel.source", Kind::Str, false, &Config::kernel, nullptr},
  };
  return f;
}

const Field *find_field(const std::string &key) {
  for (const auto &f : fields())
    if (key == f.key) return &f;
  return nullptr;
}

std::string quote(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(c);
    }
  }
  out += "\"";
  return out;
}

bool one_of(const std::string &v, std::initializer_list<const char *> allowed) {
  for (const char *a : allowed)
    if (v == a) return true;
  return false;
}

std::string join(std::initializer_list<const char *> allowed) {
  std::string out;
  for (const char *a : allowed) {
    if (!out.empty()) out += "|";
    out += a;
  }
  return out;
}

#define ENUM_CHECK(field, value, ...)                                                        \
  do {                                                                                       \
    if (!one_of(value, {__VA_ARGS__})) {                                                     \
      err = std::string(field) + " must be one of " + join({__VA_ARGS__}) + " (got '" + value + \
            "')";                                                                            \
      return false;                                                                          \
    }                                                                                        \
  } while (0)

}

std::string usage_text() {
  return "usage: salt-setup [options]\n"
         "  --from <system.toml>     non-interactive install; every answer comes from the file\n"
         "  --profile <file>         preseed answers from a profile (repeatable; later wins)\n"
         "  --set <section.key=val>  override one value (repeatable, applied last)\n"
         "  --disk <dev>             target disk (install.disk)\n"
         "  --user <name>            primary user (user.name)\n"
         "  --target <dir>           install into an already partitioned and mounted target\n"
         "  --mnt <dir>              work mount point (default /mnt/saltos-target)\n"
         "  --yes                    skip the interactive confirmation\n"
         "  --dump-config            print the effective configuration and exit\n"
         "  -h, --help               show this help\n";
}

int parse_cli(int argc, char **argv, CliOptions &opts, std::string &err) {
  auto need = [&](int i, const std::string &a) -> bool {
    if (i + 1 < argc) return true;
    err = a + " requires an argument";
    return false;
  };
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      opts.help = true;
      return 0;
    } else if (a == "--from") {
      if (!need(i, a)) return 2;
      opts.from = argv[++i];
    } else if (a == "--profile") {
      if (!need(i, a)) return 2;
      opts.profiles.push_back(argv[++i]);
    } else if (a == "--set") {
      if (!need(i, a)) return 2;
      std::string kv = argv[++i];
      if (kv.find('=') == std::string::npos) {
        err = "--set expects section.key=value";
        return 2;
      }
      opts.sets.push_back(kv);
    } else if (a == "--disk") {
      if (!need(i, a)) return 2;
      opts.sets.push_back(std::string("install.disk=") + argv[++i]);
    } else if (a == "--user") {
      if (!need(i, a)) return 2;
      opts.sets.push_back(std::string("user.name=") + argv[++i]);
    } else if (a == "--target") {
      if (!need(i, a)) return 2;
      opts.sets.push_back("install.mode=mounted");
      opts.sets.push_back(std::string("install.target=") + argv[++i]);
    } else if (a == "--mnt") {
      if (!need(i, a)) return 2;
      opts.mnt = argv[++i];
    } else if (a == "--yes" || a == "-y") {
      opts.yes = true;
    } else if (a == "--dump-config") {
      opts.dump_config = true;
    } else {
      err = "unknown argument: " + a;
      return 2;
    }
  }
  return 0;
}

bool parse_bool(const std::string &s, bool &out) {
  if (s == "true" || s == "yes" || s == "on" || s == "1" || s == "y") {
    out = true;
    return true;
  }
  if (s == "false" || s == "no" || s == "off" || s == "0" || s == "n") {
    out = false;
    return true;
  }
  return false;
}

bool set_value(Config &cfg, const std::string &dotted, const std::string &value, std::string &err) {
  if (dotted == "stratum.name" || dotted == "stratum") {
    cfg.distro = value;
    return true;
  }
  const Field *f = find_field(dotted);
  if (!f) {
    err = "unknown configuration key: " + dotted;
    return false;
  }
  if (f->kind == Kind::Str) {
    cfg.*(f->s) = value;
    return true;
  }
  bool b = false;
  if (!parse_bool(value, b)) {
    err = dotted + " expects true or false (got '" + value + "')";
    return false;
  }
  cfg.*(f->b) = b;
  return true;
}

bool load_toml(const std::string &text, Config &cfg, std::string &err) {
  salt_toml *root = salt_toml_parse(text.c_str(), text.size());
  if (!root) {
    err = "invalid TOML";
    return false;
  }
  for (const auto &f : fields()) {
    const salt_toml *t = salt_toml_path(root, f.key);
    if (!t) continue;
    if (f.kind == Kind::Str) {
      if (salt_toml_typeof(t) != SALT_TOML_STRING) {
        err = std::string(f.key) + " must be a string";
        salt_toml_free(root);
        return false;
      }
      cfg.*(f.s) = salt_toml_as_string(t);
    } else {
      if (salt_toml_typeof(t) != SALT_TOML_BOOL) {
        err = std::string(f.key) + " must be a boolean";
        salt_toml_free(root);
        return false;
      }
      cfg.*(f.b) = salt_toml_as_bool(t, cfg.*(f.b));
    }
  }
  const salt_toml *strata = salt_toml_get(root, "stratum");
  if (strata && salt_toml_typeof(strata) == SALT_TOML_ARRAY) {
    size_t n = salt_toml_array_len(strata);
    std::string first;
    for (size_t i = 0; i < n; i++) {
      const salt_toml *s = salt_toml_array_at(strata, i);
      const char *name = salt_toml_string(s, "name", nullptr);
      if (!name) continue;
      if (first.empty()) first = name;
      const char *role = salt_toml_string(s, "role", "");
      if (strcmp(role, "primary") == 0) {
        cfg.distro = name;
        first.clear();
        break;
      }
    }
    if (!first.empty()) cfg.distro = first;
  } else if (strata && salt_toml_typeof(strata) == SALT_TOML_TABLE) {
    const char *name = salt_toml_string(strata, "name", nullptr);
    if (name) cfg.distro = name;
  }
  salt_toml_free(root);
  return true;
}

bool load_toml_file(const std::string &path, Config &cfg, std::string &err) {
  salt_buf b;
  salt_buf_init(&b);
  if (salt_read_file(path.c_str(), &b) != SALT_OK) {
    salt_buf_free(&b);
    err = "cannot read " + path;
    return false;
  }
  std::string text(b.data ? b.data : "", b.len);
  salt_buf_free(&b);
  if (!load_toml(text, cfg, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

bool parse_size_mib(const std::string &in, unsigned long long &mib) {
  std::string s;
  for (char c : in)
    if (c != ' ' && c != '\t') s.push_back(c);
  if (s.empty()) return false;
  char *end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str() || v < 0) return false;
  std::string unit = end;
  for (auto &c : unit) c = (char)toupper((unsigned char)c);
  if (unit == "" || unit == "M" || unit == "MB" || unit == "MIB") {
  } else if (unit == "G" || unit == "GB" || unit == "GIB") {
    v *= 1024.0;
  } else if (unit == "T" || unit == "TB" || unit == "TIB") {
    v *= 1024.0 * 1024.0;
  } else if (unit == "K" || unit == "KB" || unit == "KIB") {
    v /= 1024.0;
  } else {
    return false;
  }
  mib = (unsigned long long)(v + 0.5);
  return true;
}

std::string default_swap_size(unsigned long long mem_kib) {
  unsigned long long mib = mem_kib / 1024;
  if (mib == 0) mib = 2048;
  if (mib > 8192) mib = 8192;
  if (mib < 1024) mib = 1024;
  return std::to_string(mib) + "M";
}

bool validate(const Config &cfg, bool interactive, std::string &err) {
  ENUM_CHECK("install.mode", cfg.mode, "erase", "alongside", "mounted");
  ENUM_CHECK("install.filesystem", cfg.filesystem, "btrfs", "ext4");
  ENUM_CHECK("install.swap", cfg.swap, "none", "file", "partition", "zram");
  ENUM_CHECK("install.desktop", cfg.desktop, "auto", "keep", "none");
  ENUM_CHECK("boot.firmware", cfg.firmware, "auto", "uefi", "bios", "both");
  ENUM_CHECK("boot.shim", cfg.shim, "auto", "yes", "no");
  ENUM_CHECK("network.mode", cfg.network, "dhcp", "wifi", "none");
  if (cfg.mode == "mounted") {
    if (cfg.target.empty()) {
      err = "install.target is required when install.mode = \"mounted\"";
      return false;
    }
  } else if (cfg.disk.empty()) {
    err = "install.disk is required";
    return false;
  } else if (cfg.disk.rfind("/dev/", 0) != 0) {
    err = "install.disk must be a /dev path (got '" + cfg.disk + "')";
    return false;
  }
  if (cfg.mode == "mounted" && cfg.swap == "partition" && cfg.swap_device.empty()) {
    err = "install.swap_device is required for install.swap = \"partition\" with "
          "install.mode = \"mounted\"";
    return false;
  }
  if (!cfg.swap_device.empty() && (cfg.mode != "mounted" || cfg.swap != "partition")) {
    err = "install.swap_device only applies to install.mode = \"mounted\" with "
          "install.swap = \"partition\"";
    return false;
  }
  if (!cfg.sudo && cfg.root_password.empty() && cfg.root_password_hash.empty()) {
    err = "user.root_password (or user.root_password_hash) is required when user.sudo = false, "
          "otherwise no account can administer the system";
    return false;
  }
  if (cfg.distro.empty()) {
    err = "a primary [[stratum]] name is required";
    return false;
  }
  if (cfg.hostname.empty()) {
    err = "system.hostname must not be empty";
    return false;
  }
  for (char c : cfg.hostname)
    if (!(isalnum((unsigned char)c) || c == '-' || c == '.')) {
      err = "system.hostname contains invalid character '" + std::string(1, c) + "'";
      return false;
    }
  if (cfg.create_user) {
    if (cfg.username.empty()) {
      err = "user.name must not be empty";
      return false;
    }
    if (cfg.username == "root") {
      err = "user.name must not be root";
      return false;
    }
    for (char c : cfg.username)
      if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-' || c == '_')) {
        err = "user.name contains invalid character '" + std::string(1, c) + "'";
        return false;
      }
    if (!interactive && cfg.password.empty() && cfg.password_hash.empty()) {
      err = "user.password or user.password_hash is required for a non-interactive install";
      return false;
    }
  }
  if (cfg.encrypt && cfg.mode != "mounted" && !interactive && cfg.passphrase.empty() &&
      cfg.passphrase_file.empty()) {
    err = "install.passphrase or install.passphrase_file is required when install.encrypt = true";
    return false;
  }
  unsigned long long mib = 0;
  if (cfg.swap != "none" && cfg.swap_size != "auto" && !parse_size_mib(cfg.swap_size, mib)) {
    err = "install.swap_size must be 'auto' or a size like 4G (got '" + cfg.swap_size + "')";
    return false;
  }
  if (!cfg.root_size.empty() && !parse_size_mib(cfg.root_size, mib)) {
    err = "install.root_size must be a size like 40G (got '" + cfg.root_size + "')";
    return false;
  }
  if (!cfg.root_size.empty() && cfg.mode != "alongside") {
    err = "install.root_size only applies to install.mode = \"alongside\"";
    return false;
  }
  if (cfg.network == "wifi" && cfg.wifi_ssid.empty()) {
    err = "network.wifi_ssid is required when network.mode = \"wifi\"";
    return false;
  }
  if (cfg.kernel.empty()) {
    err = "kernel.source must not be empty";
    return false;
  }
  return true;
}

std::string to_toml(const Config &cfg, bool include_secrets) {
  std::string out;
  std::string section;
  for (const auto &f : fields()) {
    if (f.secret && !include_secrets) continue;
    std::string key = f.key;
    size_t dot = key.find('.');
    std::string sec = key.substr(0, dot);
    std::string name = key.substr(dot + 1);
    if (f.kind == Kind::Str && (cfg.*(f.s)).empty()) continue;
    if (sec != section) {
      if (!out.empty()) out += "\n";
      out += "[" + sec + "]\n";
      section = sec;
    }
    if (f.kind == Kind::Str)
      out += name + " = " + quote(cfg.*(f.s)) + "\n";
    else
      out += name + " = " + (cfg.*(f.b) ? "true" : "false") + "\n";
  }
  out += "\n[[stratum]]\nname = " + quote(cfg.distro) + "\nrole = \"primary\"\nexpose = true\n";
  return out;
}

}

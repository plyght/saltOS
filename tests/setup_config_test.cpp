#include "config.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                                     \
  do {                                                       \
    g_total++;                                               \
    if (!(cond)) {                                           \
      printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                              \
    }                                                        \
  } while (0)

using setup::Config;

static void test_defaults(void) {
  Config c;
  std::string err;
  CHECK(c.mode == "erase", "default mode erase");
  CHECK(c.filesystem == "btrfs", "default fs btrfs");
  CHECK(c.swap == "none", "default swap none");
  CHECK(c.firmware == "auto", "default firmware auto");
  CHECK(c.network == "dhcp", "default network dhcp");
  CHECK(!c.encrypt, "default unencrypted");
  CHECK(c.os_prober, "default os-prober on");
  CHECK(!setup::validate(c, false, err), "defaults alone do not validate");
  CHECK(err.find("install.disk") != std::string::npos, "missing disk reported");
}

static void test_full_toml(void) {
  const char *toml =
      "[system]\n"
      "hostname = \"box\"\n"
      "locale = \"de_DE.UTF-8\"\n"
      "timezone = \"Europe/Berlin\"\n"
      "keymap = \"de-latin1\"\n"
      "xkb_layout = \"de\"\n"
      "\n"
      "[install]\n"
      "disk = \"/dev/vda\"\n"
      "mode = \"alongside\"\n"
      "filesystem = \"ext4\"\n"
      "encrypt = true\n"
      "passphrase = \"secret\"\n"
      "swap = \"partition\"\n"
      "swap_size = \"2G\"\n"
      "root_size = \"40G\"\n"
      "desktop = \"none\"\n"
      "\n"
      "[boot]\n"
      "firmware = \"both\"\n"
      "os_prober = false\n"
      "cmdline = \"console=ttyS0,115200\"\n"
      "shim = \"no\"\n"
      "\n"
      "[user]\n"
      "name = \"alice\"\n"
      "password = \"pw\"\n"
      "root_password_hash = \"$6$abc\"\n"
      "shell = \"/bin/sh\"\n"
      "sudo = false\n"
      "autologin = true\n"
      "\n"
      "[network]\n"
      "mode = \"wifi\"\n"
      "wifi_ssid = \"home\"\n"
      "wifi_psk = \"psk\"\n"
      "\n"
      "[kernel]\n"
      "source = \"arch\"\n"
      "\n"
      "[[stratum]]\n"
      "name = \"debian\"\n"
      "role = \"secondary\"\n"
      "\n"
      "[[stratum]]\n"
      "name = \"arch\"\n"
      "role = \"primary\"\n"
      "expose = true\n";
  Config c;
  std::string err;
  CHECK(setup::load_toml(toml, c, err), "full toml loads");
  CHECK(c.hostname == "box", "hostname");
  CHECK(c.locale == "de_DE.UTF-8", "locale");
  CHECK(c.timezone == "Europe/Berlin", "timezone");
  CHECK(c.keymap == "de-latin1", "keymap");
  CHECK(c.xkb_layout == "de", "xkb layout");
  CHECK(c.disk == "/dev/vda", "disk");
  CHECK(c.mode == "alongside", "mode");
  CHECK(c.filesystem == "ext4", "filesystem");
  CHECK(c.encrypt, "encrypt");
  CHECK(c.passphrase == "secret", "passphrase");
  CHECK(c.swap == "partition", "swap");
  CHECK(c.swap_size == "2G", "swap size");
  CHECK(c.root_size == "40G", "root size");
  CHECK(c.desktop == "none", "desktop");
  CHECK(c.firmware == "both", "firmware");
  CHECK(!c.os_prober, "os-prober off");
  CHECK(c.cmdline == "console=ttyS0,115200", "cmdline");
  CHECK(c.shim == "no", "shim");
  CHECK(c.username == "alice", "username");
  CHECK(c.password == "pw", "password");
  CHECK(c.root_password_hash == "$6$abc", "root password hash");
  CHECK(c.shell == "/bin/sh", "shell");
  CHECK(!c.sudo, "sudo off");
  CHECK(c.autologin, "autologin");
  CHECK(c.network == "wifi", "network wifi");
  CHECK(c.wifi_ssid == "home", "ssid");
  CHECK(c.wifi_psk == "psk", "psk");
  CHECK(c.kernel == "arch", "kernel source");
  CHECK(c.distro == "arch", "primary stratum wins over first listed");
  CHECK(setup::validate(c, false, err), err.c_str());
}

static void test_roundtrip(void) {
  Config c;
  c.disk = "/dev/sda";
  c.distro = "void";
  c.password = "pw";
  c.wifi_psk = "psk";
  c.passphrase = "pp";
  c.encrypt = true;
  c.swap = "zram";
  std::string err;
  std::string pub = setup::to_toml(c, false);
  CHECK(pub.find("password") == std::string::npos, "secrets omitted from public toml");
  CHECK(pub.find("psk") == std::string::npos, "wifi psk omitted");
  CHECK(pub.find("passphrase") == std::string::npos, "passphrase omitted");
  CHECK(pub.find("[[stratum]]\nname = \"void\"\nrole = \"primary\"") != std::string::npos,
        "stratum block");
  CHECK(pub.find("encrypt = true") != std::string::npos, "encrypt written");
  std::string full = setup::to_toml(c, true);
  Config back;
  CHECK(setup::load_toml(full, back, err), "roundtrip parses");
  CHECK(back.disk == c.disk, "roundtrip disk");
  CHECK(back.distro == c.distro, "roundtrip distro");
  CHECK(back.password == "pw", "roundtrip password");
  CHECK(back.swap == "zram", "roundtrip swap");
  CHECK(back.encrypt, "roundtrip encrypt");
  CHECK(setup::to_toml(back, true) == full, "roundtrip stable");
}

static void test_set_and_validate(void) {
  Config c;
  std::string err;
  CHECK(setup::set_value(c, "install.disk", "/dev/nvme0n1", err), "set disk");
  CHECK(setup::set_value(c, "stratum.name", "arch", err), "set stratum");
  CHECK(setup::set_value(c, "user.password", "x", err), "set password");
  CHECK(setup::set_value(c, "boot.os_prober", "no", err), "set bool");
  CHECK(!c.os_prober, "bool applied");
  CHECK(!setup::set_value(c, "boot.os_prober", "maybe", err), "bad bool rejected");
  CHECK(!setup::set_value(c, "nope.key", "1", err), "unknown key rejected");
  CHECK(setup::validate(c, false, err), err.c_str());

  c.filesystem = "xfs";
  CHECK(!setup::validate(c, false, err), "bad filesystem rejected");
  c.filesystem = "btrfs";
  c.swap = "partition";
  c.swap_size = "lots";
  CHECK(!setup::validate(c, false, err), "bad swap size rejected");
  c.swap_size = "auto";
  CHECK(setup::validate(c, false, err), "auto swap size ok");
  c.encrypt = true;
  CHECK(!setup::validate(c, false, err), "encrypt without passphrase rejected");
  c.passphrase_file = "/run/key";
  CHECK(setup::validate(c, false, err), "passphrase file ok");
  c.mode = "mounted";
  CHECK(!setup::validate(c, false, err), "mounted needs target");
  c.target = "/tmp/root";
  CHECK(!setup::validate(c, false, err), "mounted swap partition needs swap_device");
  c.swap_device = "/dev/vda3";
  CHECK(setup::validate(c, false, err), err.c_str());
  c.swap = "file";
  CHECK(!setup::validate(c, false, err), "swap_device without swap partition rejected");
  c.swap_device.clear();
  CHECK(setup::validate(c, false, err), err.c_str());
  c.sudo = false;
  CHECK(!setup::validate(c, false, err), "no sudo and no root password rejected");
  c.root_password = "toor";
  CHECK(setup::validate(c, false, err), err.c_str());
  CHECK(setup::to_toml(c, false).find("toor") == std::string::npos, "root password omitted");
  CHECK(setup::to_toml(c, true).find("root_password = \"toor\"") != std::string::npos,
        "root password kept with secrets");
  c.sudo = true;
  c.root_password.clear();
  c.mode = "erase";
  c.root_size = "10G";
  CHECK(!setup::validate(c, false, err), "root_size only for alongside");
  c.mode = "alongside";
  CHECK(setup::validate(c, false, err), err.c_str());
  c.username = "Bad User";
  CHECK(!setup::validate(c, false, err), "bad username rejected");
  c.username = "root";
  CHECK(!setup::validate(c, false, err), "root username rejected");
  c.username = "ok";
  c.password.clear();
  CHECK(!setup::validate(c, false, err), "non-interactive needs password");
  CHECK(setup::validate(c, true, err), "interactive prompts for password later");
  c.create_user = false;
  CHECK(setup::validate(c, false, err), "user.create=false skips password");
  c.network = "wifi";
  CHECK(!setup::validate(c, false, err), "wifi needs ssid");
  c.wifi_ssid = "n";
  CHECK(setup::validate(c, false, err), err.c_str());
  c.hostname = "bad host";
  CHECK(!setup::validate(c, false, err), "bad hostname rejected");
}

static void test_cli(void) {
  setup::CliOptions o;
  std::string err;
  const char *argv1[] = {
      "salt-setup", "--from", "/x.toml",      "--disk", "/dev/vdb",           "--user",
      "bob",        "--mnt",  "/m",           "--set",  "boot.firmware=bios", "--profile",
      "/p.toml",    "--yes",  "--dump-config"};
  CHECK(setup::parse_cli(15, (char **)argv1, o, err) == 0, "cli parses");
  CHECK(o.from == "/x.toml", "from");
  CHECK(o.mnt == "/m", "mnt");
  CHECK(o.yes, "yes");
  CHECK(o.dump_config, "dump");
  CHECK(o.profiles.size() == 1 && o.profiles[0] == "/p.toml", "profile");
  CHECK(o.sets.size() == 3, "sets collected");
  CHECK(o.sets[0] == "install.disk=/dev/vdb", "disk set");
  CHECK(o.sets[1] == "user.name=bob", "user set");
  CHECK(o.sets[2] == "boot.firmware=bios", "explicit set");

  setup::CliOptions t;
  const char *argv2[] = {"salt-setup", "--target", "/tmp/calamares-root"};
  CHECK(setup::parse_cli(3, (char **)argv2, t, err) == 0, "target parses");
  CHECK(t.sets.size() == 2 && t.sets[0] == "install.mode=mounted", "target sets mode");

  setup::CliOptions bad;
  const char *argv3[] = {"salt-setup", "--bogus"};
  CHECK(setup::parse_cli(2, (char **)argv3, bad, err) == 2, "unknown arg is exit 2");
  const char *argv4[] = {"salt-setup", "--from"};
  CHECK(setup::parse_cli(2, (char **)argv4, bad, err) == 2, "missing value is exit 2");
  const char *argv5[] = {"salt-setup", "--set", "novalue"};
  CHECK(setup::parse_cli(3, (char **)argv5, bad, err) == 2, "set without = is exit 2");
  const char *argv6[] = {"salt-setup", "-h"};
  CHECK(setup::parse_cli(2, (char **)argv6, bad, err) == 0 && bad.help, "help");
}

static void test_sizes(void) {
  unsigned long long mib = 0;
  CHECK(setup::parse_size_mib("4G", mib) && mib == 4096, "4G");
  CHECK(setup::parse_size_mib("512M", mib) && mib == 512, "512M");
  CHECK(setup::parse_size_mib("1.5GiB", mib) && mib == 1536, "1.5GiB");
  CHECK(setup::parse_size_mib("2048", mib) && mib == 2048, "bare MiB");
  CHECK(!setup::parse_size_mib("abc", mib), "garbage rejected");
  CHECK(!setup::parse_size_mib("", mib), "empty rejected");
  CHECK(setup::default_swap_size(4ULL * 1024 * 1024) == "4096M", "swap = ram at 4G");
  CHECK(setup::default_swap_size(64ULL * 1024 * 1024) == "8192M", "swap capped at 8G");
  CHECK(setup::default_swap_size(512ULL * 1024) == "1024M", "swap floor 1G");
  bool b = false;
  CHECK(setup::parse_bool("yes", b) && b, "yes");
  CHECK(setup::parse_bool("0", b) && !b, "0");
  CHECK(!setup::parse_bool("nah", b), "bad bool");
}

int main(void) {
  test_defaults();
  test_full_toml();
  test_roundtrip();
  test_set_and_validate();
  test_cli();
  test_sizes();
  printf("%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}

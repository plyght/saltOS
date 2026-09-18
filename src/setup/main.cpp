extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
}

#include "config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

const char *kProg = "salt-setup";

void fail(const std::string &msg) {
  fprintf(stderr, "%s: %s\n", kProg, msg.c_str());
  exit(1);
}

void info(const std::string &msg) { printf("==> %s\n", msg.c_str()); }

int run(const std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int run_quiet(const std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void must(const std::vector<std::string> &args, const std::string &what) {
  if (run(args) != 0) fail("failed: " + what);
}

int chroot_run(const std::string &mnt, const std::vector<std::string> &args) {
  std::vector<std::string> full = {"chroot", mnt};
  full.insert(full.end(), args.begin(), args.end());
  return run(full);
}

void chroot_must(const std::string &mnt, const std::vector<std::string> &args,
                 const std::string &what) {
  if (chroot_run(mnt, args) != 0) fail("failed: " + what);
}

bool chroot_has(const std::string &mnt, const std::string &cmd) {
  return run_quiet({"chroot", mnt, "sh", "-c", "command -v " + cmd}) == 0;
}

int run_capture(const std::vector<std::string> &args, std::string &out) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
  close(fds[0]);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int run_stdin(const std::vector<std::string> &args, const std::string &input) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(fds[0], STDIN_FILENO);
    close(fds[0]);
    close(fds[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[0]);
  size_t off = 0;
  while (off < input.size()) {
    ssize_t w = write(fds[1], input.data() + off, input.size() - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  close(fds[1]);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string line;
  while (std::getline(ss, line)) out.push_back(line);
  return out;
}

std::vector<std::string> split_ws(const std::string &s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string w;
  while (ss >> w) out.push_back(w);
  return out;
}

std::string prompt(const std::string &q, const std::string &def) {
  printf("%s", q.c_str());
  if (!def.empty()) printf(" [%s]", def.c_str());
  printf(": ");
  fflush(stdout);
  std::string line;
  int ch;
  while ((ch = getchar()) != EOF && ch != '\n') line.push_back((char)ch);
  if (ch == EOF && line.empty()) fail("end of input while reading answers");
  line = trim(line);
  return line.empty() ? def : line;
}

std::string prompt_secret(const std::string &q) {
  printf("%s: ", q.c_str());
  fflush(stdout);
  struct termios old{};
  bool restored = false;
  if (tcgetattr(STDIN_FILENO, &old) == 0) {
    struct termios noecho = old;
    noecho.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    restored = true;
  }
  std::string line;
  int ch;
  while ((ch = getchar()) != EOF && ch != '\n') line.push_back((char)ch);
  if (restored) tcsetattr(STDIN_FILENO, TCSANOW, &old);
  printf("\n");
  return line;
}

std::string prompt_secret_twice(const std::string &what) {
  for (;;) {
    std::string a = prompt_secret(what);
    if (a.empty()) {
      printf("%s must not be empty\n", what.c_str());
      continue;
    }
    std::string b = prompt_secret("Repeat " + what);
    if (a == b) return a;
    printf("entries do not match, try again\n");
  }
}

bool prompt_yesno(const std::string &q, bool def) {
  for (;;) {
    std::string a = prompt(q + " (yes/no)", def ? "yes" : "no");
    bool b = false;
    if (setup::parse_bool(a, b)) return b;
    printf("please answer yes or no\n");
  }
}

struct Choice {
  std::string key;
  std::string desc;
};

std::string prompt_choice(const std::string &q, const std::vector<Choice> &choices,
                          const std::string &def) {
  printf("%s:\n", q.c_str());
  size_t defidx = 1;
  for (size_t i = 0; i < choices.size(); i++) {
    printf("  [%zu] %-12s %s\n", i + 1, choices[i].key.c_str(), choices[i].desc.c_str());
    if (choices[i].key == def) defidx = i + 1;
  }
  for (;;) {
    std::string a = prompt("Select", std::to_string(defidx));
    for (const auto &c : choices)
      if (a == c.key) return c.key;
    char *end = nullptr;
    long idx = strtol(a.c_str(), &end, 10);
    if (end && *end == 0 && idx >= 1 && idx <= (long)choices.size()) return choices[idx - 1].key;
    printf("invalid selection\n");
  }
}

std::string human_size(unsigned long long sectors) {
  double bytes = (double)sectors * 512.0;
  const char *units[] = {"B", "K", "M", "G", "T", "P"};
  int u = 0;
  while (bytes >= 1024.0 && u < 5) {
    bytes /= 1024.0;
    u++;
  }
  char b[64];
  snprintf(b, sizeof(b), "%.1f%s", bytes, units[u]);
  return b;
}

std::string read_first_line(const std::string &path) {
  salt_buf b;
  salt_buf_init(&b);
  std::string out;
  if (salt_read_file(path.c_str(), &b) == SALT_OK && b.data) {
    std::string s(b.data, b.len);
    out = trim(s.substr(0, s.find('\n')));
  }
  salt_buf_free(&b);
  return out;
}

std::string read_file(const std::string &path) {
  salt_buf b;
  salt_buf_init(&b);
  std::string out;
  if (salt_read_file(path.c_str(), &b) == SALT_OK && b.data) out.assign(b.data, b.len);
  salt_buf_free(&b);
  return out;
}

void write_file(const std::string &path, const std::string &content, unsigned mode) {
  std::string dir = path.substr(0, path.find_last_of('/'));
  salt_mkdirs(dir.c_str(), 0755);
  if (salt_write_file(path.c_str(), content.data(), content.size(), mode) != SALT_OK)
    fail("cannot write " + path);
}

void append_file(const std::string &path, const std::string &content) {
  std::string cur = read_file(path);
  if (!cur.empty() && cur.back() != '\n') cur += "\n";
  cur += content;
  write_file(path, cur, 0644);
}

void set_kv(const std::string &path, const std::string &key, const std::string &value) {
  std::vector<std::string> lines = split_lines(read_file(path));
  bool done = false;
  for (auto &l : lines) {
    std::string t = trim(l);
    if (t.rfind(key + "=", 0) == 0 || t.rfind("#" + key + "=", 0) == 0 ||
        t.rfind("# " + key + "=", 0) == 0) {
      if (done) {
        l = "";
        continue;
      }
      l = key + "=" + value;
      done = true;
    }
  }
  if (!done) lines.push_back(key + "=" + value);
  std::string out;
  for (const auto &l : lines) out += l + "\n";
  write_file(path, out, 0644);
}

std::string sh_quote(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') out.push_back('\\');
    out.push_back(c);
  }
  return out + "\"";
}

bool is_dir(const std::string &p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_blockdev(const std::string &p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
}

struct Disk {
  std::string node;
  std::string size;
  std::string model;
};

std::vector<Disk> list_disks() {
  std::vector<Disk> disks;
  DIR *dh = opendir("/sys/block");
  if (!dh) return disks;
  struct dirent *de;
  while ((de = readdir(dh)) != nullptr) {
    std::string name = de->d_name;
    if (name[0] == '.') continue;
    if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("sr", 0) == 0 ||
        name.rfind("zram", 0) == 0 || name.rfind("dm-", 0) == 0)
      continue;
    std::string base = "/sys/block/" + name;
    std::string sectors = read_first_line(base + "/size");
    if (sectors.empty() || sectors == "0") continue;
    Disk d;
    d.node = "/dev/" + name;
    d.size = human_size(strtoull(sectors.c_str(), nullptr, 10));
    d.model = read_first_line(base + "/device/model");
    disks.push_back(d);
  }
  closedir(dh);
  std::sort(disks.begin(), disks.end(), [](const Disk &a, const Disk &b) { return a.node < b.node; });
  return disks;
}

std::vector<std::string> list_strata(const std::string &root) {
  std::vector<std::string> names;
  std::string dir = root;
  if (!dir.empty() && dir.back() != '/') dir += "/";
  dir += "etc/salt/strata";
  DIR *dh = opendir(dir.c_str());
  if (!dh) return names;
  struct dirent *de;
  while ((de = readdir(dh)) != nullptr) {
    std::string n = de->d_name;
    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".toml") == 0)
      names.push_back(n.substr(0, n.size() - 5));
  }
  closedir(dh);
  std::sort(names.begin(), names.end());
  return names;
}

std::string blkid_value(const std::string &dev, const std::string &tag) {
  std::string out;
  run_capture({"blkid", "-s", tag, "-o", "value", dev}, out);
  return trim(out);
}

std::string blkid_uuid(const std::string &dev) { return blkid_value(dev, "UUID"); }

std::string partnode(const std::string &disk, int n) {
  char last = disk.empty() ? 0 : disk.back();
  if (last >= '0' && last <= '9') return disk + "p" + std::to_string(n);
  return disk + std::to_string(n);
}

void wait_for_node(const std::string &node) {
  for (int i = 0; i < 50; i++) {
    if (is_blockdev(node)) return;
    usleep(200000);
  }
  fail("partition node did not appear: " + node);
}

void settle(const std::string &disk) {
  run_quiet({"partprobe", disk});
  run_quiet({"udevadm", "settle"});
  usleep(500000);
}

struct Part {
  int num;
  unsigned long long start;
  unsigned long long end;
  std::string code;
  std::string name;
};

std::vector<Part> read_parts(const std::string &disk) {
  std::string out;
  run_capture({"sgdisk", "-p", disk}, out);
  std::vector<Part> parts;
  bool in_table = false;
  for (const auto &line : split_lines(out)) {
    if (line.rfind("Number", 0) == 0) {
      in_table = true;
      continue;
    }
    if (!in_table) continue;
    std::vector<std::string> f = split_ws(line);
    if (f.size() < 6) continue;
    char *end = nullptr;
    long num = strtol(f[0].c_str(), &end, 10);
    if (!end || *end) continue;
    Part p;
    p.num = (int)num;
    p.start = strtoull(f[1].c_str(), nullptr, 10);
    p.end = strtoull(f[2].c_str(), nullptr, 10);
    p.code = f[5];
    for (size_t i = 6; i < f.size(); i++) p.name += (i > 6 ? " " : "") + f[i];
    parts.push_back(p);
  }
  return parts;
}

std::string new_part(const std::string &disk, const std::string &size, const std::string &code,
                     const std::string &name) {
  std::set<int> before;
  for (const auto &p : read_parts(disk)) before.insert(p.num);
  std::string span = size.empty() ? "0:0" : "0:+" + size;
  must({"sgdisk", "-n0:" + span, "-t0:" + code, "-c0:" + name, disk}, "sgdisk create " + name);
  int created = 0;
  for (const auto &p : read_parts(disk))
    if (!before.count(p.num)) created = p.num;
  if (!created) fail("sgdisk did not report the new " + name + " partition");
  return partnode(disk, created);
}

unsigned long long largest_free_mib(const std::string &disk) {
  std::string f, e;
  run_capture({"sgdisk", "-F", disk}, f);
  run_capture({"sgdisk", "-E", disk}, e);
  unsigned long long first = strtoull(trim(f).c_str(), nullptr, 10);
  unsigned long long last = strtoull(trim(e).c_str(), nullptr, 10);
  if (last <= first) return 0;
  return (last - first + 1) * 512ULL / (1024ULL * 1024ULL);
}

std::string highest_kver(const std::string &mnt) {
  std::string dir = mnt + "/lib/modules";
  DIR *dh = opendir(dir.c_str());
  if (!dh) return "";
  std::vector<std::string> v;
  struct dirent *de;
  while ((de = readdir(dh)) != nullptr) {
    std::string n = de->d_name;
    if (n[0] != '.') v.push_back(n);
  }
  closedir(dh);
  std::sort(v.begin(), v.end());
  return v.empty() ? "" : v.back();
}

const char *pm_binary(const char *pm) {
  if (!pm) return "";
  std::string p = pm;
  if (p == "pacman") return "pacman";
  if (p == "apt") return "apt";
  if (p == "apk") return "apk";
  if (p == "dnf") return "dnf";
  if (p == "zypper") return "zypper";
  return "xbps-install";
}

void expose_userland(salt_strata_db *db, const std::string &mnt, const std::string &name,
                     const std::string &stratum_root, const char *pm) {
  const char *binary = pm_binary(pm);
  if (binary[0]) salt_expose_pm(db, mnt.c_str(), name.c_str(), binary);
  const char *dirs[] = {"usr/bin", "bin", "usr/local/bin", "usr/sbin", "sbin"};
  for (const char *d : dirs) {
    std::string p = stratum_root + "/" + d;
    DIR *dh = opendir(p.c_str());
    if (!dh) continue;
    struct dirent *de;
    while ((de = readdir(dh)) != nullptr) {
      if (de->d_name[0] == '.') continue;
      salt_expose_add(db, mnt.c_str(), name.c_str(), de->d_name, de->d_name, "cli");
    }
    closedir(dh);
  }
}

std::string sv_dir(const std::string &mnt) {
  if (is_dir(mnt + "/etc/runit/sv")) return "/etc/runit/sv";
  return "/etc/sv";
}

std::string runsvdir(const std::string &mnt) {
  if (is_dir(mnt + "/etc/runit/runsvdir/current")) return "/etc/runit/runsvdir/current";
  if (is_dir(mnt + "/etc/runit/runsvdir/default")) return "/etc/runit/runsvdir/default";
  return "/etc/runit/runsvdir/current";
}

void sv_enable(const std::string &mnt, const std::string &name) {
  std::string target = sv_dir(mnt) + "/" + name;
  if (!is_dir(mnt + target)) return;
  std::string dir = mnt + runsvdir(mnt);
  salt_mkdirs(dir.c_str(), 0755);
  run_quiet({"ln", "-sfn", target, dir + "/" + name});
}

void sv_disable(const std::string &mnt, const std::string &name) {
  run_quiet({"rm", "-f", mnt + runsvdir(mnt) + "/" + name});
}

void sv_write(const std::string &mnt, const std::string &name, const std::string &script) {
  write_file(mnt + sv_dir(mnt) + "/" + name + "/run", script, 0755);
}

void delive(const std::string &mnt, const setup::Config &cfg) {
  info("removing live-only configuration");
  std::string getty = mnt + sv_dir(mnt) + "/agetty-tty1/run";
  if (salt_path_exists(getty.c_str())) {
    std::string g = "#!/bin/sh\nexec 2>&1\nexec agetty --noclear tty1 38400 linux\n";
    if (cfg.autologin && cfg.create_user)
      g = "#!/bin/sh\nexec 2>&1\nexec setsid -w agetty --noclear --autologin " + cfg.username +
          " tty1 38400 linux\n";
    salt_write_file(getty.c_str(), g.data(), g.size(), 0755);
  }
  for (const char *f : {"home/salt/.bash_profile", "home/salt/.xsession-errors",
                        "home/salt/Desktop/Install-saltOS.desktop", "etc/sudoers.d/salt",
                        "etc/sudoers.d/calamares", "etc/xdg/autostart/saltos-installer.desktop",
                        "etc/xdg/autostart/saltos-setup.desktop", "usr/local/bin/saltos-installer",
                        "etc/salt/live-profile.toml", "etc/motd"})
    run_quiet({"rm", "-rf", mnt + "/" + f});
  std::vector<std::string> drop = {"agetty-serial", "stratum-e2e", "installer-check",
                                   "desktop-check", "saltos-autoinstall"};
  if (cfg.network != "dhcp" || is_dir(mnt + sv_dir(mnt) + "/NetworkManager"))
    drop.push_back("netdhcp");
  for (const auto &s : drop) {
    sv_disable(mnt, s);
    run_quiet({"rm", "-rf", mnt + sv_dir(mnt) + "/" + s});
  }
  if (cfg.username != "salt" || !cfg.create_user) chroot_run(mnt, {"userdel", "-r", "salt"});
  if (chroot_has(mnt, "dpkg"))
    run_quiet({"chroot", mnt, "dpkg", "--purge", "live-boot", "live-boot-initramfs-tools",
               "live-boot-doc"});
}

struct Layout {
  std::string disk;
  std::string bios_boot;
  std::string esp;
  bool esp_is_new = false;
  std::string boot;
  std::string root_part;
  std::string root_dev;
  std::string swap_part;
  std::string luks_uuid;
  std::string mopts;
  struct Mount {
    std::string dev;
    std::string where;
    std::string fstype;
    std::string opts;
  };
  std::vector<Mount> mounts;
};

std::string firmware_target(const std::string &arch, const setup::Config &cfg, bool booted_efi) {
  if (arch == "aarch64") return "uefi";
  if (cfg.firmware != "auto") return cfg.firmware;
  return booted_efi ? "both" : "bios";
}

std::string disk_mopts(const std::string &disk) {
  std::string base = disk.substr(disk.find_last_of('/') + 1);
  std::string rot = read_first_line("/sys/block/" + base + "/queue/rotational");
  if (rot == "0") return "rw,noatime,compress=zstd:1,ssd,space_cache=v2,discard=async";
  return "rw,noatime,compress=zstd:1,space_cache=v2";
}

std::string luks_key(const setup::Config &cfg) {
  std::string key = cfg.passphrase;
  if (key.empty() && !cfg.passphrase_file.empty()) {
    key = read_file(cfg.passphrase_file);
    if (!key.empty() && key.back() == '\n') key.pop_back();
  }
  if (key.empty()) fail("empty LUKS passphrase");
  return key;
}

void luks_format_open(const std::string &part, const std::string &name, const std::string &key,
                      Layout &lay) {
  if (run_stdin({"cryptsetup", "luksFormat", "--type", "luks2", "--batch-mode", "--key-file",
                 "-", part},
                key) != 0)
    fail("cryptsetup luksFormat " + part);
  if (run_stdin({"cryptsetup", "open", "--key-file", "-", part, name}, key) != 0)
    fail("cryptsetup open " + part);
  wait_for_node("/dev/mapper/" + name);
  lay.luks_uuid = blkid_uuid(part);
  lay.root_dev = "/dev/mapper/" + name;
}

void swap_size_mib(const setup::Config &cfg, unsigned long long &mib) {
  if (cfg.swap_size == "auto") {
    unsigned long long kib = 0;
    for (const auto &l : split_lines(read_file("/proc/meminfo")))
      if (l.rfind("MemTotal:", 0) == 0) kib = strtoull(split_ws(l)[1].c_str(), nullptr, 10);
    setup::parse_size_mib(setup::default_swap_size(kib), mib);
  } else {
    setup::parse_size_mib(cfg.swap_size, mib);
  }
}

void partition_disk(const setup::Config &cfg, const std::string &fw, const std::string &mnt,
                    Layout &lay) {
  lay.disk = cfg.disk;
  run_quiet({"umount", "-R", mnt});
  bool want_bios = fw == "bios" || fw == "both";
  unsigned long long need = 512 + (cfg.encrypt ? 1024 : 0) + 4096;
  unsigned long long swap_mib = 0;
  if (cfg.swap == "partition") {
    swap_size_mib(cfg, swap_mib);
    need += swap_mib;
  }

  if (cfg.mode == "erase") {
    info("erasing and partitioning " + cfg.disk);
    run_quiet({"wipefs", "-a", cfg.disk});
    must({"sgdisk", "--zap-all", cfg.disk}, "zap");
  } else {
    info("partitioning free space on " + cfg.disk + " (existing partitions are kept)");
    std::string pttype = blkid_value(cfg.disk, "PTTYPE");
    if (pttype != "gpt")
      fail("install alongside needs a GPT partition table on " + cfg.disk + " (found '" + pttype +
           "'); shrink and convert the disk first, or choose erase");
    unsigned long long free_mib = largest_free_mib(cfg.disk);
    if (free_mib < need)
      fail("largest free region on " + cfg.disk + " is " + std::to_string(free_mib) +
           " MiB; at least " + std::to_string(need) +
           " MiB of unpartitioned space is required (shrink another partition first)");
    for (const auto &p : read_parts(cfg.disk)) {
      if (p.code == "EF00" && lay.esp.empty()) lay.esp = partnode(cfg.disk, p.num);
      if (p.code == "EF02" && lay.bios_boot.empty()) lay.bios_boot = partnode(cfg.disk, p.num);
    }
  }

  if (want_bios && lay.bios_boot.empty())
    lay.bios_boot = new_part(cfg.disk, "1MiB", "ef02", "BIOS boot");
  if (lay.esp.empty()) {
    lay.esp = new_part(cfg.disk, "512MiB", "ef00", "EFI");
    lay.esp_is_new = true;
  }
  if (cfg.encrypt) lay.boot = new_part(cfg.disk, "1GiB", "8300", "saltOS boot");
  if (cfg.swap == "partition")
    lay.swap_part = new_part(cfg.disk, std::to_string(swap_mib) + "MiB", "8200", "saltOS swap");
  lay.root_part = new_part(cfg.disk, cfg.root_size.empty() ? "" : cfg.root_size, "8300", "saltOS");
  settle(cfg.disk);
  for (const std::string *n : {&lay.esp, &lay.root_part, &lay.boot, &lay.swap_part, &lay.bios_boot})
    if (!n->empty()) wait_for_node(*n);

  info("creating filesystems");
  if (lay.esp_is_new) must({"mkfs.fat", "-F32", "-n", "EFI", lay.esp}, "mkfs esp");
  if (!lay.boot.empty()) must({"mkfs.ext4", "-F", "-L", "saltOS-boot", lay.boot}, "mkfs boot");
  lay.root_dev = lay.root_part;
  if (cfg.encrypt) {
    info("encrypting root (LUKS2)");
    luks_format_open(lay.root_part, "saltos-root", luks_key(cfg), lay);
  }
  if (cfg.filesystem == "btrfs")
    must({"mkfs.btrfs", "-f", "-L", "saltOS", lay.root_dev}, "mkfs root");
  else
    must({"mkfs.ext4", "-F", "-L", "saltOS", lay.root_dev}, "mkfs root");
  if (!lay.swap_part.empty() && !cfg.encrypt)
    must({"mkswap", "-L", "saltOS-swap", lay.swap_part}, "mkswap");

  lay.mopts = disk_mopts(cfg.disk);
  salt_mkdirs(mnt.c_str(), 0755);
  if (cfg.filesystem == "btrfs") {
    info("creating btrfs subvolume layout");
    std::string top = mnt + "/.btrfs-top";
    must({"mkdir", "-p", top}, "mkdir top");
    must({"mount", "-o", "subvolid=5", lay.root_dev, top}, "mount top");
    const char *subs[] = {"@", "@home", "@var", "@log", "@snapshots", "@strata"};
    for (const char *s : subs) must({"btrfs", "subvolume", "create", top + "/" + s}, "subvol");
    must({"umount", top}, "umount top");
    run_quiet({"rmdir", top});

    must({"mount", "-o", lay.mopts + ",subvol=@", lay.root_dev, mnt}, "mount @");
    for (const char *d : {"home", "var", "var/log", ".snapshots", "strata", "boot/efi"})
      must({"mkdir", "-p", mnt + "/" + d}, "mkdir");
    must({"mount", "-o", lay.mopts + ",subvol=@home", lay.root_dev, mnt + "/home"}, "mount home");
    must({"mount", "-o", lay.mopts + ",subvol=@var", lay.root_dev, mnt + "/var"}, "mount var");
    must({"mkdir", "-p", mnt + "/var/log"}, "mkdir varlog");
    must({"mount", "-o", lay.mopts + ",subvol=@log", lay.root_dev, mnt + "/var/log"}, "mount log");
    must({"mount", "-o", lay.mopts + ",subvol=@snapshots", lay.root_dev, mnt + "/.snapshots"},
         "mount snaps");
    must({"mount", "-o", lay.mopts + ",subvol=@strata", lay.root_dev, mnt + "/strata"},
         "mount strata");
    std::string ruuid = blkid_uuid(lay.root_dev);
    for (const char *sub : {"@", "@home", "@var", "@log", "@snapshots", "@strata"}) {
      std::string where = "/";
      std::string s = sub;
      if (s == "@home") where = "/home";
      if (s == "@var") where = "/var";
      if (s == "@log") where = "/var/log";
      if (s == "@snapshots") where = "/.snapshots";
      if (s == "@strata") where = "/strata";
      lay.mounts.push_back({"UUID=" + ruuid, where, "btrfs", lay.mopts + ",subvol=" + s});
    }
  } else {
    must({"mount", "-o", "rw,noatime", lay.root_dev, mnt}, "mount root");
    for (const char *d : {"home", "var", "var/log", "strata", "boot/efi"})
      must({"mkdir", "-p", mnt + "/" + d}, "mkdir");
    lay.mounts.push_back({"UUID=" + blkid_uuid(lay.root_dev), "/", "ext4", "rw,noatime"});
  }
  if (!lay.boot.empty()) {
    must({"mkdir", "-p", mnt + "/boot"}, "mkdir boot");
    must({"mount", lay.boot, mnt + "/boot"}, "mount boot");
    must({"mkdir", "-p", mnt + "/boot/efi"}, "mkdir efi");
    lay.mounts.push_back({"UUID=" + blkid_uuid(lay.boot), "/boot", "ext4", "rw,noatime"});
  }
  must({"mount", lay.esp, mnt + "/boot/efi"}, "mount esp");
  lay.mounts.push_back({"UUID=" + blkid_uuid(lay.esp), "/boot/efi", "vfat", "umask=0077"});
}

std::string parent_disk(const std::string &dev) {
  std::string cur = dev;
  for (int i = 0; i < 6; i++) {
    std::string out;
    run_capture({"lsblk", "-no", "PKNAME", cur}, out);
    std::string p = trim(split_lines(out).empty() ? "" : split_lines(out)[0]);
    if (p.empty()) break;
    cur = "/dev/" + p;
  }
  std::string type;
  run_capture({"lsblk", "-dno", "TYPE", cur}, type);
  return trim(type) == "disk" ? cur : "";
}

std::string luks_backing(const std::string &mapper) {
  std::string name = mapper.substr(mapper.find_last_of('/') + 1);
  std::string out;
  run_capture({"cryptsetup", "status", name}, out);
  for (const auto &l : split_lines(out)) {
    std::string t = trim(l);
    if (t.rfind("device:", 0) == 0) return trim(t.substr(7));
  }
  return "";
}

void discover_mounted(const setup::Config &cfg, Layout &lay) {
  std::string mnt = cfg.target;
  while (mnt.size() > 1 && mnt.back() == '/') mnt.pop_back();
  static const std::set<std::string> real = {"btrfs", "ext4", "ext3", "ext2", "xfs", "vfat",
                                             "f2fs"};
  std::vector<Layout::Mount> found;
  for (const auto &line : split_lines(read_file("/proc/self/mounts"))) {
    std::vector<std::string> f = split_ws(line);
    if (f.size() < 4) continue;
    std::string where = f[1];
    for (size_t p; (p = where.find("\\040")) != std::string::npos;) where.replace(p, 4, " ");
    if (where != mnt && where.rfind(mnt + "/", 0) != 0) continue;
    if (!real.count(f[2])) continue;
    std::string rel = where == mnt ? "/" : where.substr(mnt.size());
    std::string opts;
    std::string optlist = f[3];
    std::replace(optlist.begin(), optlist.end(), ',', '\n');
    for (const auto &o : split_lines(optlist)) {
      if (o == "rw" || o == "relatime" || o.rfind("subvolid=", 0) == 0 || o == "seclabel")
        continue;
      if (!opts.empty()) opts += ",";
      opts += o;
    }
    if (opts.empty()) opts = "defaults";
    found.push_back({f[0], rel, f[2], opts});
  }
  std::string root;
  for (const auto &m : found)
    if (m.where == "/") root = m.dev;
  if (root.empty()) fail("nothing is mounted at " + mnt);
  lay.root_dev = root;
  lay.root_part = root;
  if (root.rfind("/dev/mapper/", 0) == 0) {
    std::string backing = luks_backing(root);
    if (backing.empty()) fail("cannot resolve the device behind " + root);
    lay.root_part = backing;
    lay.luks_uuid = blkid_uuid(backing);
  }
  lay.disk = parent_disk(lay.root_part);
  for (const auto &m : found) {
    if (m.where == "/boot/efi") lay.esp = m.dev;
    if (m.where == "/boot") lay.boot = m.dev;
    std::string dev = m.dev;
    if (dev.rfind("/dev/mapper/", 0) == 0) {
      lay.mounts.push_back({dev, m.where, m.fstype, m.opts});
      continue;
    }
    std::string uuid = blkid_uuid(dev);
    lay.mounts.push_back({uuid.empty() ? dev : "UUID=" + uuid, m.where, m.fstype, m.opts});
  }
  if (lay.esp.empty()) fail("no EFI system partition is mounted at " + mnt + "/boot/efi");
  std::sort(lay.mounts.begin(), lay.mounts.end(),
            [](const Layout::Mount &a, const Layout::Mount &b) { return a.where < b.where; });
  for (const auto &m : lay.mounts)
    if (m.where == "/" && m.fstype == "btrfs") lay.mopts = m.opts;
  if (lay.mopts.empty()) lay.mopts = "rw,noatime";
  if (lay.mopts.find("subvol=") != std::string::npos)
    lay.mopts = lay.mopts.substr(0, lay.mopts.find(",subvol="));
  for (const char *d : {"home", "var", "var/log", "strata", "boot/efi"})
    run_quiet({"mkdir", "-p", mnt + "/" + d});
}

void unmount_pseudo(const std::string &mnt) {
  for (const char *d : {"/sys/firmware/efi/efivars", "/dev/pts", "/dev", "/proc", "/sys", "/run"})
    run_quiet({"umount", "-l", mnt + d});
}

void lay_down_base(const std::string &mnt) {
  info("laying down native base");
  unmount_pseudo(mnt);
  std::string squashfs;
  for (const char *c : {"/run/live/medium/live/filesystem.squashfs",
                        "/lib/live/mount/medium/live/filesystem.squashfs"})
    if (salt_path_exists(c)) {
      squashfs = c;
      break;
    }
  if (!squashfs.empty()) {
    must({"unsquashfs", "-f", "-d", mnt, squashfs}, "unsquashfs");
  } else {
    must({"rsync", "-aHAXx", "--numeric-ids", "--exclude=/proc", "--exclude=/sys",
          "--exclude=/dev", "--exclude=/run", "--exclude=/tmp", "--exclude=/mnt",
          "--exclude=/media", "--exclude=/strata", "--exclude=/run/live", "--exclude=/lib/live",
          "--exclude=/boot/efi", "--exclude=" + mnt, "/", mnt + "/"},
         "rsync base");
  }
  for (const char *d : {"proc", "sys", "dev", "run", "tmp", "mnt", "media", "strata", "boot/efi"})
    run_quiet({"mkdir", "-p", mnt + "/" + d});
  run_quiet({"chmod", "1777", mnt + "/tmp"});
  run_quiet({"rm", "-f", mnt + "/etc/machine-id", mnt + "/var/lib/dbus/machine-id"});
  write_file(mnt + "/etc/machine-id", "", 0644);
}

void bind_pseudo(const std::string &mnt, bool efi) {
  info("binding pseudo-filesystems");
  auto mounted = [&](const std::string &p) { return run_quiet({"mountpoint", "-q", p}) == 0; };
  if (!mounted(mnt + "/dev")) must({"mount", "--bind", "/dev", mnt + "/dev"}, "bind dev");
  if (!mounted(mnt + "/dev/pts")) run_quiet({"mount", "--bind", "/dev/pts", mnt + "/dev/pts"});
  if (!mounted(mnt + "/proc")) must({"mount", "-t", "proc", "proc", mnt + "/proc"}, "mount proc");
  if (!mounted(mnt + "/sys")) must({"mount", "-t", "sysfs", "sys", mnt + "/sys"}, "mount sys");
  if (!mounted(mnt + "/run")) must({"mount", "-t", "tmpfs", "tmpfs", mnt + "/run"}, "mount run");
  if (efi && !mounted(mnt + "/sys/firmware/efi/efivars"))
    run_quiet({"mount", "-t", "efivarfs", "efivarfs", mnt + "/sys/firmware/efi/efivars"});
  std::string resolv = read_file("/etc/resolv.conf");
  if (!resolv.empty()) {
    run_quiet({"rm", "-f", mnt + "/etc/resolv.conf"});
    write_file(mnt + "/etc/resolv.conf", resolv, 0644);
  }
}

void write_fstab(const std::string &mnt, const setup::Config &cfg, const Layout &lay) {
  info("writing fstab");
  std::string fstab;
  for (const auto &m : lay.mounts) {
    char line[1024];
    snprintf(line, sizeof(line), "%-45s %-12s %-6s %s 0 %d\n", m.dev.c_str(), m.where.c_str(),
             m.fstype.c_str(), m.opts.c_str(), m.where == "/boot/efi" ? 1 : 0);
    fstab += line;
  }
  std::string crypttab;
  if (!lay.luks_uuid.empty())
    crypttab += "saltos-root UUID=" + lay.luks_uuid + " none luks,discard\n";
  if (!lay.swap_part.empty()) {
    if (cfg.encrypt) {
      fstab += "/dev/mapper/saltos-swap none swap defaults 0 0\n";
    } else {
      fstab += "UUID=" + blkid_uuid(lay.swap_part) + " none swap defaults 0 0\n";
    }
  }
  if (cfg.swap == "file") fstab += "/swapfile none swap defaults 0 0\n";
  fstab += "tmpfs /tmp tmpfs defaults,nosuid,nodev 0 0\n";
  write_file(mnt + "/etc/fstab", fstab, 0644);
  if (!crypttab.empty()) write_file(mnt + "/etc/crypttab", crypttab, 0644);
}

void setup_swap(const std::string &mnt, const setup::Config &cfg, const Layout &lay) {
  if (cfg.swap == "none") return;
  info("configuring swap (" + cfg.swap + ")");
  unsigned long long mib = 0;
  swap_size_mib(cfg, mib);
  if (cfg.swap == "file") {
    std::string path = mnt + "/swapfile";
    if (cfg.filesystem == "btrfs" || lay.mopts.find("compress") != std::string::npos) {
      if (run_quiet({"btrfs", "filesystem", "mkswapfile", "--size", std::to_string(mib) + "m",
                     path}) != 0) {
        must({"truncate", "-s", "0", path}, "swapfile create");
        run_quiet({"chattr", "+C", path});
        must({"fallocate", "-l", std::to_string(mib) + "M", path}, "swapfile allocate");
        must({"chmod", "0600", path}, "swapfile mode");
        must({"mkswap", path}, "mkswap file");
      }
    } else {
      must({"fallocate", "-l", std::to_string(mib) + "M", path}, "swapfile allocate");
      must({"chmod", "0600", path}, "swapfile mode");
      must({"mkswap", path}, "mkswap file");
    }
  } else if (cfg.swap == "zram") {
    std::string script =
        "#!/bin/sh\nexec 2>&1\nmodprobe zram\n"
        "dev=$(zramctl --find --size " + std::to_string(mib) + "M --algorithm zstd) || exit 1\n"
        "mkswap \"$dev\" >/dev/null && swapon -p 100 \"$dev\"\n"
        "trap 'swapoff \"$dev\"; zramctl --reset \"$dev\"; exit 0' TERM INT\n"
        "while :; do sleep 86400 & wait $!; done\n";
    sv_write(mnt, "zram-swap", script);
    sv_enable(mnt, "zram-swap");
  } else if (cfg.swap == "partition" && cfg.encrypt) {
    std::string puuid = blkid_value(lay.swap_part, "PARTUUID");
    std::string script =
        "#!/bin/sh\nexec 2>&1\n"
        "dev=/dev/disk/by-partuuid/" + puuid + "\n"
        "if [ ! -e /dev/mapper/saltos-swap ]; then\n"
        "  cryptsetup open --type plain --cipher aes-xts-plain64 --key-size 512 "
        "--key-file /dev/urandom \"$dev\" saltos-swap || exit 1\n"
        "  mkswap /dev/mapper/saltos-swap >/dev/null\n"
        "fi\n"
        "swapon /dev/mapper/saltos-swap\n"
        "trap 'swapoff /dev/mapper/saltos-swap; cryptsetup close saltos-swap; exit 0' TERM INT\n"
        "while :; do sleep 86400 & wait $!; done\n";
    sv_write(mnt, "crypt-swap", script);
    sv_enable(mnt, "crypt-swap");
  }
}

void create_user(const std::string &mnt, const setup::Config &cfg) {
  if (!cfg.create_user) return;
  info("creating user " + cfg.username);
  if (chroot_run(mnt, {"useradd", "-m", "-s", cfg.shell, cfg.username}) != 0)
    chroot_must(mnt, {"usermod", "-s", cfg.shell, cfg.username}, "user account " + cfg.username);
  if (!cfg.password_hash.empty()) {
    if (run_stdin({"chroot", mnt, "chpasswd", "-e"}, cfg.username + ":" + cfg.password_hash + "\n") != 0)
      fail("setting password hash");
  } else {
    if (run_stdin({"chroot", mnt, "chpasswd"}, cfg.username + ":" + cfg.password + "\n") != 0)
      fail("setting password");
  }
  for (const char *g : {"audio", "video", "input", "netdev", "network", "plugdev", "storage",
                        "users", "cdrom", "_seatd"})
    run_quiet({"chroot", mnt, "usermod", "-aG", g, cfg.username});
  if (cfg.sudo) {
    if (chroot_run(mnt, {"usermod", "-aG", "sudo", cfg.username}) != 0)
      chroot_run(mnt, {"usermod", "-aG", "wheel", cfg.username});
    write_file(mnt + "/etc/sudoers.d/10-" + cfg.username,
               cfg.username + " ALL=(ALL:ALL) ALL\n", 0440);
  }
}

std::string xkb_from_keymap(const std::string &keymap) {
  if (keymap == "uk") return "gb";
  if (keymap == "dvorak") return "us";
  std::string l = keymap.substr(0, keymap.find('-'));
  return l.empty() ? "us" : l;
}

void configure_locale(const std::string &mnt, const setup::Config &cfg) {
  info("configuring locale, timezone and keyboard");
  run_quiet({"rm", "-f", mnt + "/etc/localtime"});
  run({"ln", "-sf", "/usr/share/zoneinfo/" + cfg.timezone, mnt + "/etc/localtime"});
  write_file(mnt + "/etc/timezone", cfg.timezone + "\n", 0644);
  std::string charset = cfg.locale.find("UTF-8") != std::string::npos ? "UTF-8" : "ISO-8859-1";
  if (salt_path_exists((mnt + "/etc/locale.gen").c_str()) || chroot_has(mnt, "locale-gen")) {
    std::string lg = read_file(mnt + "/etc/locale.gen");
    if (lg.find("\n" + cfg.locale + " ") == std::string::npos &&
        lg.rfind(cfg.locale + " ", 0) != 0)
      append_file(mnt + "/etc/locale.gen", cfg.locale + " " + charset + "\n");
    chroot_run(mnt, {"locale-gen"});
  }
  if (salt_path_exists((mnt + "/etc/default/libc-locales").c_str())) {
    append_file(mnt + "/etc/default/libc-locales", cfg.locale + " " + charset + "\n");
    chroot_run(mnt, {"xbps-reconfigure", "-f", "glibc-locales"});
  }
  write_file(mnt + "/etc/locale.conf", "LANG=" + cfg.locale + "\n", 0644);
  write_file(mnt + "/etc/default/locale", "LANG=" + cfg.locale + "\n", 0644);

  std::string xkb = cfg.xkb_layout.empty() ? xkb_from_keymap(cfg.keymap) : cfg.xkb_layout;
  write_file(mnt + "/etc/vconsole.conf",
             "KEYMAP=" + cfg.keymap + "\nXKBLAYOUT=" + xkb + "\nXKBVARIANT=" + cfg.xkb_variant + "\n",
             0644);
  if (salt_path_exists((mnt + "/etc/default/keyboard").c_str())) {
    set_kv(mnt + "/etc/default/keyboard", "XKBLAYOUT", sh_quote(xkb));
    set_kv(mnt + "/etc/default/keyboard", "XKBVARIANT", sh_quote(cfg.xkb_variant));
  } else {
    write_file(mnt + "/etc/default/keyboard",
               "XKBMODEL=\"pc105\"\nXKBLAYOUT=" + sh_quote(xkb) + "\nXKBVARIANT=" +
                   sh_quote(cfg.xkb_variant) + "\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n",
               0644);
  }
  if (salt_path_exists((mnt + "/etc/rc.conf").c_str()))
    set_kv(mnt + "/etc/rc.conf", "KEYMAP", sh_quote(cfg.keymap));
  write_file(mnt + "/etc/X11/xorg.conf.d/00-keyboard.conf",
             "Section \"InputClass\"\n    Identifier \"system-keyboard\"\n"
             "    MatchIsKeyboard \"on\"\n    Option \"XkbLayout\" " + sh_quote(xkb) + "\n"
             "    Option \"XkbVariant\" " + sh_quote(cfg.xkb_variant) + "\nEndSection\n",
             0644);
  write_file(mnt + "/etc/hostname", cfg.hostname + "\n", 0644);
  write_file(mnt + "/etc/hosts",
             "127.0.0.1   localhost\n127.0.1.1   " + cfg.hostname +
                 "\n::1         localhost ip6-localhost ip6-loopback\n"
                 "ff02::1     ip6-allnodes\nff02::2     ip6-allrouters\n",
             0644);
}

bool has_cmd(const std::string &cmd) { return run_quiet({"sh", "-c", "command -v " + cmd}) == 0; }

std::string wifi_device() {
  DIR *dh = opendir("/sys/class/net");
  if (!dh) return "";
  struct dirent *de;
  std::string found;
  while ((de = readdir(dh)) != nullptr) {
    std::string n = de->d_name;
    if (n[0] == '.') continue;
    if (is_dir("/sys/class/net/" + n + "/wireless") ||
        salt_path_exists(("/sys/class/net/" + n + "/phy80211").c_str())) {
      found = n;
      break;
    }
  }
  closedir(dh);
  return found;
}

bool wired_carrier() {
  DIR *dh = opendir("/sys/class/net");
  if (!dh) return false;
  struct dirent *de;
  bool up = false;
  while ((de = readdir(dh)) != nullptr) {
    std::string n = de->d_name;
    if (n[0] == '.' || n == "lo") continue;
    if (is_dir("/sys/class/net/" + n + "/wireless")) continue;
    if (read_first_line("/sys/class/net/" + n + "/carrier") == "1") up = true;
  }
  closedir(dh);
  return up;
}

void connect_wifi_live(const setup::Config &cfg) {
  info("connecting to Wi-Fi network " + cfg.wifi_ssid);
  if (has_cmd("nmcli")) {
    std::vector<std::string> a = {"nmcli", "device", "wifi", "connect", cfg.wifi_ssid};
    if (!cfg.wifi_psk.empty()) {
      a.push_back("password");
      a.push_back(cfg.wifi_psk);
    }
    if (run(a) == 0) return;
    fail("nmcli could not connect to " + cfg.wifi_ssid);
  }
  if (has_cmd("salt-wifi")) {
    std::vector<std::string> a = {"salt-wifi", "connect", cfg.wifi_ssid};
    if (!cfg.wifi_psk.empty()) a.push_back(cfg.wifi_psk);
    if (run(a) == 0) return;
    fail("salt-wifi could not connect to " + cfg.wifi_ssid);
  }
  if (has_cmd("iwctl")) {
    std::string dev = wifi_device();
    if (dev.empty()) fail("no wireless interface found");
    std::vector<std::string> a = {"iwctl"};
    if (!cfg.wifi_psk.empty()) {
      a.push_back("--passphrase");
      a.push_back(cfg.wifi_psk);
    }
    a.insert(a.end(), {"station", dev, "connect", cfg.wifi_ssid});
    if (run(a) == 0) return;
    fail("iwctl could not connect to " + cfg.wifi_ssid);
  }
  fail("no Wi-Fi tool (nmcli, salt-wifi or iwctl) is available on this medium");
}

void configure_network(const std::string &mnt, const setup::Config &cfg) {
  info("configuring network (" + cfg.network + ")");
  bool nm = is_dir(mnt + "/etc/NetworkManager") &&
            (salt_path_exists((mnt + "/usr/sbin/NetworkManager").c_str()) ||
             salt_path_exists((mnt + "/usr/bin/NetworkManager").c_str()));
  if (cfg.network == "none") {
    sv_disable(mnt, "NetworkManager");
    sv_disable(mnt, "netdhcp");
    sv_disable(mnt, "dhcpcd");
    return;
  }
  if (nm) {
    if (!is_dir(mnt + sv_dir(mnt) + "/NetworkManager"))
      sv_write(mnt, "NetworkManager",
               "#!/bin/sh\nexec 2>&1\n[ -d /var/lib/NetworkManager ] || mkdir -p "
               "/var/lib/NetworkManager\nexec NetworkManager --no-daemon\n");
    sv_enable(mnt, "dbus");
    sv_enable(mnt, "NetworkManager");
    sv_disable(mnt, "netdhcp");
    sv_disable(mnt, "dhcpcd");
  } else if (is_dir(mnt + sv_dir(mnt) + "/dhcpcd")) {
    sv_enable(mnt, "dhcpcd");
  } else if (has_cmd("dhclient") || salt_path_exists((mnt + "/sbin/dhclient").c_str())) {
    if (!is_dir(mnt + sv_dir(mnt) + "/netdhcp"))
      sv_write(mnt, "netdhcp",
               "#!/bin/sh\nexec 2>&1\n"
               "iface=$(ip -o link show 2>/dev/null | awk -F': ' '$2 != \"lo\" {print $2; exit}')\n"
               "[ -n \"$iface\" ] || { sleep 5; exec sleep 30; }\n"
               "ip link set \"$iface\" up 2>/dev/null\nexec dhclient -d \"$iface\"\n");
    sv_enable(mnt, "netdhcp");
  }
  if (cfg.network != "wifi") return;
  if (nm && is_dir("/etc/NetworkManager/system-connections")) {
    run_quiet({"mkdir", "-p", mnt + "/etc/NetworkManager/system-connections"});
    run({"sh", "-c", "cp -a /etc/NetworkManager/system-connections/. " +
                         sh_quote(mnt + "/etc/NetworkManager/system-connections/")});
    run_quiet({"chmod", "600", "-R", mnt + "/etc/NetworkManager/system-connections"});
  }
  if (salt_path_exists("/etc/wpa_supplicant.conf")) {
    write_file(mnt + "/etc/wpa_supplicant.conf", read_file("/etc/wpa_supplicant.conf"), 0600);
    std::string dev = wifi_device();
    if (!dev.empty() && !nm) {
      sv_write(mnt, "wpa_supplicant",
               "#!/bin/sh\nexec 2>&1\nexec wpa_supplicant -i " + dev +
                   " -c /etc/wpa_supplicant.conf\n");
      sv_enable(mnt, "wpa_supplicant");
    }
  }
  if (is_dir("/var/lib/iwd")) {
    run_quiet({"mkdir", "-p", mnt + "/var/lib/iwd"});
    run({"sh", "-c", "cp -a /var/lib/iwd/. " + sh_quote(mnt + "/var/lib/iwd/")});
    if (!nm) sv_enable(mnt, "iwd");
  }
}

bool live_has_desktop() {
  return salt_path_exists("/usr/bin/lxqt-session") || salt_path_exists("/usr/bin/startx") ||
         salt_path_exists("/usr/bin/sddm");
}

void configure_desktop(const std::string &mnt, const setup::Config &cfg) {
  bool keep = cfg.desktop == "keep";
  bool sddm = salt_path_exists((mnt + "/usr/bin/sddm").c_str());
  std::string home = mnt + "/home/" + cfg.username;
  if (!keep) {
    sv_disable(mnt, "sddm");
    sv_disable(mnt, "lightdm");
    sv_disable(mnt, "gdm");
    return;
  }
  info("enabling the graphical session");
  if (sddm) {
    if (!is_dir(mnt + sv_dir(mnt) + "/sddm"))
      sv_write(mnt, "sddm", "#!/bin/sh\nexec 2>&1\n[ -x /usr/bin/dbus-daemon ] && sv start dbus "
                            ">/dev/null 2>&1\nexec sddm\n");
    sv_enable(mnt, "dbus");
    sv_enable(mnt, "elogind");
    sv_enable(mnt, "sddm");
    write_file(mnt + "/etc/sddm.conf.d/saltos.conf", "[X11]\nMinimumVT=7\n", 0644);
    if (cfg.autologin && cfg.create_user)
      write_file(mnt + "/etc/sddm.conf.d/autologin.conf",
                 "[Autologin]\nUser=" + cfg.username + "\nSession=lxqt\n", 0644);
    else
      run_quiet({"rm", "-f", mnt + "/etc/sddm.conf.d/autologin.conf"});
    return;
  }
  if (!cfg.create_user || !is_dir(home)) return;
  write_file(home + "/.bash_profile",
             "if [ -z \"${DISPLAY:-}\" ] && [ \"$(tty)\" = /dev/tty1 ]; then\n"
             "  exec startx > \"$HOME/.xsession-errors\" 2>&1\nfi\n",
             0644);
  if (salt_path_exists("/home/salt/.xinitrc"))
    write_file(home + "/.xinitrc", read_file("/home/salt/.xinitrc"), 0755);
  if (is_dir("/home/salt/.config/lxqt") && !is_dir(home + "/.config/lxqt")) {
    run_quiet({"mkdir", "-p", home + "/.config"});
    run({"cp", "-a", "/home/salt/.config/lxqt", home + "/.config/lxqt"});
  }
  chroot_run(mnt, {"chown", "-R", cfg.username + ":" + cfg.username, "/home/" + cfg.username});
}

void bootstrap_stratum(const std::string &mnt, const setup::Config &cfg) {
  info("bootstrapping " + cfg.distro + " stratum into the target");
  std::string recipe = mnt + "/etc/salt/strata/" + cfg.distro + ".toml";
  if (!salt_path_exists(recipe.c_str())) fail("stratum recipe not found: " + recipe);
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(mnt.c_str(), &db) != SALT_OK) fail(salt_last_error());
  salt_strata_ctx ctx;
  salt_strata_ctx_init(&ctx, mnt.c_str());
  salt_stratum_recipe r;
  salt_stratum_recipe_init(&r);
  if (salt_stratum_recipe_load(recipe.c_str(), &r) != SALT_OK) fail(salt_last_error());
  if (salt_stratum_bootstrap(&ctx, db, &r) != SALT_OK) fail(salt_last_error());
  info("exposing " + cfg.distro + " userland");
  {
    salt_stratum s;
    memset(&s, 0, sizeof(s));
    std::string srf = mnt + "/strata/" + cfg.distro;
    if (salt_stratum_get(db, cfg.distro.c_str(), &s) == SALT_OK && s.root)
      srf = std::string(mnt) + s.root;
    expose_userland(db, mnt, cfg.distro, srf, r.package_manager);
    salt_stratum_free_fields(&s);
  }
  salt_stratum_recipe_free(&r);
  salt_strata_ctx_free(&ctx);
  salt_strata_db_close(db);
}

void install_boot(const std::string &mnt, const setup::Config &cfg, const Layout &lay,
                  const std::string &arch, const std::string &fw, bool booted_efi) {
  info("installing kernel and bootloader (" + fw + ")");
  std::string kver = highest_kver(mnt);
  if (kver.empty()) fail("no kernel modules in target");
  std::string kimg;
  for (const std::string &c : std::vector<std::string>{"/boot/vmlinuz-" + kver, "/boot/vmlinux-" + kver,
                                                       "/boot/Image-" + kver, "/boot/vmlinuz",
                                                       "/boot/Image"})
    if (salt_path_exists((mnt + c).c_str())) {
      kimg = c;
      break;
    }
  if (kimg.empty()) {
    const char *live = "/run/live/medium/live/vmlinuz";
    if (!salt_path_exists(live)) fail("no kernel image for " + kver + " in the target's /boot");
    kimg = "/boot/vmlinuz-" + kver;
    must({"cp", live, mnt + kimg}, "copy kernel from live medium");
  }
  if (chroot_has(mnt, "update-initramfs")) {
    run_quiet({"rm", "-f", mnt + "/boot/initrd.img-" + kver});
    chroot_must(mnt, {"update-initramfs", "-c", "-k", kver}, "initramfs");
  } else if (chroot_has(mnt, "dracut")) {
    std::vector<std::string> d = {"dracut", "--force", "--no-hostonly"};
    if (!lay.luks_uuid.empty()) d.insert(d.end(), {"--add", "crypt"});
    d.insert(d.end(), {"/boot/initramfs-" + kver + ".img", kver});
    chroot_must(mnt, d, "dracut");
  } else {
    fail("no initramfs generator in target");
  }

  std::string cmdline = cfg.cmdline;
  if (!lay.luks_uuid.empty()) {
    if (!cmdline.empty()) cmdline += " ";
    cmdline += "rd.luks.uuid=" + lay.luks_uuid + " rd.luks.name=" + lay.luks_uuid + "=saltos-root";
  }
  if (cfg.filesystem == "btrfs" || lay.mopts.find("subvol") != std::string::npos) {
    std::string rootflags = "rootflags=subvol=@";
    for (const auto &m : lay.mounts)
      if (m.where == "/" && m.fstype == "btrfs" && m.opts.find("subvol=") != std::string::npos)
        rootflags = "rootflags=" + m.opts.substr(m.opts.find("subvol="));
    if (!cmdline.empty()) cmdline += " ";
    cmdline += rootflags;
  }
  std::string grub_def = mnt + "/etc/default/grub";
  if (!salt_path_exists(grub_def.c_str()))
    write_file(grub_def, "GRUB_DEFAULT=0\nGRUB_TIMEOUT=3\nGRUB_DISTRIBUTOR=\"saltOS\"\n", 0644);
  set_kv(grub_def, "GRUB_DISTRIBUTOR", "\"saltOS\"");
  set_kv(grub_def, "GRUB_TIMEOUT", "3");
  set_kv(grub_def, "GRUB_TIMEOUT_STYLE", "menu");
  set_kv(grub_def, "GRUB_CMDLINE_LINUX_DEFAULT", "\"quiet\"");
  set_kv(grub_def, "GRUB_CMDLINE_LINUX", sh_quote(cmdline));
  bool prober = cfg.os_prober && chroot_has(mnt, "os-prober");
  set_kv(grub_def, "GRUB_DISABLE_OS_PROBER", prober ? "false" : "true");
  if (cmdline.find("console=ttyS") != std::string::npos ||
      cmdline.find("console=ttyAMA") != std::string::npos) {
    set_kv(grub_def, "GRUB_TERMINAL", "\"console serial\"");
    set_kv(grub_def, "GRUB_SERIAL_COMMAND", "\"serial --speed=115200\"");
  }
  if (!lay.luks_uuid.empty()) set_kv(grub_def, "GRUB_ENABLE_CRYPTODISK", "n");

  bool signed_grub = salt_path_exists((mnt + "/usr/lib/grub/x86_64-efi-signed").c_str()) ||
                     salt_path_exists((mnt + "/usr/lib/grub/arm64-efi-signed").c_str());
  bool shim = salt_path_exists((mnt + "/usr/lib/shim/shimx64.efi.signed").c_str()) ||
              salt_path_exists((mnt + "/usr/lib/shim/shimaa64.efi.signed").c_str());
  if (cfg.shim == "yes" && !(signed_grub && shim))
    fail("boot.shim = \"yes\" but shim-signed / signed GRUB are not present in the target");
  std::vector<std::string> efi_flags;
  if (signed_grub) efi_flags.push_back(cfg.shim == "no" || !shim ? "--no-uefi-secure-boot"
                                                                 : "--uefi-secure-boot");
  std::string efi_target = arch == "aarch64" ? "arm64-efi" : "x86_64-efi";
  if (fw == "bios" || fw == "both") {
    if (lay.disk.empty()) fail("cannot determine the disk holding the root filesystem");
    chroot_must(mnt, {"grub-install", "--target=i386-pc", "--recheck", lay.disk},
                "grub-install (BIOS)");
  }
  if (fw == "uefi" || fw == "both") {
    std::vector<std::string> g = {"grub-install", "--target=" + efi_target,
                                  "--efi-directory=/boot/efi", "--bootloader-id=saltOS",
                                  "--recheck", "--removable", "--no-nvram"};
    g.insert(g.end(), efi_flags.begin(), efi_flags.end());
    chroot_must(mnt, g, "grub-install (UEFI removable path)");
    if (booted_efi) {
      std::vector<std::string> n = {"grub-install", "--target=" + efi_target,
                                    "--efi-directory=/boot/efi", "--bootloader-id=saltOS",
                                    "--recheck"};
      n.insert(n.end(), efi_flags.begin(), efi_flags.end());
      if (chroot_run(mnt, n) != 0)
        info("firmware boot entry not registered (NVRAM unavailable); the removable path "
             "/EFI/BOOT will be used");
    }
  }
  chroot_must(mnt, {"grub-mkconfig", "-o", "/boot/grub/grub.cfg"}, "grub-mkconfig");
}

void write_system_config(const std::string &mnt, const setup::Config &cfg) {
  info("writing reproducible system config");
  std::string sys_dir = mnt + "/etc/salt";
  salt_mkdirs(sys_dir.c_str(), 0755);
  write_file(sys_dir + "/system.toml", setup::to_toml(cfg, false), 0644);
  if (chroot_run(mnt, {"salt", "lock", "--output", "/etc/salt/system.lock.toml"}) != 0)
    info("lockfile capture incomplete; system.toml written, run 'salt lock' later");
  chroot_run(mnt, {"salt", "deployments", "--register-current"});
}

void ask_interactive(setup::Config &cfg, const std::string &arch, bool booted_efi) {
  std::vector<Disk> disks = list_disks();
  if (disks.empty()) fail("no disks found");
  printf("Available disks:\n");
  std::string def = "1";
  for (size_t i = 0; i < disks.size(); i++) {
    printf("  [%zu] %s  %s  %s\n", i + 1, disks[i].node.c_str(), disks[i].size.c_str(),
           disks[i].model.c_str());
    if (disks[i].node == cfg.disk) def = std::to_string(i + 1);
  }
  for (;;) {
    std::string pick = prompt("Select disk number", def);
    long idx = strtol(pick.c_str(), nullptr, 10);
    if (idx >= 1 && idx <= (long)disks.size()) {
      cfg.disk = disks[idx - 1].node;
      break;
    }
    if (is_blockdev(pick)) {
      cfg.disk = pick;
      break;
    }
    printf("invalid disk selection\n");
  }

  std::string free_note;
  if (blkid_value(cfg.disk, "PTTYPE") == "gpt")
    free_note = " (" + std::to_string(largest_free_mib(cfg.disk)) + " MiB unpartitioned)";
  cfg.mode = prompt_choice("Installation mode",
                           {{"erase", "erase the whole disk"},
                            {"alongside", "keep existing partitions, use free space" + free_note}},
                           cfg.mode == "mounted" ? "erase" : cfg.mode);
  if (cfg.mode == "alongside") {
    std::string rs = prompt("Size of the new saltOS root (e.g. 40G, empty = all free space)",
                            cfg.root_size);
    cfg.root_size = rs;
  } else {
    cfg.root_size.clear();
  }

  if (arch == "aarch64") {
    cfg.firmware = "uefi";
  } else {
    cfg.firmware = prompt_choice(
        std::string("Firmware / boot mode (this machine booted via ") +
            (booted_efi ? "UEFI" : "legacy BIOS") + ")",
        {{"auto", booted_efi ? "UEFI + BIOS fallback" : "legacy BIOS only"},
         {"uefi", "UEFI only"},
         {"bios", "legacy BIOS only"},
         {"both", "UEFI and legacy BIOS"}},
        cfg.firmware);
  }
  cfg.filesystem = prompt_choice("Root filesystem",
                                 {{"btrfs", "snapshots and rollback (recommended)"},
                                  {"ext4", "plain journaling filesystem, no snapshots"}},
                                 cfg.filesystem);
  cfg.encrypt = prompt_yesno("Encrypt the root filesystem with LUKS2", cfg.encrypt);
  if (cfg.encrypt && cfg.passphrase.empty() && cfg.passphrase_file.empty())
    cfg.passphrase = prompt_secret_twice("disk passphrase");
  cfg.swap = prompt_choice("Swap",
                           {{"none", "no swap"},
                            {"file", "swap file on the root filesystem"},
                            {"partition", "dedicated swap partition"},
                            {"zram", "compressed swap in RAM"}},
                           cfg.swap);
  if (cfg.swap != "none")
    cfg.swap_size = prompt("Swap size (auto = RAM size, capped at 8G)", cfg.swap_size);

  std::vector<std::string> strata = list_strata("/");
  if (strata.empty()) fail("no stratum definitions in /etc/salt/strata");
  std::vector<Choice> sc;
  for (const auto &s : strata) sc.push_back({s, "primary stratum"});
  cfg.distro = prompt_choice("Base distribution (primary stratum)", sc,
                             cfg.distro.empty() ? strata[0] : cfg.distro);

  cfg.hostname = prompt("Hostname", cfg.hostname);
  cfg.username = prompt("Username", cfg.username);
  if (cfg.password.empty() && cfg.password_hash.empty())
    cfg.password = prompt_secret_twice("password for " + cfg.username);
  cfg.sudo = prompt_yesno("Grant " + cfg.username + " administrative (sudo) rights", cfg.sudo);
  cfg.autologin = prompt_yesno("Log " + cfg.username + " in automatically", cfg.autologin);
  cfg.timezone = prompt("Timezone", cfg.timezone);
  cfg.locale = prompt("Locale", cfg.locale);
  cfg.keymap = prompt("Console keymap", cfg.keymap);
  if (has_cmd("loadkeys")) run_quiet({"loadkeys", cfg.keymap});

  std::string wdev = wifi_device();
  std::vector<Choice> nc = {{"dhcp", std::string("wired, automatic (DHCP)") +
                                         (wired_carrier() ? " -- link detected" : "")}};
  if (!wdev.empty()) nc.push_back({"wifi", "Wi-Fi via " + wdev});
  nc.push_back({"none", "no network configuration"});
  cfg.network = prompt_choice("Network", nc, cfg.network);
  if (cfg.network == "wifi") {
    cfg.wifi_ssid = prompt("Wi-Fi network name (SSID)", cfg.wifi_ssid);
    if (cfg.wifi_psk.empty()) cfg.wifi_psk = prompt_secret("Wi-Fi passphrase (empty for open)");
    connect_wifi_live(cfg);
  }

  if (live_has_desktop())
    cfg.desktop = prompt_choice("Graphical desktop on the installed system",
                                {{"keep", "keep the live desktop (LXQt)"},
                                 {"none", "console only"}},
                                cfg.desktop == "auto" ? "keep" : cfg.desktop);
  else
    cfg.desktop = "none";

  if (cfg.mode == "alongside")
    cfg.os_prober = prompt_yesno("Add other installed operating systems to the boot menu",
                                 cfg.os_prober);
  cfg.cmdline = prompt("Extra kernel command line (empty for none)", cfg.cmdline);
  cfg.kernel = prompt("Kernel source", cfg.kernel);
}

void print_summary(const setup::Config &cfg, const std::string &fw) {
  printf("\n");
  if (cfg.mode == "erase")
    printf("About to ERASE %s and install saltOS with the %s stratum.\n", cfg.disk.c_str(),
           cfg.distro.c_str());
  else if (cfg.mode == "alongside")
    printf("About to install saltOS into the free space of %s (existing partitions are kept) "
           "with the %s stratum.\n",
           cfg.disk.c_str(), cfg.distro.c_str());
  else
    printf("About to install saltOS into the prepared target %s with the %s stratum.\n",
           cfg.target.c_str(), cfg.distro.c_str());
  printf("firmware=%s filesystem=%s encrypt=%s swap=%s(%s)\n", fw.c_str(), cfg.filesystem.c_str(),
         cfg.encrypt ? "yes" : "no", cfg.swap.c_str(), cfg.swap_size.c_str());
  printf("hostname=%s user=%s sudo=%s autologin=%s\n", cfg.hostname.c_str(), cfg.username.c_str(),
         cfg.sudo ? "yes" : "no", cfg.autologin ? "yes" : "no");
  printf("tz=%s locale=%s keymap=%s network=%s desktop=%s\n", cfg.timezone.c_str(),
         cfg.locale.c_str(), cfg.keymap.c_str(), cfg.network.c_str(), cfg.desktop.c_str());
  printf("kernel=%s os-prober=%s cmdline=\"%s\"\n", cfg.kernel.c_str(),
         cfg.os_prober ? "yes" : "no", cfg.cmdline.c_str());
}

}

int main(int argc, char **argv) {
  setup::CliOptions opts;
  std::string err;
  int rc = setup::parse_cli(argc, argv, opts, err);
  if (rc != 0) {
    fprintf(stderr, "%s: %s\n", kProg, err.c_str());
    fputs(setup::usage_text().c_str(), stderr);
    return rc;
  }
  if (opts.help) {
    fputs(setup::usage_text().c_str(), stdout);
    return 0;
  }

  setup::Config cfg;
  for (const auto &p : opts.profiles)
    if (!setup::load_toml_file(p, cfg, err)) fail(err);
  if (!opts.from.empty() && !setup::load_toml_file(opts.from, cfg, err)) fail(err);
  for (const auto &kv : opts.sets) {
    size_t eq = kv.find('=');
    if (!setup::set_value(cfg, kv.substr(0, eq), kv.substr(eq + 1), err)) fail(err);
  }
  bool interactive = opts.from.empty();

  if (opts.dump_config) {
    if (!setup::validate(cfg, true, err)) fprintf(stderr, "%s: warning: %s\n", kProg, err.c_str());
    fputs(setup::to_toml(cfg, false).c_str(), stdout);
    return 0;
  }

  if (geteuid() != 0) fail("must run as root");

  struct utsname un{};
  uname(&un);
  std::string arch = un.machine;
  bool booted_efi = salt_path_exists("/sys/firmware/efi");

  if (interactive) {
    if (cfg.mode == "mounted") fail("--target is only supported together with --from");
    ask_interactive(cfg, arch, booted_efi);
  }
  if (cfg.desktop == "auto") cfg.desktop = live_has_desktop() ? "keep" : "none";
  if (!setup::validate(cfg, false, err)) fail(err);

  std::string fw = firmware_target(arch, cfg, booted_efi);
  std::string mnt = cfg.mode == "mounted" ? cfg.target : opts.mnt;
  while (mnt.size() > 1 && mnt.back() == '/') mnt.pop_back();

  if (cfg.mode != "mounted" && !is_blockdev(cfg.disk)) fail("not a block device: " + cfg.disk);
  if (cfg.mode == "mounted" && !is_dir(mnt)) fail("target is not a directory: " + mnt);

  if (interactive && !opts.yes) {
    print_summary(cfg, fw);
    std::string yes = prompt("Type 'yes' to proceed", "");
    if (yes != "yes") fail("aborted");
  } else {
    print_summary(cfg, fw);
  }

  if (!interactive && cfg.network == "wifi") {
    std::string out;
    if (has_cmd("nmcli")) run_capture({"nmcli", "-t", "-f", "STATE", "general"}, out);
    if (trim(out).rfind("connected", 0) != 0) connect_wifi_live(cfg);
  }

  Layout lay;
  if (cfg.mode == "mounted") {
    discover_mounted(cfg, lay);
    info("installing into prepared target " + mnt + " (root on " + lay.root_dev + ")");
  } else {
    partition_disk(cfg, fw, mnt, lay);
  }

  lay_down_base(mnt);
  write_fstab(mnt, cfg, lay);
  bind_pseudo(mnt, booted_efi);
  create_user(mnt, cfg);
  configure_locale(mnt, cfg);
  configure_network(mnt, cfg);
  setup_swap(mnt, cfg, lay);
  bootstrap_stratum(mnt, cfg);
  install_boot(mnt, cfg, lay, arch, fw, booted_efi);
  write_system_config(mnt, cfg);
  delive(mnt, cfg);
  configure_desktop(mnt, cfg);

  info("syncing");
  run({"sync"});
  if (cfg.mode != "mounted") {
    run_quiet({"umount", "-R", mnt});
    if (!lay.luks_uuid.empty()) run_quiet({"cryptsetup", "close", "saltos-root"});
    info("saltOS installed to " + cfg.disk + " with the " + cfg.distro + " stratum");
  } else {
    unmount_pseudo(mnt);
    info("saltOS installed into " + mnt + " with the " + cfg.distro + " stratum");
  }
  return 0;
}

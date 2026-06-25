extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
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

void must(const std::vector<std::string> &args, const std::string &what) {
  if (run(args) != 0) fail("failed: " + what);
}

int chroot_run(const std::string &mnt, const std::vector<std::string> &args) {
  std::vector<std::string> full = {"chroot", mnt};
  full.insert(full.end(), args.begin(), args.end());
  return run(full);
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

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string prompt(const std::string &q, const std::string &def) {
  printf("%s", q.c_str());
  if (!def.empty()) printf(" [%s]", def.c_str());
  printf(": ");
  fflush(stdout);
  std::string line;
  int ch;
  while ((ch = getchar()) != EOF && ch != '\n') line.push_back((char)ch);
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
    if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("sr", 0) == 0)
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

std::string blkid_uuid(const std::string &dev) {
  std::string out;
  run_capture({"blkid", "-s", "UUID", "-o", "value", dev}, out);
  return trim(out);
}

std::string partnode(const std::string &disk, int n) {
  char last = disk.empty() ? 0 : disk.back();
  if (last >= '0' && last <= '9') return disk + "p" + std::to_string(n);
  return disk + std::to_string(n);
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

void delive(const std::string &mnt, const std::string &username) {
  info("removing live-only configuration");
  std::string getty = mnt + "/etc/runit/sv/agetty-tty1/run";
  if (salt_path_exists(getty.c_str())) {
    std::string g = "#!/bin/sh\nexec 2>&1\nexec agetty --noclear tty1 38400 linux\n";
    salt_write_file(getty.c_str(), g.data(), g.size(), 0755);
  }
  for (const char *f : {"home/salt/.bash_profile", "home/salt/.xsession-errors",
                        "etc/sudoers.d/salt", "etc/sudoers.d/calamares"})
    run({"rm", "-rf", mnt + "/" + f});
  for (const char *s : {"boot-check", "agetty-serial", "stratum-e2e", "netdhcp",
                        "installer-check", "desktop-check"}) {
    run({"rm", "-f", mnt + "/etc/runit/runsvdir/current/" + s});
    run({"rm", "-rf", mnt + "/etc/runit/sv/" + s});
  }
  if (username != "salt") chroot_run(mnt, {"userdel", "-r", "salt"});
}

struct Config {
  std::string disk;
  std::string distro;
  std::string hostname = "saltos";
  std::string username = "salt";
  std::string password;
  std::string timezone = "UTC";
  std::string locale = "en_US.UTF-8";
  std::string kernel = "native";
};

std::string toml_value(const std::string &content, const std::string &key) {
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    std::string line = trim(content.substr(pos, eol == std::string::npos ? eol : eol - pos));
    pos = eol == std::string::npos ? content.size() : eol + 1;
    if (line.rfind(key, 0) == 0) {
      std::string rest = trim(line.substr(key.size()));
      if (!rest.empty() && rest[0] == '=') {
        std::string v = trim(rest.substr(1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
      }
    }
  }
  return "";
}

void load_config_file(const std::string &path, Config &cfg) {
  salt_buf b;
  salt_buf_init(&b);
  if (salt_read_file(path.c_str(), &b) != SALT_OK) {
    salt_buf_free(&b);
    fail("cannot read config: " + path);
  }
  std::string content(b.data ? b.data : "", b.len);
  salt_buf_free(&b);
  std::string v;
  if (!(v = toml_value(content, "hostname")).empty()) cfg.hostname = v;
  if (!(v = toml_value(content, "locale")).empty()) cfg.locale = v;
  if (!(v = toml_value(content, "timezone")).empty()) cfg.timezone = v;
  if (!(v = toml_value(content, "source")).empty()) cfg.kernel = v;
  if (!(v = toml_value(content, "name")).empty()) cfg.distro = v;
}

void usage() {
  fprintf(stderr,
          "usage: %s [--from <system.toml>] [--disk <dev>] [--user <name>] [--mnt <dir>]\n",
          kProg);
}

}

int main(int argc, char **argv) {
  std::string mnt = "/mnt/saltos-target";
  std::string from;
  std::string disk_arg;
  std::string user_arg;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--from" && i + 1 < argc) {
      from = argv[++i];
    } else if (a == "--disk" && i + 1 < argc) {
      disk_arg = argv[++i];
    } else if (a == "--user" && i + 1 < argc) {
      user_arg = argv[++i];
    } else if (a == "--mnt" && i + 1 < argc) {
      mnt = argv[++i];
    } else {
      usage();
      return 2;
    }
  }

  if (geteuid() != 0) fail("must run as root");

  Config cfg;
  bool interactive = from.empty();

  if (!from.empty()) {
    load_config_file(from, cfg);
    if (!disk_arg.empty()) cfg.disk = disk_arg;
    if (!user_arg.empty()) cfg.username = user_arg;
    if (cfg.disk.empty()) fail("--disk is required with --from");
    if (cfg.distro.empty()) fail("no stratum 'name' in config");
    cfg.password = prompt_secret("Password for " + cfg.username);
  } else {
    std::vector<Disk> disks = list_disks();
    if (disks.empty()) fail("no disks found");
    printf("Available disks:\n");
    for (size_t i = 0; i < disks.size(); i++)
      printf("  [%zu] %s  %s  %s\n", i + 1, disks[i].node.c_str(), disks[i].size.c_str(),
             disks[i].model.c_str());
    std::string pick = prompt("Select disk number", "1");
    long idx = strtol(pick.c_str(), nullptr, 10);
    if (idx < 1 || idx > (long)disks.size()) fail("invalid disk selection");
    cfg.disk = disks[idx - 1].node;

    std::vector<std::string> strata = list_strata("/");
    if (strata.empty()) fail("no stratum definitions in /etc/salt/strata");
    printf("Available base distributions:\n");
    for (size_t i = 0; i < strata.size(); i++)
      printf("  [%zu] %s\n", i + 1, strata[i].c_str());
    std::string dp = prompt("Select distribution number", "1");
    long di = strtol(dp.c_str(), nullptr, 10);
    if (di < 1 || di > (long)strata.size()) fail("invalid distribution selection");
    cfg.distro = strata[di - 1];

    cfg.hostname = prompt("Hostname", cfg.hostname);
    cfg.username = prompt("Username", cfg.username);
    cfg.password = prompt_secret("Password for " + cfg.username);
    if (cfg.password.empty()) fail("empty password");
    cfg.timezone = prompt("Timezone", cfg.timezone);
    cfg.locale = prompt("Locale", cfg.locale);
    cfg.kernel = prompt("Kernel source", cfg.kernel);
  }

  std::string node = cfg.disk;
  if (access(node.c_str(), F_OK) != 0) fail("not a block device: " + node);

  if (interactive) {
    printf("\nAbout to ERASE %s and install saltOS with the %s stratum.\n", cfg.disk.c_str(),
           cfg.distro.c_str());
    printf("hostname=%s user=%s tz=%s locale=%s kernel=%s\n", cfg.hostname.c_str(),
           cfg.username.c_str(), cfg.timezone.c_str(), cfg.locale.c_str(), cfg.kernel.c_str());
    std::string yes = prompt("Type 'yes' to proceed", "");
    if (yes != "yes") fail("aborted");
  }

  struct utsname un{};
  uname(&un);
  std::string arch = un.machine;
  bool efi = salt_path_exists("/sys/firmware/efi");

  info("partitioning " + cfg.disk);
  run({"umount", "-R", mnt});
  run({"wipefs", "-a", cfg.disk});
  must({"sgdisk", "--zap-all", cfg.disk}, "zap");
  must({"sgdisk", "-n1:0:+1MiB", "-t1:ef02", "-c1:BIOS boot", cfg.disk}, "bios part");
  must({"sgdisk", "-n2:0:+512MiB", "-t2:ef00", "-c2:EFI", cfg.disk}, "esp part");
  must({"sgdisk", "-n3:0:0", "-t3:8300", "-c3:saltOS", cfg.disk}, "root part");
  run({"partprobe", cfg.disk});
  sleep(1);

  std::string esp = partnode(cfg.disk, 2);
  std::string rootp = partnode(cfg.disk, 3);

  info("creating filesystems");
  must({"mkfs.fat", "-F32", "-n", "EFI", esp}, "mkfs esp");
  must({"mkfs.btrfs", "-f", "-L", "saltOS", rootp}, "mkfs root");

  std::string base = cfg.disk.substr(cfg.disk.find_last_of('/') + 1);
  std::string rot = read_first_line("/sys/block/" + base + "/queue/rotational");
  std::string mopts = "rw,noatime,compress=zstd:1,space_cache=v2";
  if (rot == "0") mopts = "rw,noatime,compress=zstd:1,ssd,space_cache=v2,discard=async";

  info("creating btrfs subvolume layout");
  std::string top = mnt + "/.btrfs-top";
  must({"mkdir", "-p", top}, "mkdir top");
  must({"mount", "-o", "subvolid=5", rootp, top}, "mount top");
  const char *subs[] = {"@", "@home", "@var", "@log", "@snapshots", "@strata"};
  for (const char *s : subs) must({"btrfs", "subvolume", "create", top + "/" + s}, "subvol");
  must({"umount", top}, "umount top");
  run({"rmdir", top});

  must({"mount", "-o", mopts + ",subvol=@", rootp, mnt}, "mount @");
  for (const char *d : {"home", "var", "var/log", ".snapshots", "strata", "boot/efi"})
    must({"mkdir", "-p", mnt + "/" + d}, "mkdir");
  must({"mount", "-o", mopts + ",subvol=@home", rootp, mnt + "/home"}, "mount home");
  must({"mount", "-o", mopts + ",subvol=@var", rootp, mnt + "/var"}, "mount var");
  must({"mkdir", "-p", mnt + "/var/log"}, "mkdir varlog");
  must({"mount", "-o", mopts + ",subvol=@log", rootp, mnt + "/var/log"}, "mount log");
  must({"mount", "-o", mopts + ",subvol=@snapshots", rootp, mnt + "/.snapshots"}, "mount snaps");
  must({"mount", "-o", mopts + ",subvol=@strata", rootp, mnt + "/strata"}, "mount strata");
  must({"mount", esp, mnt + "/boot/efi"}, "mount esp");

  info("laying down native base");
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
          "/", mnt + "/"},
         "rsync base");
  }
  for (const char *d : {"proc", "sys", "dev", "run", "tmp", "mnt", "media", "strata"})
    run({"mkdir", "-p", mnt + "/" + d});
  run({"chmod", "1777", mnt + "/tmp"});

  info("writing fstab and hostname");
  std::string ruuid = blkid_uuid(rootp);
  std::string euuid = blkid_uuid(esp);
  salt_buf fstab;
  salt_buf_init(&fstab);
  salt_buf_printf(&fstab, "UUID=%s /           btrfs %s,subvol=@          0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /home       btrfs %s,subvol=@home      0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /var        btrfs %s,subvol=@var       0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /var/log    btrfs %s,subvol=@log       0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /.snapshots btrfs %s,subvol=@snapshots 0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /strata     btrfs %s,subvol=@strata    0 0\n", ruuid.c_str(),
                  mopts.c_str());
  salt_buf_printf(&fstab, "UUID=%s /boot/efi   vfat  umask=0077                  0 1\n",
                  euuid.c_str());
  std::string fstab_path = mnt + "/etc/fstab";
  salt_write_file(fstab_path.c_str(), fstab.data, fstab.len, 0644);
  salt_buf_free(&fstab);
  std::string hn = cfg.hostname + "\n";
  std::string hn_path = mnt + "/etc/hostname";
  salt_write_file(hn_path.c_str(), hn.data(), hn.size(), 0644);

  info("binding pseudo-filesystems");
  must({"mount", "--bind", "/dev", mnt + "/dev"}, "bind dev");
  run({"mount", "--bind", "/dev/pts", mnt + "/dev/pts"});
  must({"mount", "-t", "proc", "proc", mnt + "/proc"}, "mount proc");
  must({"mount", "-t", "sysfs", "sys", mnt + "/sys"}, "mount sys");
  must({"mount", "-t", "tmpfs", "tmpfs", mnt + "/run"}, "mount run");
  if (efi) run({"mount", "-t", "efivarfs", "efivarfs", mnt + "/sys/firmware/efi/efivars"});

  info("creating user " + cfg.username);
  chroot_run(mnt, {"useradd", "-m", "-s", "/bin/bash", cfg.username});
  std::string chpw = cfg.username + ":" + cfg.password + "\n";
  {
    int fds[2];
    if (pipe(fds) == 0) {
      pid_t pid = fork();
      if (pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp("chroot", "chroot", mnt.c_str(), "chpasswd", (char *)nullptr);
        _exit(127);
      }
      close(fds[0]);
      if (write(fds[1], chpw.data(), chpw.size()) < 0) {
      }
      close(fds[1]);
      int st = 0;
      waitpid(pid, &st, 0);
    }
  }
  if (chroot_run(mnt, {"usermod", "-aG", "sudo", cfg.username}) != 0)
    chroot_run(mnt, {"usermod", "-aG", "wheel", cfg.username});

  info("configuring locale and timezone");
  run({"ln", "-sf", "/usr/share/zoneinfo/" + cfg.timezone, mnt + "/etc/localtime"});
  std::string lg = cfg.locale + " UTF-8\n";
  std::string lg_path = mnt + "/etc/locale.gen";
  {
    salt_buf cur;
    salt_buf_init(&cur);
    salt_read_file(lg_path.c_str(), &cur);
    salt_buf_append_str(&cur, lg.c_str());
    salt_write_file(lg_path.c_str(), cur.data, cur.len, 0644);
    salt_buf_free(&cur);
  }
  chroot_run(mnt, {"locale-gen"});
  std::string lc = "LANG=" + cfg.locale + "\n";
  std::string lc_path = mnt + "/etc/locale.conf";
  salt_write_file(lc_path.c_str(), lc.data(), lc.size(), 0644);

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

  info("installing kernel and bootloader");
  std::string kver = highest_kver(mnt);
  if (kver.empty()) fail("no kernel modules in target");
  if (!salt_path_exists((mnt + "/boot/vmlinuz-" + kver).c_str()))
    fail("no kernel image at /boot/vmlinuz-" + kver);
  if (chroot_run(mnt, {"sh", "-c", "command -v update-initramfs"}) == 0)
    must(std::vector<std::string>{"chroot", mnt, "update-initramfs", "-c", "-k", kver},
         "initramfs");
  else if (chroot_run(mnt, {"sh", "-c", "command -v dracut"}) == 0)
    must(std::vector<std::string>{"chroot", mnt, "dracut", "--force",
                                  "/boot/initramfs-" + kver + ".img", kver},
         "dracut");
  else
    fail("no initramfs generator in target");

  std::string grub_def = mnt + "/etc/default/grub";
  if (!salt_path_exists(grub_def.c_str())) {
    std::string g =
        "GRUB_DEFAULT=0\nGRUB_TIMEOUT=3\nGRUB_DISTRIBUTOR=\"saltOS\"\n"
        "GRUB_CMDLINE_LINUX_DEFAULT=\"quiet\"\nGRUB_CMDLINE_LINUX=\"\"\n";
    salt_write_file(grub_def.c_str(), g.data(), g.size(), 0644);
  }
  if (arch == "aarch64") {
    chroot_run(mnt, {"grub-install", "--target=arm64-efi", "--efi-directory=/boot/efi",
                     "--bootloader-id=saltOS", "--recheck", "--removable"});
  } else {
    chroot_run(mnt, {"grub-install", "--target=i386-pc", "--recheck", cfg.disk});
    if (efi)
      chroot_run(mnt, {"grub-install", "--target=x86_64-efi", "--efi-directory=/boot/efi",
                       "--bootloader-id=saltOS", "--recheck", "--removable"});
  }
  must(std::vector<std::string>{"chroot", mnt, "grub-mkconfig", "-o", "/boot/grub/grub.cfg"},
       "grub-mkconfig");

  info("writing reproducible system config");
  salt_buf sys;
  salt_buf_init(&sys);
  salt_buf_printf(&sys, "[system]\nhostname = \"%s\"\nlocale = \"%s\"\ntimezone = \"%s\"\n\n",
                  cfg.hostname.c_str(), cfg.locale.c_str(), cfg.timezone.c_str());
  salt_buf_printf(&sys, "[kernel]\nsource = \"%s\"\n\n", cfg.kernel.c_str());
  salt_buf_printf(&sys, "[[stratum]]\nname = \"%s\"\nrole = \"primary\"\nexpose = true\n",
                  cfg.distro.c_str());
  std::string sys_dir = mnt + "/etc/salt";
  salt_mkdirs(sys_dir.c_str(), 0755);
  std::string sys_path = sys_dir + "/system.toml";
  salt_write_file(sys_path.c_str(), sys.data, sys.len, 0644);
  salt_buf_free(&sys);

  if (chroot_run(mnt, {"salt", "lock", "--output", "/etc/salt/system.lock.toml"}) != 0)
    info("lockfile capture incomplete; system.toml written, run 'salt lock' later");

  chroot_run(mnt, {"salt", "deployments", "--register-current"});

  delive(mnt, cfg.username);

  info("syncing");
  run({"sync"});
  run({"umount", "-R", mnt});
  info("saltOS installed to " + cfg.disk + " with the " + cfg.distro + " stratum");
  return 0;
}

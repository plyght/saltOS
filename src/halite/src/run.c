#define _GNU_SOURCE
#include "salt/run.h"
#include "salt/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <grp.h>
#include <errno.h>
#endif

void salt_run_opts_default(salt_run_opts *o) {
  if (!o) return;
  o->graphics = true;
  o->interactive = false;
  o->user = NULL;
  o->workdir = NULL;
}

#if defined(__linux__)

static const char *g_root = NULL;

static char *salt_run_full_path(const char *dst_in_root) {
  size_t rlen = strlen(g_root);
  size_t dlen = strlen(dst_in_root);
  char *full = (char *)malloc(rlen + dlen + 2);
  if (!full) return NULL;
  memcpy(full, g_root, rlen);
  size_t pos = rlen;
  while (pos > 1 && full[pos - 1] == '/') pos--;
  if (dlen == 0 || dst_in_root[0] != '/') full[pos++] = '/';
  memcpy(full + pos, dst_in_root, dlen);
  full[pos + dlen] = '\0';
  return full;
}

static void salt_run_bind_dir(const char *src, const char *dst_in_root) {
  struct stat st;
  if (stat(src, &st) != 0) return;
  char *dst = salt_run_full_path(dst_in_root);
  if (!dst) return;
  salt_mkdirs(dst, 0755);
  mount(src, dst, NULL, MS_BIND | MS_REC, NULL);
  free(dst);
}

static char *salt_run_parent_dir(const char *path) {
  char *copy = salt_strdup(path);
  if (!copy) return NULL;
  char *slash = strrchr(copy, '/');
  if (!slash) {
    free(copy);
    return NULL;
  }
  if (slash == copy) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return copy;
}

static void salt_run_bind_file(const char *src, const char *dst_in_root) {
  struct stat st;
  if (stat(src, &st) != 0) return;
  char *dst = salt_run_full_path(dst_in_root);
  if (!dst) return;
  char *parent = salt_run_parent_dir(dst);
  if (parent) {
    salt_mkdirs(parent, 0755);
    free(parent);
  }
  /* The target may be a symlink (e.g. Arch's /etc/resolv.conf ->
   * /run/systemd/resolve/stub-resolv.conf, which dangles inside the stratum).
   * Binding onto a dangling symlink fails silently, leaving the command with no
   * resolv.conf at all -> DNS failures. Replace anything that isn't a regular
   * file with a fresh empty file before binding the host's copy over it. */
  struct stat dstat;
  if (lstat(dst, &dstat) == 0 && !S_ISREG(dstat.st_mode)) {
    unlink(dst);
  }
  if (lstat(dst, &dstat) != 0) {
    int fd = open(dst, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
  }
  mount(src, dst, NULL, MS_BIND, NULL);
  free(dst);
}

static void salt_run_mount_proc(void) {
  char *proc = salt_run_full_path("/proc");
  if (!proc) {
    salt_run_bind_dir("/proc", "/proc");
    return;
  }
  salt_mkdirs(proc, 0755);
  if (mount("proc", proc, "proc", 0, NULL) != 0) {
    salt_run_bind_dir("/proc", "/proc");
  }
  free(proc);
}

/* Write a one-shot string to a /proc/self/{uid,gid}_map or setgroups file. */
static int salt_run_write_str(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  ssize_t n = write(fd, val, strlen(val));
  close(fd);
  return n < 0 ? -1 : 0;
}

/* Enter an unprivileged user namespace, mapping the calling user to uid/gid 0
 * inside it. This grants CAP_SYS_ADMIN over the new namespace -- enough to set
 * up the stratum mount namespace + chroot -- with NO real root, NO setuid, NO
 * sudo. The command then execs as namespace-root, which maps back to the
 * calling user on the host, so it runs as you and files are owned by you.
 * Returns 0 on success, -1 if the kernel denies unprivileged userns. */
static int salt_run_enter_userns(void) {
  uid_t ruid = getuid();
  gid_t rgid = getgid();
  if (unshare(CLONE_NEWUSER) != 0) return -1;
  char buf[64];
  /* Map the caller to *itself* (not to 0). Creating the namespace already grants
   * a full capability set within it -- enough for the mount/chroot setup -- so
   * the command can still run while keeping the caller's real uid/gid, exactly
   * like a normal command. setgroups must be denied before writing gid_map. */
  salt_run_write_str("/proc/self/setgroups", "deny");
  snprintf(buf, sizeof(buf), "%u %u 1", (unsigned)ruid, (unsigned)ruid);
  if (salt_run_write_str("/proc/self/uid_map", buf) != 0) return -1;
  snprintf(buf, sizeof(buf), "%u %u 1", (unsigned)rgid, (unsigned)rgid);
  if (salt_run_write_str("/proc/self/gid_map", buf) != 0) return -1;
  return 0;
}

static void salt_run_child(const salt_stratum *s, const salt_run_opts *opts,
                           char *const argv[]) {
  g_root = s->root;

  /* Unprivileged caller: get CAP_SYS_ADMIN via a user namespace instead of
   * requiring sudo. If the kernel forbids it, exit 126 so the dispatcher can
   * fall back to re-execing under sudo. */
  bool in_userns = false;
  if (geteuid() != 0) {
    if (salt_run_enter_userns() != 0) _exit(126);
    in_userns = true;
  }

  if (unshare(CLONE_NEWNS) != 0) {
    fprintf(stderr,
            "salt run: could not set up the stratum mount namespace: %s\n",
            strerror(errno));
    _exit(125);
  }

  mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

  salt_run_mount_proc();

  salt_run_bind_dir("/sys", "/sys");
  salt_run_bind_dir("/dev", "/dev");
  salt_run_bind_dir("/run", "/run");
  salt_run_bind_dir("/tmp", "/tmp");
  salt_run_bind_dir("/home", "/home");
  salt_run_bind_dir("/root", "/root");

  salt_run_bind_file("/etc/resolv.conf", "/etc/resolv.conf");
  salt_run_bind_file("/etc/hosts", "/etc/hosts");
  salt_run_bind_file("/etc/localtime", "/etc/localtime");

  /* Make the caller's working directory available at the same path inside the
   * stratum so commands run "in the repo" exactly like a normal shell -- file
   * watchers, relative paths and node_modules all resolve. /home, /root and
   * /tmp are already bound above; binding the workdir explicitly also covers
   * project dirs kept elsewhere (e.g. /srv, /opt, /mnt). */
  if (opts->workdir && opts->workdir[0] == '/' && strcmp(opts->workdir, "/") != 0)
    salt_run_bind_dir(opts->workdir, opts->workdir);

  const char *sudo_uid = getenv("SUDO_UID");
  const char *sudo_gid = getenv("SUDO_GID");
  const char *sudo_user = getenv("SUDO_USER");

  bool drop = false;
  long uid = 0;
  long gid = 0;
  bool keep_root = false;

  if (in_userns) {
    /* We are already mapped to the calling user (as namespace-root). No sudo
     * was involved, so there is nothing to drop -- exec as-is. */
  } else if (opts->user && strcmp(opts->user, "root") == 0) {
    keep_root = true;
  } else if (opts->user && opts->user[0] != '\0') {
    if (sudo_uid && sudo_gid) {
      uid = strtol(sudo_uid, NULL, 10);
      gid = strtol(sudo_gid, NULL, 10);
      if (uid != 0) drop = true;
    }
  } else {
    if (sudo_uid && sudo_gid) {
      uid = strtol(sudo_uid, NULL, 10);
      gid = strtol(sudo_gid, NULL, 10);
      if (uid != 0) drop = true;
    }
  }

  char home_buf[4096];
  const char *home = NULL;

  if (keep_root) {
    home = "/root";
  } else if (drop) {
    const char *sudo_home = getenv("SUDO_HOME");
    if (sudo_home && sudo_home[0] != '\0') {
      home = sudo_home;
    } else if (sudo_user && sudo_user[0] != '\0') {
      snprintf(home_buf, sizeof(home_buf), "/home/%s", sudo_user);
      home = home_buf;
    } else {
      home = getenv("HOME");
    }
  } else {
    home = getenv("HOME");
  }

  if (chroot(s->root) != 0) {
    fprintf(stderr, "salt run: chroot %s failed: %s\n", s->root, strerror(errno));
    _exit(125);
  }

  const char *workdir = NULL;
  if (opts->workdir && opts->workdir[0] != '\0') {
    workdir = opts->workdir;
  } else if (home && salt_is_dir(home)) {
    workdir = home;
  } else {
    workdir = "/";
  }
  if (chdir(workdir) != 0 && (!home || chdir(home) != 0) && chdir("/") != 0) {
    fprintf(stderr, "salt run: chdir failed: %s\n", strerror(errno));
    _exit(125);
  }

  if (drop) {
    setgroups(0, NULL);
    if (setgid((gid_t)gid) != 0) {
      fprintf(stderr, "salt run: setgid failed: %s\n", strerror(errno));
      _exit(125);
    }
    if (setuid((uid_t)uid) != 0) {
      fprintf(stderr, "salt run: setuid failed: %s\n", strerror(errno));
      _exit(125);
    }
  }

  setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 1);

  if (keep_root) {
    setenv("HOME", "/root", 1);
  } else if (home && home[0] != '\0') {
    setenv("HOME", home, 1);
  }

  if (drop && sudo_user && sudo_user[0] != '\0') {
    setenv("USER", sudo_user, 1);
    setenv("LOGNAME", sudo_user, 1);
  }

  execvp(argv[0], argv);
  fprintf(stderr, "salt run: exec %s failed: %s\n", argv[0], strerror(errno));
  _exit(127);
}

#endif

int salt_stratum_run(const salt_stratum *s, const salt_run_opts *opts, char *const argv[],
                     int *status) {
  if (status) *status = -1;

  if (!s || !s->root || s->root[0] == '\0') {
    salt_set_error("stratum has no root filesystem");
    return SALT_ERR;
  }
  if (!salt_is_dir(s->root)) {
    salt_set_error("stratum root '%s' is not a directory", s->root);
    return SALT_ERR;
  }
  if (!argv || !argv[0]) {
    salt_set_error("no command given to run in stratum");
    return SALT_ERR;
  }

  salt_run_opts local;
  if (!opts) {
    salt_run_opts_default(&local);
    opts = &local;
  }

#if defined(__linux__)
  pid_t pid = fork();
  if (pid < 0) {
    salt_set_error("fork failed: %s", strerror(errno));
    return SALT_ERR;
  }
  if (pid == 0) {
    salt_run_child(s, opts, argv);
    _exit(127);
  }

  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno == EINTR) continue;
    salt_set_error("waitpid failed: %s", strerror(errno));
    return SALT_ERR;
  }

  if (status) {
    if (WIFEXITED(st)) {
      *status = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
      *status = 128 + WTERMSIG(st);
    } else {
      *status = -1;
    }
  }
  return SALT_OK;
#else
  (void)opts;
  (void)argv;
  salt_set_error("stratum execution requires Linux");
  return SALT_ERR;
#endif
}

static const char *salt_pkg_kind(const salt_stratum *s) {
  const char *candidates[2];
  candidates[0] = s->package_manager;
  candidates[1] = s->family;

  for (int i = 0; i < 2; i++) {
    const char *v = candidates[i];
    if (!v || v[0] == '\0') continue;
    if (strcmp(v, "pacman") == 0 || strcmp(v, "arch") == 0) return "pacman";
    if (strcmp(v, "apt") == 0 || strcmp(v, "apt-get") == 0 ||
        strcmp(v, "debian") == 0 || strcmp(v, "ubuntu") == 0)
      return "apt";
    if (strcmp(v, "xbps") == 0 || strcmp(v, "void") == 0) return "xbps";
    if (strcmp(v, "apk") == 0 || strcmp(v, "alpine") == 0) return "apk";
    if (strcmp(v, "dnf") == 0 || strcmp(v, "yum") == 0 || strcmp(v, "fedora") == 0 ||
        strcmp(v, "rhel") == 0 || strcmp(v, "centos") == 0 || strcmp(v, "rocky") == 0 ||
        strcmp(v, "almalinux") == 0)
      return "dnf";
    if (strcmp(v, "zypper") == 0 || strcmp(v, "opensuse") == 0 || strcmp(v, "suse") == 0 ||
        strcmp(v, "sles") == 0)
      return "zypper";
  }
  return NULL;
}

int salt_stratum_pkg(const salt_stratum *s, const char *op, char *const pkgs[], int npkgs,
                     int *status) {
  if (status) *status = -1;

  if (!s) {
    salt_set_error("no stratum given");
    return SALT_ERR;
  }
  if (!op || op[0] == '\0') {
    salt_set_error("no package operation given");
    return SALT_ERR;
  }

  const char *kind = salt_pkg_kind(s);
  if (!kind) {
    salt_set_error("unknown package manager for stratum '%s'",
                   s->name ? s->name : "?");
    return SALT_ERR;
  }

  const char *base[8];
  int nbase = 0;
  bool append_pkgs = true;
  const char *pm_binary = NULL;
  bool known_op = true;

  if (strcmp(kind, "pacman") == 0) {
    pm_binary = "pacman";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "pacman";
      base[nbase++] = "-S";
      base[nbase++] = "--needed";
      base[nbase++] = "--noconfirm";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "pacman";
      base[nbase++] = "-Rns";
      base[nbase++] = "--noconfirm";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "pacman";
      base[nbase++] = "-Syu";
      base[nbase++] = "--noconfirm";
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "pacman";
      base[nbase++] = "-Ss";
    } else {
      known_op = false;
    }
  } else if (strcmp(kind, "dnf") == 0) {
    pm_binary = "dnf";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "dnf";
      base[nbase++] = "install";
      base[nbase++] = "-y";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "dnf";
      base[nbase++] = "remove";
      base[nbase++] = "-y";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "dnf";
      base[nbase++] = "upgrade";
      base[nbase++] = "-y";
      append_pkgs = false;
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "dnf";
      base[nbase++] = "search";
    } else {
      known_op = false;
    }
  } else if (strcmp(kind, "zypper") == 0) {
    pm_binary = "zypper";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "zypper";
      base[nbase++] = "-n";
      base[nbase++] = "install";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "zypper";
      base[nbase++] = "-n";
      base[nbase++] = "remove";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "zypper";
      base[nbase++] = "-n";
      base[nbase++] = "update";
      append_pkgs = false;
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "zypper";
      base[nbase++] = "-n";
      base[nbase++] = "search";
    } else {
      known_op = false;
    }
  } else if (strcmp(kind, "apt") == 0) {
    pm_binary = "apt-get";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "apt-get";
      base[nbase++] = "install";
      base[nbase++] = "-y";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "apt-get";
      base[nbase++] = "remove";
      base[nbase++] = "-y";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "sh";
      base[nbase++] = "-c";
      base[nbase++] = "apt-get update && apt-get dist-upgrade -y";
      append_pkgs = false;
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "apt-cache";
      base[nbase++] = "search";
    } else {
      known_op = false;
    }
  } else if (strcmp(kind, "apk") == 0) {
    pm_binary = "apk";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "apk";
      base[nbase++] = "add";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "apk";
      base[nbase++] = "del";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "apk";
      base[nbase++] = "upgrade";
      base[nbase++] = "-U";
      append_pkgs = false;
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "apk";
      base[nbase++] = "search";
      base[nbase++] = "-v";
    } else {
      known_op = false;
    }
  } else {
    pm_binary = "xbps-install";
    if (strcmp(op, "install") == 0) {
      base[nbase++] = "xbps-install";
      base[nbase++] = "-Sy";
    } else if (strcmp(op, "remove") == 0) {
      base[nbase++] = "xbps-remove";
      base[nbase++] = "-Ry";
    } else if (strcmp(op, "update") == 0) {
      base[nbase++] = "xbps-install";
      base[nbase++] = "-Suy";
    } else if (strcmp(op, "search") == 0) {
      base[nbase++] = "xbps-query";
      base[nbase++] = "-Rs";
    } else {
      known_op = false;
    }
  }

  if (!known_op) {
    nbase = 0;
    base[nbase++] = pm_binary;
    base[nbase++] = op;
    append_pkgs = true;
  }

  int npkg_eff = (append_pkgs && pkgs && npkgs > 0) ? npkgs : 0;
  int total = nbase + npkg_eff;

  char **argv = (char **)calloc((size_t)total + 1, sizeof(char *));
  if (!argv) {
    salt_set_error("out of memory building package command");
    return SALT_ERR;
  }

  int n = 0;
  for (int i = 0; i < nbase; i++) {
    argv[n++] = (char *)base[i];
  }
  for (int i = 0; i < npkg_eff; i++) {
    argv[n++] = pkgs[i];
  }
  argv[n] = NULL;

  salt_run_opts opts;
  salt_run_opts_default(&opts);
  opts.graphics = false;
  opts.interactive = true;
  opts.user = "root";
  opts.workdir = "/";

  int rc = salt_stratum_run(s, &opts, argv, status);

  free(argv);
  return rc;
}

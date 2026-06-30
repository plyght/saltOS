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
#include <pwd.h>
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
/* Write end of a pipe to the parent, used to report "userns denied" out-of-band
 * so it isn't conflated with a real exit code of 126. -1 when unavailable. */
static int g_signal_fd = -1;

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

/* Returns 0 on success, -1 if the source is missing or the bind failed. The
 * caller decides whether a given mount is critical enough to warn/abort -- a
 * silently-skipped bind of /home or the workdir used to surface as "my files
 * aren't there" with a clean exit and no diagnostic. */
static int salt_run_bind_dir(const char *src, const char *dst_in_root) {
  struct stat st;
  if (stat(src, &st) != 0) return -1;
  char *dst = salt_run_full_path(dst_in_root);
  if (!dst) return -1;
  salt_mkdirs(dst, 0755);
  int rc = mount(src, dst, NULL, MS_BIND | MS_REC, NULL);
  if (rc != 0)
    fprintf(stderr, "salt run: warning: could not bind %s into stratum: %s\n", src,
            strerror(errno));
  free(dst);
  return rc == 0 ? 0 : -1;
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
    if (salt_run_enter_userns() != 0) {
      if (g_signal_fd >= 0) {
        char u = 'U';
        ssize_t w = write(g_signal_fd, &u, 1);
        (void)w;
      }
      _exit(126);
    }
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

  /* The whole point of the expose UX: see and run everything from any stratum.
   * RUN: salt + the host shims on PATH -- salt is static so it runs under any
   * libc, and when invoked in here it re-enters the host mount namespace to do
   * its work (salt_escape_to_host in cli), so cross-stratum commands route to the
   * host cleanly with no nesting. SEE: bind /strata so other strata's files are
   * visible (a mount is the only way to share a filesystem view). Bound first so
   * the recursive bind doesn't also re-bind the /home,/root,/tmp mounts below. */
  salt_run_bind_dir("/strata", "/strata");
  salt_run_bind_dir("/usr/local/salt", "/usr/local/salt");
  salt_run_bind_file("/usr/bin/salt", "/usr/local/bin/salt");

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

  /* Resolve the caller's name and home on the HOST (before chroot), via the
   * host's passwd db, so they're correct even when the stratum's /etc/passwd
   * has no entry for this uid -- many CLIs (git author, getpwuid lookups, cache
   * paths) misbehave with an empty/wrong $USER or $HOME. */
  char home_buf[4096];
  char user_buf[256];
  const char *home = NULL;
  const char *run_user = NULL;
  uid_t ident_uid = drop ? (uid_t)uid : getuid();
  struct passwd *pw = getpwuid(ident_uid);

  if (keep_root) {
    run_user = "root";
  } else if (drop && sudo_user && sudo_user[0] != '\0') {
    run_user = sudo_user;
  } else if (pw && pw->pw_name && pw->pw_name[0] != '\0') {
    snprintf(user_buf, sizeof(user_buf), "%s", pw->pw_name);
    run_user = user_buf;
  } else {
    const char *e = getenv("USER");
    if (e && e[0] != '\0') run_user = e;
  }

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
    if ((!home || home[0] == '\0') && pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
      snprintf(home_buf, sizeof(home_buf), "%s", pw->pw_dir);
      home = home_buf;
    }
    if ((!home || home[0] == '\0') && run_user) {
      snprintf(home_buf, sizeof(home_buf), "/home/%s", run_user);
      home = home_buf;
    }
  }

  if (chroot(s->root) != 0) {
    fprintf(stderr, "salt run: chroot %s failed: %s\n", s->root, strerror(errno));
    _exit(125);
  }

  /* Per-user runtime dir. /run is bound from the host, but /run/user/<uid> is
   * absent on a logind-less base -- create it so $XDG_RUNTIME_DIR resolves
   * (Wayland compositors hard-require it; many CLIs use it for sockets/caches).
   * We're still privileged here (userns-root, or pre-setuid on the sudo path). */
  uid_t run_uid = drop ? (uid_t)uid : getuid();
  gid_t run_gid = drop ? (gid_t)gid : getgid();
  char xdg_runtime[64];
  snprintf(xdg_runtime, sizeof(xdg_runtime), "/run/user/%u", (unsigned)run_uid);
  if (mkdir("/run/user", 0755) != 0 && errno != EEXIST) { /* non-fatal */ }
  if (mkdir(xdg_runtime, 0700) != 0 && errno != EEXIST) { /* non-fatal */ }
  if (chown(xdg_runtime, run_uid, run_gid) != 0) { /* non-fatal */ }

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

  /* Shim dir LAST so a stratum's own binaries win (no pointless self-nesting),
   * and only names absent from this stratum fall through to a shim that routes
   * to whichever stratum provides them. /usr/local/bin is first so the bound
   * host `salt` is found. */
  setenv("PATH",
         "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/usr/local/salt/shims",
         1);
  setenv("XDG_RUNTIME_DIR", xdg_runtime, 1);

  if (home && home[0] != '\0')
    setenv("HOME", home, 1);
  else if (keep_root)
    setenv("HOME", "/root", 1);

  if (run_user && run_user[0] != '\0') {
    setenv("USER", run_user, 1);
    setenv("LOGNAME", run_user, 1);
  }

  /* If the inherited $SHELL doesn't exist in the stratum (e.g. host /bin/bash
   * but an Alpine/busybox stratum), tools that shell out would fail -- fall
   * back to the always-present /bin/sh. */
  const char *sh = getenv("SHELL");
  if (!sh || sh[0] != '/' || access(sh, X_OK) != 0) setenv("SHELL", "/bin/sh", 1);

  /* Graphics enabler: the single-uid user namespace drops supplementary groups
   * (setgroups is denied), so a compositor can't open /dev/dri or /dev/input by
   * video/input group membership. seatd sidesteps that by opening the devices
   * itself and passing fds over its socket. /run is bound from the host, so if
   * the host runs seatd its socket is already visible here -- point libseat at
   * it so e.g. `sway` on KMS/DRM can acquire GPU + input without root or groups. */
  if (access("/run/seatd.sock", F_OK) == 0 && !getenv("LIBSEAT_BACKEND"))
    setenv("LIBSEAT_BACKEND", "seatd", 1);

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
  int sigpipe[2] = {-1, -1};
  if (pipe(sigpipe) != 0) {
    sigpipe[0] = sigpipe[1] = -1;
  }
  g_signal_fd = sigpipe[1];

  pid_t pid = fork();
  if (pid < 0) {
    salt_set_error("fork failed: %s", strerror(errno));
    if (sigpipe[0] >= 0) close(sigpipe[0]);
    if (sigpipe[1] >= 0) close(sigpipe[1]);
    return SALT_ERR;
  }
  if (pid == 0) {
    if (sigpipe[0] >= 0) close(sigpipe[0]);
    salt_run_child(s, opts, argv);
    _exit(127);
  }
  if (sigpipe[1] >= 0) {
    close(sigpipe[1]);
    g_signal_fd = -1;
  }

  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno == EINTR) continue;
    salt_set_error("waitpid failed: %s", strerror(errno));
    if (sigpipe[0] >= 0) close(sigpipe[0]);
    return SALT_ERR;
  }

  /* Did the child signal "userns denied" before exiting? If so, report the
   * dedicated sentinel rather than its exit code (126), so a command that
   * really exits 126 is not mistaken for a setup failure. */
  char sig = 0;
  if (sigpipe[0] >= 0) {
    ssize_t r = read(sigpipe[0], &sig, 1);
    (void)r;
    close(sigpipe[0]);
  }
  if (sig == 'U') {
    if (status) *status = SALT_RUN_USERNS_DENIED;
    return SALT_OK;
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

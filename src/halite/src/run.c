#define _GNU_SOURCE
#include "salt/run.h"
#include "salt/hash.h"
#include "salt/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

#if defined(__linux__)
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
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

/* Read-only variant for host assets a stratum may use but must not modify. */
static void salt_run_bind_ro(const char *src, const char *dst_in_root) {
  struct stat st;
  if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode)) return;
  if (salt_run_bind_dir(src, dst_in_root) != 0) return;
  char *dst = salt_run_full_path(dst_in_root);
  if (!dst) return;
  mount(NULL, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
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

/* Set up every bind/mount that makes a stratum's filesystem view consistent
 * with the host's data dirs. Called once inside a fresh mount namespace --
 * either the per-stratum persistent holder (root path) or a per-command
 * userns+CLONE_NEWNS (unprivileged path). g_root must already point at the
 * stratum root. bind_workdir binds the caller's cwd too (only meaningful on the
 * per-command path; the shared holder uses the fixed host data-dir set). */
static void salt_run_setup_mounts(const salt_run_opts *opts, bool bind_workdir) {
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
  /* Share ALL of the user/data dirs, not a cherry-picked few: anywhere you keep
   * files should be the same inside any stratum as on the host. The distro's own
   * program dirs (/usr,/lib,/bin) stay isolated -- that's the point of strata --
   * but your data is the system's, shared. (bind_dir skips any that don't exist.) */
  salt_run_bind_dir("/tmp", "/tmp");
  salt_run_bind_dir("/home", "/home");
  salt_run_bind_dir("/root", "/root");
  salt_run_bind_dir("/srv", "/srv");
  salt_run_bind_dir("/opt", "/opt");
  salt_run_bind_dir("/mnt", "/mnt");
  salt_run_bind_dir("/media", "/media");

  /* Desktop integration: host fonts, icon/cursor themes and GTK/Qt themes,
   * read-only, beside (never over) the stratum's own. Fonts land under
   * /usr/local/share/fonts, which every distro's fontconfig scans; icons and
   * themes under /usr/share/saltos-host, appended to XDG_DATA_DIRS and
   * XCURSOR_PATH at exec so the stratum's own copies still win. Theme
   * *settings* (gtk-3.0/settings.ini, qt6ct, kdeglobals) live in ~/.config,
   * which /home already shares; the XDG desktop portal is reached over the
   * session bus in /run/user. */
  salt_run_bind_ro("/usr/share/fonts", "/usr/local/share/fonts/saltos-host");
  salt_run_bind_ro("/usr/share/icons", "/usr/share/saltos-host/icons");
  salt_run_bind_ro("/usr/share/themes", "/usr/share/saltos-host/themes");

  salt_run_bind_file("/etc/resolv.conf", "/etc/resolv.conf");
  salt_run_bind_file("/etc/hosts", "/etc/hosts");
  salt_run_bind_file("/etc/localtime", "/etc/localtime");
  /* NOTE: the user identity db (/etc/passwd,/group,/shadow) is deliberately NOT
   * bound here. A single-file bind breaks useradd (it renames over the file --
   * EBUSY) and shadows each distro's own system users (Arch's alpm/http -> pacman
   * refuses to run). Unified identity is a merge (host users + each stratum's
   * system users, via an NSS extrausers-style shared db), not a raw bind. */

  /* Make the caller's working directory available at the same path inside the
   * stratum so commands run "in the repo" exactly like a normal shell -- file
   * watchers, relative paths and node_modules all resolve. /home, /root and
   * /tmp are already bound above; binding the workdir explicitly also covers
   * project dirs kept elsewhere (e.g. /srv, /opt, /mnt). */
  if (bind_workdir && opts && opts->workdir && opts->workdir[0] == '/' &&
      strcmp(opts->workdir, "/") != 0)
    salt_run_bind_dir(opts->workdir, opts->workdir);
}

/* Path of the pid file that pins a stratum's persistent mount namespace. Lives
 * under /run (bound identically into every stratum), so the holder's pid -- and
 * thus /proc/<pid>/ns/mnt -- is reachable from any later invocation, host or
 * stratum. */
static void salt_run_ns_pidpath(const salt_stratum *s, char *out, size_t n) {
  snprintf(out, n, "/run/salt/ns/%s.pid", (s->name && s->name[0]) ? s->name : "default");
}

static pid_t salt_run_read_holder(const char *pidpath) {
  int fd = open(pidpath, O_RDONLY);
  if (fd < 0) return -1;
  char buf[32];
  ssize_t r = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (r <= 0) return -1;
  buf[r] = '\0';
  long p = strtol(buf, NULL, 10);
  return p > 0 ? (pid_t)p : -1;
}

static bool salt_run_holder_alive(pid_t p) {
  return p > 0 && kill(p, 0) == 0;
}

int salt_stratum_ns_stop(const salt_stratum *s) {
  char pidpath[256];
  salt_run_ns_pidpath(s, pidpath, sizeof(pidpath));
  pid_t p = salt_run_read_holder(pidpath);
  if (salt_run_holder_alive(p)) {
    kill(p, SIGTERM);
    for (int i = 0; i < 50 && salt_run_holder_alive(p); i++) {
      struct timespec ts = {0, 20 * 1000 * 1000};
      nanosleep(&ts, NULL);
    }
    if (salt_run_holder_alive(p)) kill(p, SIGKILL);
  }
  unlink(pidpath);
  return SALT_OK;
}

/* Body of the detached holder: create the stratum's mount namespace, lay down
 * all the binds ONCE, publish the pid file (atomically, so a poller never sees a
 * half-written pid), then pause() forever. Its /proc/<pid>/ns/mnt is what every
 * later `salt run <stratum>` setns()es into, giving all commands one consistent
 * filesystem view. Returns 0 on success, -1 if namespace setup failed. */
static int salt_run_holder_setup(const salt_stratum *s, const char *pidpath) {
  g_root = s->root;
  if (unshare(CLONE_NEWNS) != 0) return -1;
  salt_run_setup_mounts(NULL, false);

  char tmp[300];
  snprintf(tmp, sizeof(tmp), "%s.tmp.%d", pidpath, (int)getpid());
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return -1;
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
  ssize_t w = write(fd, buf, (size_t)n);
  close(fd);
  if (w != n) {
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, pidpath) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

/* Ensure the stratum's persistent namespace holder exists and return its pid.
 * A live holder is reused; a stale/dead one (pid file present but process gone)
 * is recreated. Creation double-forks + setsid so the holder is detached from
 * this command and survives it, then blocks in salt_run_holder_setup's pause().
 * An flock serializes concurrent first-runs so only one holder is created. */
static pid_t salt_run_ensure_holder(const salt_stratum *s) {
  char pidpath[256];
  salt_run_ns_pidpath(s, pidpath, sizeof(pidpath));

  pid_t existing = salt_run_read_holder(pidpath);
  if (salt_run_holder_alive(existing)) return existing;

  salt_mkdirs("/run/salt/ns", 0755);

  char lockpath[300];
  snprintf(lockpath, sizeof(lockpath), "%s.lock", pidpath);
  int lockfd = open(lockpath, O_CREAT | O_RDWR, 0644);
  if (lockfd >= 0) {
    while (flock(lockfd, LOCK_EX) != 0 && errno == EINTR) {
    }
  }

  /* Re-check under the lock: another invocation may have just created it. */
  existing = salt_run_read_holder(pidpath);
  if (salt_run_holder_alive(existing)) {
    if (lockfd >= 0) close(lockfd);
    return existing;
  }
  unlink(pidpath);

  pid_t mid = fork();
  if (mid < 0) {
    if (lockfd >= 0) close(lockfd);
    return -1;
  }
  if (mid == 0) {
    if (lockfd >= 0) close(lockfd);
    if (setsid() < 0) _exit(127);
    pid_t h = fork();
    if (h < 0) _exit(127);
    if (h > 0) _exit(0);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
      dup2(nul, 0);
      dup2(nul, 1);
      dup2(nul, 2);
      if (nul > 2) close(nul);
    }
    /* Detach from EVERY inherited descriptor. The holder outlives this command,
     * so any fd it keeps open is leaked forever -- most damagingly the parent's
     * "userns denied" signal pipe (g_signal_fd), whose write end staying open
     * makes the parent's read() block indefinitely and hangs the whole run. */
    for (int fd = 3; fd < 256; fd++) close(fd);
    if (salt_run_holder_setup(s, pidpath) != 0) _exit(127);
    for (;;) pause();
    _exit(0);
  }

  int st;
  while (waitpid(mid, &st, 0) < 0 && errno == EINTR) {
  }

  pid_t holder = -1;
  for (int i = 0; i < 500; i++) {
    holder = salt_run_read_holder(pidpath);
    if (salt_run_holder_alive(holder)) break;
    usleep(10000);
  }
  if (lockfd >= 0) close(lockfd);
  return salt_run_holder_alive(holder) ? holder : -1;
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

static void salt_run_child(const salt_stratum *s, const salt_run_opts *opts, char *const argv[]) {
  g_root = s->root;

  /* Root path: JOIN the stratum's ONE persistent mount namespace instead of
   * unsharing a throwaway one per command. The holder set up all the binds once
   * and pins the namespace via pause(); setns()ing into it means every `salt run
   * <stratum>` -- and therefore every exposed shim -- shares a single, coherent
   * filesystem view (a file one command creates, the next command sees). Each
   * stratum keeps its OWN namespace/root (Arch -> /strata/arch, Alpine ->
   * /strata/alpine); only the host data dirs are bound identically into each. */
  bool in_userns = false;
  if (geteuid() == 0) {
    pid_t holder = salt_run_ensure_holder(s);
    if (holder <= 0) {
      fprintf(stderr, "salt run: could not set up the stratum mount namespace holder\n");
      _exit(125);
    }
    char nspath[64];
    snprintf(nspath, sizeof(nspath), "/proc/%d/ns/mnt", (int)holder);
    int nsfd = open(nspath, O_RDONLY);
    if (nsfd < 0) {
      fprintf(stderr, "salt run: could not open %s: %s\n", nspath, strerror(errno));
      _exit(125);
    }
    if (setns(nsfd, CLONE_NEWNS) != 0) {
      fprintf(stderr, "salt run: could not join the stratum namespace: %s\n", strerror(errno));
      close(nsfd);
      _exit(125);
    }
    close(nsfd);
  } else {
    /* Unprivileged caller: persistent-join needs CAP_SYS_ADMIN we don't have, so
     * keep the per-command model -- get capabilities via a user namespace (no
     * sudo) and unshare a fresh mount namespace. If the kernel forbids userns,
     * exit 126 so the dispatcher can fall back to re-execing under sudo. */
    if (salt_run_enter_userns() != 0) {
      if (g_signal_fd >= 0) {
        char u = 'U';
        ssize_t w = write(g_signal_fd, &u, 1);
        (void)w;
      }
      _exit(126);
    }
    in_userns = true;
    if (unshare(CLONE_NEWNS) != 0) {
      fprintf(stderr, "salt run: could not set up the stratum mount namespace: %s\n",
              strerror(errno));
      _exit(125);
    }
    salt_run_setup_mounts(opts, true);
  }

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
  if (mkdir("/run/user", 0755) != 0 && errno != EEXIST) { /* non-fatal */
  }
  if (mkdir(xdg_runtime, 0700) != 0 && errno != EEXIST) { /* non-fatal */
  }
  if (chown(xdg_runtime, run_uid, run_gid) != 0) { /* non-fatal */
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

  /* Shim dir LAST so a stratum's own binaries win (no pointless self-nesting),
   * and only names absent from this stratum fall through to a shim that routes
   * to whichever stratum provides them. /usr/local/bin is first so the bound
   * host `salt` is found. */
  setenv("PATH",
         "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/usr/local/salt/shims", 1);
  setenv("XDG_RUNTIME_DIR", xdg_runtime, 1);

  if (home && home[0] != '\0')
    setenv("HOME", home, 1);
  else if (keep_root)
    setenv("HOME", "/root", 1);

  if (run_user && run_user[0] != '\0') {
    setenv("USER", run_user, 1);
    setenv("LOGNAME", run_user, 1);
  }

  /* Host icons/themes (bound read-only in setup_mounts) come after the
   * stratum's own data dirs. The host's own XDG_DATA_DIRS names host paths, so
   * it is replaced rather than extended. */
  setenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share:/usr/share/saltos-host", 1);
  {
    const char *h = getenv("HOME");
    char cursor[1024];
    snprintf(cursor, sizeof(cursor),
             "%s/.local/share/icons:%s/.icons:/usr/share/icons:/usr/share/pixmaps:"
             "/usr/share/saltos-host/icons",
             h ? h : "", h ? h : "");
    setenv("XCURSOR_PATH", cursor, 1);
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
    if (strcmp(v, "apt") == 0 || strcmp(v, "apt-get") == 0 || strcmp(v, "debian") == 0 ||
        strcmp(v, "ubuntu") == 0)
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
    salt_set_error("unknown package manager for stratum '%s'", s->name ? s->name : "?");
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

void salt_foreign_pkg_list_init(salt_foreign_pkg_list *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

int salt_foreign_pkg_list_push(salt_foreign_pkg_list *l, const char *name, const char *version) {
  if (l->len == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 64;
    salt_foreign_pkg *n = (salt_foreign_pkg *)realloc(l->items, ncap * sizeof(*n));
    if (!n) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    l->items = n;
    l->cap = ncap;
  }
  l->items[l->len].name = salt_strdup(name);
  l->items[l->len].version = salt_strdup(version);
  l->items[l->len].digest = NULL;
  if (!l->items[l->len].name || !l->items[l->len].version) {
    free(l->items[l->len].name);
    free(l->items[l->len].version);
    salt_set_error("out of memory");
    return SALT_ERR;
  }
  l->len++;
  return SALT_OK;
}

int salt_foreign_pkg_set_digest(salt_foreign_pkg *p, const char *digest) {
  char *d = salt_strdup(digest);
  if (!d) {
    salt_set_error("out of memory");
    return SALT_ERR;
  }
  free(p->digest);
  p->digest = d;
  return SALT_OK;
}

const salt_foreign_pkg *salt_foreign_pkg_list_find(const salt_foreign_pkg_list *l,
                                                   const char *name) {
  for (size_t i = 0; i < l->len; i++)
    if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
  return NULL;
}

void salt_foreign_pkg_list_free(salt_foreign_pkg_list *l) {
  for (size_t i = 0; i < l->len; i++) {
    free(l->items[i].name);
    free(l->items[i].version);
    free(l->items[i].digest);
  }
  free(l->items);
  salt_foreign_pkg_list_init(l);
}

const char *salt_stratum_pkg_kind(const salt_stratum *s) {
  return s ? salt_pkg_kind(s) : NULL;
}

static int foreign_pkg_cmp(const void *a, const void *b) {
  return strcmp(((const salt_foreign_pkg *)a)->name, ((const salt_foreign_pkg *)b)->name);
}

/* Split "name-ver-rel" where the version is the last NDASH dash-separated
 * fields (apk: ver-rN, xbps/pacman-style pkgver: ver). */
static int split_trailing(const char *tok, int ndash, salt_foreign_pkg_list *out) {
  const char *cut = tok + strlen(tok);
  for (int i = 0; i < ndash; i++) {
    while (cut > tok && *cut != '-') cut--;
    if (cut == tok) return SALT_ERR;
    if (i + 1 < ndash) cut--;
  }
  if (cut == tok) return SALT_ERR;
  char *name = (char *)malloc((size_t)(cut - tok) + 1);
  if (!name) return SALT_ERR;
  memcpy(name, tok, (size_t)(cut - tok));
  name[cut - tok] = '\0';
  int rc = salt_foreign_pkg_list_push(out, name, cut + 1);
  free(name);
  return rc;
}

int salt_foreign_pkg_parse(const char *kind, const char *text, size_t len,
                           salt_foreign_pkg_list *out) {
  if (!kind) {
    salt_set_error("unknown package manager");
    return SALT_ERR;
  }
  const char *p = text;
  const char *end = text + len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) nl = end;
    size_t n = (size_t)(nl - p);
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
    char *line = (char *)malloc(n + 1);
    if (!line) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    memcpy(line, p, n);
    line[n] = '\0';
    p = nl < end ? nl + 1 : end;
    if (n == 0) {
      free(line);
      continue;
    }
    int rc = SALT_OK;
    if (strcmp(kind, "pacman") == 0) {
      char *sp = strchr(line, ' ');
      if (!sp) {
        salt_set_error("pacman -Q: malformed line '%s'", line);
        rc = SALT_ERR;
      } else {
        *sp = '\0';
        rc = salt_foreign_pkg_list_push(out, line, sp + 1);
      }
    } else if (strcmp(kind, "apt") == 0) {
      char *t1 = strchr(line, '\t');
      char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
      if (!t1 || !t2) {
        salt_set_error("dpkg-query: malformed line '%s'", line);
        rc = SALT_ERR;
      } else {
        *t1 = '\0';
        *t2 = '\0';
        if (strcmp(line, "installed") == 0) rc = salt_foreign_pkg_list_push(out, t1 + 1, t2 + 1);
      }
    } else if (strcmp(kind, "apk") == 0) {
      if (strncmp(line, "WARNING", 7) == 0) {
        free(line);
        continue;
      }
      if (split_trailing(line, 2, out) != SALT_OK) {
        salt_set_error("apk info: malformed line '%s'", line);
        rc = SALT_ERR;
      }
    } else if (strcmp(kind, "dnf") == 0 || strcmp(kind, "zypper") == 0) {
      char *t1 = strchr(line, '\t');
      if (!t1) {
        salt_set_error("rpm -qa: malformed line '%s'", line);
        rc = SALT_ERR;
      } else {
        *t1 = '\0';
        if (strcmp(line, "gpg-pubkey") != 0) rc = salt_foreign_pkg_list_push(out, line, t1 + 1);
      }
    } else if (strcmp(kind, "xbps") == 0) {
      /* "ii pkg-1.0_1  summary": two-letter state, pkgver, description */
      if (line[0] != 'i' || line[1] == '\0' || line[2] != ' ') {
        free(line);
        continue;
      }
      char *tok = line + 3;
      while (*tok == ' ') tok++;
      char *sp = strchr(tok, ' ');
      if (sp) *sp = '\0';
      if (split_trailing(tok, 1, out) != SALT_OK) {
        salt_set_error("xbps-query -l: malformed line '%s'", line);
        rc = SALT_ERR;
      }
    } else {
      salt_set_error("unknown package manager '%s'", kind);
      rc = SALT_ERR;
    }
    free(line);
    if (rc != SALT_OK) return rc;
  }
  if (out->len > 1) qsort(out->items, out->len, sizeof(*out->items), foreign_pkg_cmp);
  return SALT_OK;
}

int salt_foreign_pkg_spec(const char *kind, const char *name, const char *version, salt_buf *out) {
  if (!kind || !name || !version || !name[0] || !version[0]) {
    salt_set_error("package spec needs a manager, name and version");
    return SALT_ERR;
  }
  if (strcmp(kind, "apt") == 0 || strcmp(kind, "apk") == 0 || strcmp(kind, "zypper") == 0)
    return salt_buf_printf(out, "%s=%s", name, version);
  if (strcmp(kind, "dnf") == 0 || strcmp(kind, "xbps") == 0)
    return salt_buf_printf(out, "%s-%s", name, version);
  if (strcmp(kind, "pacman") == 0) {
    salt_set_error("pacman repositories serve only the current version of %s", name);
    return SALT_ERR;
  }
  salt_set_error("unknown package manager '%s'", kind);
  return SALT_ERR;
}

static int pkg_run(const salt_stratum *s, char *const argv[], int *status) {
  salt_run_opts opts;
  salt_run_opts_default(&opts);
  opts.graphics = false;
  opts.interactive = false;
  opts.user = "root";
  opts.workdir = "/";
  return salt_stratum_run(s, &opts, argv, status);
}

static int pkg_run_capture(const salt_stratum *s, char *const argv[], salt_buf *out, int *status) {
  char tmpl[] = "/tmp/salt-pkgq-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    salt_set_error("mkstemp: %s", strerror(errno));
    return SALT_ERR;
  }
  unlink(tmpl);
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  if (saved < 0 || dup2(fd, STDOUT_FILENO) < 0) {
    salt_set_error("dup2: %s", strerror(errno));
    if (saved >= 0) close(saved);
    close(fd);
    return SALT_ERR;
  }
  int rc = pkg_run(s, argv, status);
  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  if (rc == SALT_OK) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
      salt_set_error("lseek: %s", strerror(errno));
      rc = SALT_ERR;
    } else {
      char chunk[8192];
      ssize_t r;
      while ((r = read(fd, chunk, sizeof(chunk))) > 0) salt_buf_append(out, chunk, (size_t)r);
      if (r < 0) {
        salt_set_error("read: %s", strerror(errno));
        rc = SALT_ERR;
      }
    }
  }
  close(fd);
  return rc;
}

int salt_stratum_pkg_query(const salt_stratum *s, salt_foreign_pkg_list *out) {
  const char *kind = salt_stratum_pkg_kind(s);
  if (!kind) {
    salt_set_error("unknown package manager for stratum '%s'", s && s->name ? s->name : "?");
    return SALT_ERR;
  }
  const char *argv[5] = {0};
  if (strcmp(kind, "pacman") == 0) {
    argv[0] = "pacman";
    argv[1] = "-Q";
  } else if (strcmp(kind, "apt") == 0) {
    argv[0] = "dpkg-query";
    argv[1] = "-W";
    argv[2] = "--showformat=${db:Status-Status}\t${Package}\t${Version}\n";
  } else if (strcmp(kind, "apk") == 0) {
    argv[0] = "apk";
    argv[1] = "info";
    argv[2] = "-v";
  } else if (strcmp(kind, "xbps") == 0) {
    argv[0] = "xbps-query";
    argv[1] = "-l";
  } else {
    argv[0] = "rpm";
    argv[1] = "-qa";
    argv[2] = "--qf";
    argv[3] = "%{NAME}\\t%{EVR}\\n";
  }
  salt_buf text;
  salt_buf_init(&text);
  int status = -1;
  int rc = pkg_run_capture(s, (char *const *)argv, &text, &status);
  if (rc == SALT_OK && status != 0) {
    salt_set_error("%s exited %d while listing packages in stratum '%s'", argv[0], status,
                   s->name ? s->name : "?");
    rc = SALT_ERR;
  }
  if (rc == SALT_OK) rc = salt_foreign_pkg_parse(kind, text.data ? text.data : "", text.len, out);
  salt_buf_free(&text);
  return rc;
}

static salt_foreign_pkg *foreign_pkg_find_mut(salt_foreign_pkg_list *l, const char *name) {
  for (size_t i = 0; i < l->len; i++)
    if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
  return NULL;
}

static int digest_set_prefixed(salt_foreign_pkg *p, const char *prefix, const char *value) {
  salt_buf b;
  salt_buf_init(&b);
  int rc = salt_buf_printf(&b, "%s:%s", prefix, value);
  if (rc == SALT_OK) rc = salt_foreign_pkg_set_digest(p, b.data);
  salt_buf_free(&b);
  return rc;
}

static int digests_complete(const char *kind, const salt_foreign_pkg_list *l) {
  for (size_t i = 0; i < l->len; i++)
    if (!l->items[i].digest) {
      salt_set_error("%s reports no content digest for %s %s", kind, l->items[i].name,
                     l->items[i].version);
      return SALT_ERR;
    }
  return SALT_OK;
}

static bool hex_valid(const char *s) {
  size_t n = strlen(s);
  if (n != SALT_SHA256_HEXLEN) return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}

int salt_foreign_pkg_parse_digests(const char *kind, const char *text, size_t len,
                                   salt_foreign_pkg_list *list) {
  if (!kind) {
    salt_set_error("unknown package manager");
    return SALT_ERR;
  }
  bool rpm = strcmp(kind, "dnf") == 0 || strcmp(kind, "zypper") == 0;
  bool xbps = strcmp(kind, "xbps") == 0;
  bool apk = strcmp(kind, "apk") == 0;
  if (!rpm && !xbps && !apk) {
    salt_set_error("%s digests are not parsed from text", kind);
    return SALT_ERR;
  }
  char *apk_name = NULL, *apk_ver = NULL, *apk_sum = NULL;
  const char *p = text;
  const char *end = text + len;
  int rc = SALT_OK;
  while (rc == SALT_OK && p <= end) {
    const char *nl = p < end ? memchr(p, '\n', (size_t)(end - p)) : NULL;
    if (!nl) nl = end;
    size_t n = (size_t)(nl - p);
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
    char *line = (char *)malloc(n + 1);
    if (!line) {
      salt_set_error("out of memory");
      rc = SALT_ERR;
      break;
    }
    memcpy(line, p, n);
    line[n] = '\0';
    bool last = nl >= end;
    p = last ? end + 1 : nl + 1;
    if (apk) {
      if (n == 0) {
        if (apk_name && apk_ver && apk_sum) {
          salt_foreign_pkg *f = foreign_pkg_find_mut(list, apk_name);
          if (f && strcmp(f->version, apk_ver) == 0)
            rc = digest_set_prefixed(f, "apk-checksum", apk_sum);
        } else if (apk_name) {
          salt_set_error("apk installed db: %s lacks a version or checksum", apk_name);
          rc = SALT_ERR;
        }
        free(apk_name);
        free(apk_ver);
        free(apk_sum);
        apk_name = apk_ver = apk_sum = NULL;
      } else if (line[1] == ':') {
        char **slot = line[0] == 'P'   ? &apk_name
                      : line[0] == 'V' ? &apk_ver
                      : line[0] == 'C' ? &apk_sum
                                       : NULL;
        if (slot) {
          free(*slot);
          *slot = salt_strdup(line + 2);
          if (!*slot) {
            salt_set_error("out of memory");
            rc = SALT_ERR;
          }
        }
      }
    } else if (n > 0) {
      char *t1 = strchr(line, '\t');
      char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
      if (rpm) {
        if (!t1 || !t2) {
          salt_set_error("rpm -qa: malformed digest line '%s'", line);
          rc = SALT_ERR;
        } else {
          *t1 = '\0';
          *t2 = '\0';
          salt_foreign_pkg *f = foreign_pkg_find_mut(list, line);
          if (f && strcmp(f->version, t1 + 1) == 0) {
            if (!hex_valid(t2 + 1)) {
              salt_set_error("rpm reports no SHA256HEADER for %s %s (got '%s')", line, t1 + 1,
                             t2 + 1);
              rc = SALT_ERR;
            } else {
              rc = digest_set_prefixed(f, "rpm-sha256header", t2 + 1);
            }
          }
        }
      } else {
        if (!t1) {
          salt_set_error("xbps-query: malformed digest line '%s'", line);
          rc = SALT_ERR;
        } else {
          *t1 = '\0';
          char *dash = strrchr(line, '-');
          if (!dash || dash == line) {
            salt_set_error("xbps-query: malformed pkgver '%s'", line);
            rc = SALT_ERR;
          } else {
            *dash = '\0';
            salt_foreign_pkg *f = foreign_pkg_find_mut(list, line);
            if (f && strcmp(f->version, dash + 1) == 0) {
              if (!hex_valid(t1 + 1)) {
                salt_set_error("xbps reports no filename-sha256 for %s-%s (got '%s')", line,
                               dash + 1, t1 + 1);
                rc = SALT_ERR;
              } else {
                rc = digest_set_prefixed(f, "sha256", t1 + 1);
              }
            }
          }
        }
      }
    }
    free(line);
  }
  free(apk_name);
  free(apk_ver);
  free(apk_sum);
  if (rc == SALT_OK) rc = digests_complete(kind, list);
  return rc;
}

static int digest_host_file(const salt_stratum *s, const char *rel, const char *prefix,
                            salt_foreign_pkg *p) {
  char *path = salt_join_path(s->root ? s->root : "", rel);
  if (!path) {
    salt_set_error("out of memory");
    return SALT_ERR;
  }
  char hex[SALT_SHA256_HEXLEN + 1];
  int rc = salt_sha256_file(path, hex);
  free(path);
  if (rc != SALT_OK) return rc;
  return digest_set_prefixed(p, prefix, hex);
}

static int dpkg_info_file(const salt_stratum *s, const char *name, const char *ext, salt_buf *out) {
  const char *info = "var/lib/dpkg/info";
  salt_buf plain;
  salt_buf_init(&plain);
  salt_buf_printf(&plain, "%s/%s/%s%s", s->root ? s->root : "", info, name, ext);
  bool found = salt_path_exists(plain.data);
  salt_buf_free(&plain);
  if (found) return salt_buf_printf(out, "%s/%s%s", info, name, ext);
  salt_buf dir;
  salt_buf_init(&dir);
  salt_buf_printf(&dir, "%s/%s", s->root ? s->root : "", info);
  DIR *d = opendir(dir.data);
  salt_buf_free(&dir);
  if (!d) return SALT_ERR;
  size_t nl = strlen(name), el = strlen(ext);
  int rc = SALT_ERR;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t l = strlen(e->d_name);
    if (l <= nl + el || strncmp(e->d_name, name, nl) != 0 || e->d_name[nl] != ':') continue;
    if (strcmp(e->d_name + l - el, ext) != 0) continue;
    if (memchr(e->d_name + nl + 1, '.', l - nl - 1 - el) != NULL) continue;
    rc = salt_buf_printf(out, "%s/%s", info, e->d_name);
    break;
  }
  closedir(d);
  return rc;
}

int salt_stratum_pkg_digests(const salt_stratum *s, salt_foreign_pkg_list *list) {
  const char *kind = salt_stratum_pkg_kind(s);
  if (!kind) {
    salt_set_error("unknown package manager for stratum '%s'", s && s->name ? s->name : "?");
    return SALT_ERR;
  }
  if (strcmp(kind, "pacman") == 0) {
    for (size_t i = 0; i < list->len; i++) {
      salt_buf rel;
      salt_buf_init(&rel);
      salt_buf_printf(&rel, "var/lib/pacman/local/%s-%s/mtree", list->items[i].name,
                      list->items[i].version);
      int rc = digest_host_file(s, rel.data, "pacman-mtree-sha256", &list->items[i]);
      if (rc != SALT_OK)
        salt_set_error("pacman has no mtree manifest for %s %s under %s/%s", list->items[i].name,
                       list->items[i].version, s->root ? s->root : "", rel.data);
      salt_buf_free(&rel);
      if (rc != SALT_OK) return rc;
    }
    return SALT_OK;
  }
  if (strcmp(kind, "apt") == 0) {
    for (size_t i = 0; i < list->len; i++) {
      salt_buf rel;
      salt_buf_init(&rel);
      const char *prefix = "dpkg-md5sums-sha256";
      int rc = dpkg_info_file(s, list->items[i].name, ".md5sums", &rel);
      if (rc != SALT_OK) {
        prefix = "dpkg-list-sha256";
        rc = dpkg_info_file(s, list->items[i].name, ".list", &rel);
      }
      if (rc == SALT_OK) rc = digest_host_file(s, rel.data, prefix, &list->items[i]);
      if (rc != SALT_OK)
        salt_set_error("dpkg has no md5sums or list file for %s %s under %s/var/lib/dpkg/info",
                       list->items[i].name, list->items[i].version, s->root ? s->root : "");
      salt_buf_free(&rel);
      if (rc != SALT_OK) return rc;
    }
    return SALT_OK;
  }
  salt_buf text;
  salt_buf_init(&text);
  int rc;
  if (strcmp(kind, "apk") == 0) {
    char *path = salt_join_path(s->root ? s->root : "", "lib/apk/db/installed");
    if (!path) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    rc = salt_read_file(path, &text);
    free(path);
  } else {
    const char *argv[5] = {0};
    if (strcmp(kind, "xbps") == 0) {
      argv[0] = "sh";
      argv[1] = "-c";
      argv[2] =
          "set -e; xbps-query -l | while read -r st pv rest; do "
          "printf '%s\\t%s\\n' \"$pv\" \"$(xbps-query -p filename-sha256 \"$pv\")\"; done";
    } else {
      argv[0] = "rpm";
      argv[1] = "-qa";
      argv[2] = "--qf";
      argv[3] = "%{NAME}\\t%{EVR}\\t%{SHA256HEADER}\\n";
    }
    int status = -1;
    rc = pkg_run_capture(s, (char *const *)argv, &text, &status);
    if (rc == SALT_OK && status != 0) {
      salt_set_error("%s exited %d while reading package digests in stratum '%s'", argv[0], status,
                     s->name ? s->name : "?");
      rc = SALT_ERR;
    }
  }
  if (rc == SALT_OK)
    rc = salt_foreign_pkg_parse_digests(kind, text.data ? text.data : "", text.len, list);
  salt_buf_free(&text);
  return rc;
}

static int pacman_cached_artifact(const salt_stratum *s, const salt_foreign_pkg *p, salt_buf *out) {
  const char *cache = "var/cache/pacman/pkg";
  salt_buf dir;
  salt_buf_init(&dir);
  salt_buf_printf(&dir, "%s/%s", s->root ? s->root : "", cache);
  DIR *d = opendir(dir.data);
  salt_buf_free(&dir);
  if (!d) return SALT_ERR;
  salt_buf prefix;
  salt_buf_init(&prefix);
  salt_buf_printf(&prefix, "%s-%s-", p->name, p->version);
  int rc = SALT_ERR;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix.data, prefix.len) != 0) continue;
    if (strstr(e->d_name, ".pkg.tar") == NULL) continue;
    size_t l = strlen(e->d_name);
    if (l >= 4 && strcmp(e->d_name + l - 4, ".sig") == 0) continue;
    /* the arch field may not itself contain a dash, so the remainder past the
       prefix must be "<arch>.pkg.tar.<ext>" */
    if (strchr(e->d_name + prefix.len, '-') != NULL) continue;
    salt_buf_printf(out, "/%s/%s", cache, e->d_name);
    rc = SALT_OK;
    break;
  }
  salt_buf_free(&prefix);
  closedir(d);
  return rc;
}

int salt_stratum_pkg_install_exact(const salt_stratum *s, const salt_foreign_pkg *pkgs, size_t n,
                                   int *status) {
  if (status) *status = -1;
  const char *kind = salt_stratum_pkg_kind(s);
  if (!kind) {
    salt_set_error("unknown package manager for stratum '%s'", s && s->name ? s->name : "?");
    return SALT_ERR;
  }
  if (n == 0) {
    if (status) *status = 0;
    return SALT_OK;
  }

  const char *base[6];
  int nbase = 0;
  if (strcmp(kind, "pacman") == 0) {
    base[nbase++] = "pacman";
    base[nbase++] = "-U";
    base[nbase++] = "--noconfirm";
  } else if (strcmp(kind, "apt") == 0) {
    base[nbase++] = "apt-get";
    base[nbase++] = "install";
    base[nbase++] = "-y";
    base[nbase++] = "--allow-downgrades";
  } else if (strcmp(kind, "apk") == 0) {
    base[nbase++] = "apk";
    base[nbase++] = "add";
  } else if (strcmp(kind, "dnf") == 0) {
    base[nbase++] = "dnf";
    base[nbase++] = "install";
    base[nbase++] = "-y";
  } else if (strcmp(kind, "zypper") == 0) {
    base[nbase++] = "zypper";
    base[nbase++] = "-n";
    base[nbase++] = "install";
    base[nbase++] = "--oldpackage";
  } else {
    base[nbase++] = "xbps-install";
    base[nbase++] = "-Sy";
  }

  salt_buf *specs = (salt_buf *)calloc(n, sizeof(salt_buf));
  char **argv = (char **)calloc((size_t)nbase + n + 1, sizeof(char *));
  if (!specs || !argv) {
    free(specs);
    free(argv);
    salt_set_error("out of memory building package command");
    return SALT_ERR;
  }
  int rc = SALT_OK;
  for (size_t i = 0; i < n && rc == SALT_OK; i++) {
    salt_buf_init(&specs[i]);
    if (strcmp(kind, "pacman") == 0) {
      /* pacman repositories only ever serve the current build, so a pinned
         version can only come from the stratum's own package cache; anything
         else would silently substitute whatever the mirror has today. */
      rc = pacman_cached_artifact(s, &pkgs[i], &specs[i]);
      if (rc != SALT_OK)
        salt_set_error("pacman cannot provide %s %s: not in %s/var/cache/pacman/pkg", pkgs[i].name,
                       pkgs[i].version, s->root ? s->root : "");
    } else {
      rc = salt_foreign_pkg_spec(kind, pkgs[i].name, pkgs[i].version, &specs[i]);
    }
  }
  if (rc == SALT_OK) {
    int k = 0;
    for (int i = 0; i < nbase; i++) argv[k++] = (char *)base[i];
    for (size_t i = 0; i < n; i++) argv[k++] = specs[i].data;
    argv[k] = NULL;
    rc = pkg_run(s, argv, status);
  }
  for (size_t i = 0; i < n; i++) salt_buf_free(&specs[i]);
  free(specs);
  free(argv);
  return rc;
}

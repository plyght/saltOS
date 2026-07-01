#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/stratum.h"
#include "salt/run.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/stat.h>

// Universal user accounts: merge human identities (uid/gid in [1000,65534))
// across the host and every stratum by APPENDING missing entries to the real
// passwd/shadow/group files -- never overwriting, never bind-mounting. System
// users (uid<1000) are distro-owned and are never copied, so each distro's own
// pacman/apk service accounts (alpm, http, ...) stay intact. Idempotent: an
// entry already present by name is skipped.
static bool is_human_id(const std::string &field) {
  if (field.empty()) return false;
  char *end = nullptr;
  long v = strtol(field.c_str(), &end, 10);
  if (end == field.c_str() || (end && *end != '\0')) return false;
  return v >= 1000 && v < 65534;
}

static std::string first_field(const std::string &line) {
  size_t p = line.find(':');
  return p == std::string::npos ? std::string() : line.substr(0, p);
}

static std::string nth_field(const std::string &line, int n) {
  size_t start = 0;
  for (int i = 0; i < n; i++) {
    size_t p = line.find(':', start);
    if (p == std::string::npos) return std::string();
    start = p + 1;
  }
  size_t e = line.find(':', start);
  return line.substr(start, e == std::string::npos ? std::string::npos : e - start);
}

// Collect the "name" (first field) of every line in <path>, so we can skip
// already-present entries. Missing file -> empty set (helper stays robust).
static std::set<std::string> collect_names(const std::string &path) {
  std::set<std::string> names;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::string n = first_field(line);
    if (!n.empty()) names.insert(n);
  }
  return names;
}

// Append `text` (already newline-terminated lines joined) to `path`, first
// ensuring the file ends in a newline so we never glue onto a truncated last
// line. Preserves the file's intended mode.
static void append_lines(const std::string &path, const std::string &text, mode_t mode) {
  if (text.empty()) return;
  // Ensure trailing newline on the existing file.
  struct stat stbuf;
  if (stat(path.c_str(), &stbuf) == 0 && stbuf.st_size > 0) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(-1, std::ios::end);
    char last = 0;
    in.get(last);
    in.close();
    if (last != '\n') {
      std::ofstream fix(path, std::ios::binary | std::ios::app);
      fix << '\n';
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return;
  out << text;
  out.close();
  chmod(path.c_str(), mode);
}

static void append_missing_identity(const std::string &src_etc, const std::string &dst_etc) {
  const std::string src_passwd = src_etc + "/passwd";
  const std::string dst_passwd = dst_etc + "/passwd";
  std::ifstream pin(src_passwd);
  if (!pin) return;  // no source passwd -> nothing to do

  std::set<std::string> human_names;  // human users found in src passwd
  std::set<std::string> dst_names = collect_names(dst_passwd);
  std::string add_passwd;
  std::string line;
  while (std::getline(pin, line)) {
    if (line.empty()) continue;
    std::string name = first_field(line);
    std::string uid = nth_field(line, 2);
    if (name.empty() || !is_human_id(uid)) continue;  // system users never copied
    human_names.insert(name);
    if (dst_names.find(name) == dst_names.end())
      add_passwd += line + "\n";
  }
  pin.close();
  append_lines(dst_passwd, add_passwd, 0644);

  // shadow: only lines whose name is in the human set collected above.
  const std::string src_shadow = src_etc + "/shadow";
  const std::string dst_shadow = dst_etc + "/shadow";
  std::ifstream sin(src_shadow);
  if (sin) {
    std::set<std::string> dst_sh = collect_names(dst_shadow);
    std::string add_shadow;
    while (std::getline(sin, line)) {
      if (line.empty()) continue;
      std::string name = first_field(line);
      if (name.empty()) continue;
      if (human_names.find(name) == human_names.end()) continue;
      if (dst_sh.find(name) != dst_sh.end()) continue;
      add_shadow += line + "\n";
    }
    sin.close();
    append_lines(dst_shadow, add_shadow, 0600);
  }

  // group: human = gid (field[2]) in [1000,65534).
  const std::string src_group = src_etc + "/group";
  const std::string dst_group = dst_etc + "/group";
  std::ifstream gin(src_group);
  if (gin) {
    std::set<std::string> dst_g = collect_names(dst_group);
    std::string add_group;
    while (std::getline(gin, line)) {
      if (line.empty()) continue;
      std::string name = first_field(line);
      std::string gid = nth_field(line, 2);
      if (name.empty() || !is_human_id(gid)) continue;
      if (dst_g.find(name) != dst_g.end()) continue;
      add_group += line + "\n";
    }
    gin.close();
    append_lines(dst_group, add_group, 0644);
  }
}

// Change-fingerprint of a file (nanosecond mtime folded with size), or 0 if
// unavailable -- used to detect that a run created/modified users so we know to
// propagate them back out. Nanosecond precision + size catches a useradd that
// completes within the same wall-clock second as the pre-run sample.
static unsigned long long file_fingerprint(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  unsigned long long f = (unsigned long long)st.st_mtime * 1000000000ULL;
#if defined(__linux__)
  f += (unsigned long long)st.st_mtim.tv_nsec;
#endif
  f ^= ((unsigned long long)st.st_size << 1) + 1;
  return f;
}

// Build the host /etc path and a given stratum's /etc path, mirroring how run.c
// derives the stratum root under o.root.
static std::string host_etc_path(const Options &o) {
  if (o.root == "/") return "/etc";
  char *j = salt_join_path(o.root.c_str(), "etc");
  std::string r = j ? j : "/etc";
  free(j);
  return r;
}

static std::string stratum_etc_path(const Options &o, const char *stratum_root) {
  std::string base = (o.root == "/" ? std::string() : o.root) + stratum_root;
  char *j = salt_join_path(base.c_str(), "etc");
  std::string r = j ? j : (base + "/etc");
  free(j);
  return r;
}

// Kernel denied unprivileged user namespaces (salt_run_child exit 126). Fall
// back to the transparent-sudo path so `run` still works -- sudo sets up the
// stratum as root and salt drops back to the caller. Re-execs; only returns on
// failure to exec sudo.
static void run_fallback_sudo(const Options &o, const std::vector<std::string> &args) {
  if (geteuid() == 0) return;  // already root, nothing to escalate
  std::vector<char *> a;
  a.push_back(const_cast<char *>("sudo"));
  a.push_back(const_cast<char *>("-n"));
  a.push_back(const_cast<char *>("salt"));
  if (o.root != "/") {
    a.push_back(const_cast<char *>("--root"));
    a.push_back(const_cast<char *>(o.root.c_str()));
  }
  a.push_back(const_cast<char *>("run"));
  for (auto &t : args) a.push_back(const_cast<char *>(t.c_str()));
  a.push_back(nullptr);
  execvp("sudo", a.data());
}

int cmd_run(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt run <stratum> <cmd> [args...]\n");
    return 2;
  }
  // Read-only open: `run` only needs to look up the stratum's root, and an
  // unprivileged user must be able to read the root-owned DB without writing it.
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open_ro(o.root.c_str(), &db) != SALT_OK &&
      salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, args[0].c_str(), &s) != SALT_OK) {
    fprintf(stderr, "salt: unknown stratum '%s' (try 'salt stratum list')\n", args[0].c_str());
    salt_strata_db_close(db);
    return 1;
  }
  salt_strata_db_close(db);  // done reading; the run child needs no DB handle

  std::vector<std::string> hold(args.begin() + 1, args.end());
  // Allow the conventional `salt run <stratum> -- <cmd>` separator: drop a single
  // leading "--" so the command isn't exec'd as the literal program "--".
  if (!hold.empty() && hold.front() == "--") hold.erase(hold.begin());
  std::vector<char *> argv;
  for (auto &t : hold) argv.push_back(const_cast<char *>(t.c_str()));
  argv.push_back(nullptr);

  salt_run_opts opts;
  salt_run_opts_default(&opts);
  // Run in the caller's current directory (bound into the stratum by run.c) so
  // `cd ~/code/app && bun dev` behaves like a normal shell instead of starting
  // in $HOME. Falls back to the default (home) if cwd is somehow unavailable.
  char cwd[4096];
  // If we were re-exec'd by salt_escape_to_host (running a stratum command from
  // inside another stratum), our getcwd() no longer reflects the caller's logical
  // directory -- nsenter left us at a path under the wrong root. The escape stashed
  // the caller's logical cwd in SALT_ESCAPE_CWD; prefer it. Unset it immediately so
  // it doesn't leak into the run child or any nested `salt run` beneath it, which
  // must derive their own cwd. Fall back to getcwd() for the normal (non-escaped)
  // case.
  const char *escaped_cwd = getenv("SALT_ESCAPE_CWD");
  if (escaped_cwd && escaped_cwd[0] == '/') {
    snprintf(cwd, sizeof(cwd), "%s", escaped_cwd);
    unsetenv("SALT_ESCAPE_CWD");  // copied above; don't leak it to children
    opts.workdir = cwd;
  } else if (getcwd(cwd, sizeof(cwd))) {
    opts.workdir = cwd;
  }
  // Universal user accounts (root only): before running, push host humans into
  // this stratum so it sees your accounts. Fingerprint the stratum identity
  // files; if the run changes them (useradd/passwd), pull new humans back to
  // the host and fan them out to every other stratum. Purely additive file
  // appends -- no bind mounts, so distro system users are untouched.
  std::string host_etc, strat_etc;
  unsigned long long m_passwd = 0, m_shadow = 0, m_group = 0;
  bool do_identity = (geteuid() == 0 && s.root && s.root[0]);
  if (do_identity) {
    host_etc = host_etc_path(o);
    strat_etc = stratum_etc_path(o, s.root);
    append_missing_identity(host_etc, strat_etc);
    m_passwd = file_fingerprint(strat_etc + "/passwd");
    m_shadow = file_fingerprint(strat_etc + "/shadow");
    m_group = file_fingerprint(strat_etc + "/group");
  }

  int st = 0;
  int rc = salt_stratum_run(&s, &opts, argv.data(), &st);

  if (do_identity && rc == SALT_OK) {
    bool changed = file_fingerprint(strat_etc + "/passwd") != m_passwd ||
                   file_fingerprint(strat_etc + "/shadow") != m_shadow ||
                   file_fingerprint(strat_etc + "/group") != m_group;
    if (changed) {
      // New humans created in this stratum -> host, then every other stratum.
      append_missing_identity(strat_etc, host_etc);
      salt_strata_db *db2 = nullptr;
      if (salt_strata_db_open_ro(o.root.c_str(), &db2) == SALT_OK ||
          salt_strata_db_open(o.root.c_str(), &db2) == SALT_OK) {
        salt_stratum_list all;
        salt_stratum_list_init(&all);
        if (salt_stratum_list_all(db2, &all) == SALT_OK) {
          for (size_t i = 0; i < all.len; i++) {
            const salt_stratum *os = &all.items[i];
            if (!os->name || !os->root || !os->root[0]) continue;
            if (os->name && s.name && strcmp(os->name, s.name) == 0) continue;
            append_missing_identity(host_etc, stratum_etc_path(o, os->root));
          }
        }
        salt_stratum_list_free(&all);
        salt_strata_db_close(db2);
      }
    }
  }

  salt_stratum_free_fields(&s);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  // Unprivileged user namespaces unavailable; retry transparently w/ sudo. This
  // is signaled out-of-band (not a real exit code), so a command that genuinely
  // exits 126 is passed through untouched instead of being re-run under sudo.
  if (st == SALT_RUN_USERNS_DENIED) {
    run_fallback_sudo(o, args);
    fprintf(stderr, "salt run: needs unprivileged user namespaces or sudo "
                    "(could not escalate)\n");
    return 126;
  }
  // A command may have *installed* new binaries into the stratum (npm i -g,
  // pip install, cargo install, make install, anything). Re-expose so they
  // become host commands immediately -- no package-manager list, no manual
  // `salt expose`. This is best-effort: expose_all_for opens the strata DB for
  // write and silently returns if it can't (the common unprivileged path), so
  // it only does real work when we had the privilege to change the stratum --
  // exactly the case where something could have been installed. expose_all is
  // idempotent and prints only when it actually adds commands.
  if (st == 0 && expose_all_enabled(o)) expose_all_for(o, args[0]);
  return st;
}

int cmd_pkg(const Options &o, const std::vector<std::string> &args) {
  if (args.size() < 2) {
    fprintf(stderr, "usage: salt pkg <stratum> <install|remove|update|search> [pkg...]\n");
    return 2;
  }
  const std::string &name = args[0];
  const std::string &op = args[1];

  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return 1;
  }
  salt_strata_ctx c;
  salt_strata_ctx_init(&c, o.root.c_str());

  salt_stratum s;
  memset(&s, 0, sizeof(s));
  if (salt_stratum_get(db, name.c_str(), &s) != SALT_OK) {
    fprintf(stderr, "salt: unknown stratum '%s' (try 'salt stratum list')\n", name.c_str());
    salt_strata_ctx_free(&c);
    salt_strata_db_close(db);
    return 1;
  }

  if (op != "search") {
    std::string label = "pre-" + op;
    if (salt_stratum_snapshot_create(&c, db, name.c_str(), label.c_str(), nullptr) != SALT_OK)
      fprintf(stderr, "warning: could not take safety snapshot: %s\n", salt_last_error());
  }

  std::vector<std::string> hold(args.begin() + 2, args.end());
  std::vector<char *> pkgs;
  for (auto &t : hold) pkgs.push_back(const_cast<char *>(t.c_str()));
  int npkgs = (int)hold.size();

  int st = 0;
  int rc = salt_stratum_pkg(&s, op.c_str(), pkgs.empty() ? nullptr : pkgs.data(), npkgs, &st);
  if (rc != SALT_OK) fprintf(stderr, "salt: %s\n", salt_last_error());

  salt_stratum_free_fields(&s);
  salt_strata_ctx_free(&c);
  salt_strata_db_close(db);
  return rc != SALT_OK ? 1 : st;
}

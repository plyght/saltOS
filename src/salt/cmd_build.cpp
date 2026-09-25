#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/toml.h"
#include "salt/pkg.h"
#include "salt/archive.h"
#include "salt/hash.h"
#include "salt/repo.h"
#include "salt/trust.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

static const char *sev_name(salt_risk_severity s) {
  switch (s) {
    case SALT_RISK_BLOCK:
      return "BLOCK";
    case SALT_RISK_WARN:
      return "WARN";
    default:
      return "INFO";
  }
}

static int print_findings(const char *title, salt_findings *f) {
  printf("%s:\n", title);
  if (f->len == 0) {
    printf("  (no findings)\n");
    return 0;
  }
  for (size_t i = 0; i < f->len; i++)
    printf("  [%-5s] %s: %s\n", sev_name(f->items[i].severity), f->items[i].code,
           f->items[i].message);
  return salt_findings_has_block(f) ? 1 : 0;
}

int cmd_lint(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt lint <recipe-dir>\n");
    return 2;
  }
  salt_findings lf;
  salt_findings_init(&lf);
  salt_recipe_lint(args[0].c_str(), &lf);
  int blocked = print_findings("lint", &lf);
  salt_findings_free(&lf);

  salt_findings sf;
  salt_findings_init(&sf);
  salt_scan_input in;
  memset(&in, 0, sizeof(in));
  in.recipe_path = (char *)args[0].c_str();
  in.author_level = SALT_TRUST_UNKNOWN;
  std::string td = trustdb_for(o);
  const char *author = getenv("SALT_AUTHOR");
  if (author) in.author_level = salt_trust_lookup(td.c_str(), author);
  salt_supplychain_scan(&in, &sf);
  blocked |= print_findings("supply-chain", &sf);
  salt_findings_free(&sf);
  return blocked ? 1 : 0;
}

static bool is_sha256_hex(const std::string &s) {
  if (s.size() != SALT_SHA256_HEXLEN) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

static std::string recipe_file(const std::string &dir) {
  std::string p = dir;
  if (salt_is_dir(dir.c_str())) p = path_join(dir, "recipe.toml");
  return p;
}

static bool chroot_path(const std::string &root, const std::string &abs, std::string &out) {
  std::string r = root;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  if (abs.compare(0, r.size(), r) != 0 || abs.size() <= r.size() || abs[r.size()] != '/')
    return false;
  out = abs.substr(r.size());
  return true;
}

static int run_shell(const std::string &workdir, const std::string &script,
                     const std::vector<std::string> &env, const std::string &root) {
  std::string tmp = path_join(workdir, ".salt-build.sh");
  std::string full = "set -e\n" + script + "\n";
  if (salt_write_file(tmp.c_str(), full.c_str(), full.size(), 0755) != SALT_OK) return SALT_ERR;
  std::string cmd;
  for (auto &e : env) cmd += e + " ";
  cmd += "sh -e .salt-build.sh";
  std::string run;
  if (root.empty()) {
    run = "cd '" + workdir + "' && " + cmd;
  } else {
    std::string inner;
    if (!chroot_path(root, workdir, inner)) return SALT_ERR;
    run = "chroot '" + root + "' /bin/sh -c \"cd '" + inner + "' && " + cmd + "\"";
  }
  int rc = system(run.c_str());
  return rc == 0 ? SALT_OK : SALT_ERR;
}

/* Post-install payload pass, run on the host over $SALT_DEST.
 *
 * Hardlinks: the grain format stores regular files and symlinks only, so every
 * extra link to an inode would be packaged as a full copy (git's ~150
 * hardlinked builtins alone were 2.6 GB). Keep the first path of each inode and
 * turn the rest into relative symlinks to it.
 *
 * Strip: ELF files lose their debug info. Executables and shared objects get
 * --strip-unneeded; the dynamic loader, libc, libpthread and libthread_db,
 * static archives and objects only get --strip-debug (debuggers and the
 * dynamic loader need their symbol tables). Kernel modules and firmware are
 * left alone. $SALT_STRIP selects the strip binary; a file strip cannot handle
 * (another architecture, an odd ELF) is left as it is. */
static const char *kFinalizePayload = R"SH(
set -u
cd "$SALT_DEST" || exit 1
find . -type f -links +1 -printf '%i %p\n' | sort -k1,1n -k2 | while read -r ino path; do
  if [ "$ino" = "${seen:-}" ]; then
    rm -f "$path" && ln -sr "$first" "$path"
  else
    seen=$ino first=$path
  fi
done
[ "$DO_STRIP" = 1 ] || exit 0
command -v "$STRIP" >/dev/null 2>&1 || { echo "salt: $STRIP not found; payload left unstripped" >&2; exit 0; }
find . -type f ! -path './usr/lib/modules/*' ! -path './lib/modules/*' \
  ! -path './usr/lib/firmware/*' ! -path './lib/firmware/*' ! -path './usr/lib/debug/*' |
while read -r f; do
  magic=$(head -c 4 "$f" 2>/dev/null | od -An -c | tr -d ' ')
  case "$f" in
    *.a) [ "$(head -c 7 "$f" 2>/dev/null)" = '!<arch>' ] || continue; mode=--strip-debug ;;
    *) [ "$magic" = '177ELF' ] || continue
       case "${f##*/}" in
         ld-linux*|ld-*.so*|libc.so*|libc-*.so|libpthread*|libthread_db*|*.o) mode=--strip-debug ;;
         *) mode=--strip-unneeded ;;
       esac ;;
  esac
  perm=$(stat -c %a "$f")
  chmod u+w "$f"
  "$STRIP" "$mode" "$f" 2>/dev/null || true
  chmod "$perm" "$f"
done
exit 0
)SH";

static int finalize_payload(const std::string &work, const std::string &dest, bool strip) {
  std::string tmp = path_join(work, ".salt-finalize.sh");
  if (salt_write_file(tmp.c_str(), kFinalizePayload, strlen(kFinalizePayload), 0755) != SALT_OK)
    return SALT_ERR;
  const char *s = getenv("SALT_STRIP");
  std::string cmd = "SALT_DEST='" + dest + "' STRIP='" + std::string(s && *s ? s : "strip") +
                    "' DO_STRIP=" + (strip ? "1" : "0") + " sh '" + tmp + "'";
  int rc = system(cmd.c_str());
  unlink(tmp.c_str());
  return rc == 0 ? SALT_OK : SALT_ERR;
}

static std::string default_build(const std::string &system) {
  if (system == "autotools")
    return "./configure --prefix=/usr\nmake -j\"$SALT_JOBS\"\nmake DESTDIR=\"$SALT_DEST\" install";
  if (system == "make")
    return "make -j\"$SALT_JOBS\"\nmake PREFIX=/usr DESTDIR=\"$SALT_DEST\" install";
  if (system == "cmake")
    return "cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib "
           "-DCMAKE_BUILD_TYPE=Release\ncmake --build "
           "build -j\"$SALT_JOBS\"\nDESTDIR=\"$SALT_DEST\" cmake --install build";
  if (system == "meson")
    return "meson setup build --prefix=/usr --libdir=lib --buildtype=release\nninja -C build "
           "-j\"$SALT_JOBS\"\nDESTDIR=\"$SALT_DEST\" "
           "ninja -C build install";
  return "";
}

int cmd_build(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    fprintf(stderr, "usage: salt build <recipe-dir>\n");
    return 2;
  }
  std::string rdir = args[0];
  std::string rfile = recipe_file(rdir);
  salt_toml *t = salt_toml_parse_file(rfile.c_str());
  if (!t) {
    fprintf(stderr, "salt: cannot parse %s: %s\n", rfile.c_str(), salt_last_error());
    return 1;
  }
  std::string arch = arch_detect();
  salt_strlist arches;
  salt_strlist_init(&arches);
  salt_toml_string_array(t, "arch", &arches);
  bool ok_arch = arches.len == 0;
  for (size_t i = 0; i < arches.len; i++)
    if (arch == arches.items[i] || strcmp(arches.items[i], "any") == 0) ok_arch = true;
  salt_strlist_free(&arches);
  if (!ok_arch) {
    fprintf(stderr, "salt: recipe does not target %s\n", arch.c_str());
    salt_toml_free(t);
    return 1;
  }

  std::string name = salt_toml_string(t, "name", "");
  std::string version = salt_toml_string(t, "version", "");
  int release = (int)salt_toml_int(t, "release", 1);
  std::string url = salt_toml_string(t, "source.url", "");
  std::string sha = salt_toml_string(t, "source.sha256", "");
  std::string build_system = salt_toml_string(t, "build.system", "custom");
  std::string script = salt_toml_string(t, "build.script", "");

  const char *workenv = getenv("SALT_WORK");
  std::string work = workenv ? std::string(workenv) + "/" + name : "work/" + name;
  std::string src = path_join(work, "src");
  std::string dest = path_join(work, "dest");
  std::string dl = path_join(work, "dl");
  salt_remove_recursive(work.c_str());
  salt_mkdirs(src.c_str(), 0755);
  salt_mkdirs(dest.c_str(), 0755);
  salt_mkdirs(dl.c_str(), 0755);
  std::string absdest = dest;
  std::string abssrc = src;
  {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      if (!dest.empty() && dest[0] != '/') absdest = std::string(cwd) + "/" + dest;
      if (!src.empty() && src[0] != '/') abssrc = std::string(cwd) + "/" + src;
    }
  }

  printf("==> building %s %s-%d for %s\n", name.c_str(), version.c_str(), release, arch.c_str());

  if (salt_is_dir(path_join(rdir, "scripts").c_str())) {
    fprintf(stderr,
            "salt: %s/scripts: undeclared install scripts are not allowed; declare them "
            "in [hooks]\n",
            rdir.c_str());
    salt_toml_free(t);
    return 1;
  }

  std::string localpath = url.rfind("file://", 0) == 0 ? url.substr(7) : "";
  bool local = !localpath.empty() && salt_is_dir(localpath.c_str());
  if (url.empty()) {
    fprintf(stderr, "salt: recipe has no source.url\n");
    salt_toml_free(t);
    return 1;
  }
  if (local) {
    if (!sha.empty())
      fprintf(stderr, "salt: warning: ignoring source.sha256 for local source directory %s\n",
              localpath.c_str());
  } else if (!is_sha256_hex(sha)) {
    fprintf(stderr, "salt: source.sha256 must be 64 lowercase hex digits, got '%s'\n", sha.c_str());
    salt_toml_free(t);
    return 1;
  }
  if (local) {
    std::string srcpath = localpath;
    std::string copy;
    if (salt_is_dir(path_join(srcpath, ".git").c_str()))
      copy = "cd '" + srcpath +
             "' && git -c 'safe.directory=*' ls-files -z --cached --others --exclude-standard | "
             "tar --null -T - -cf - | "
             "tar -C '" +
             abssrc + "' -xf -";
    else
      copy = "cp -a '" + srcpath + "/.' '" + src + "/'";
    if (system(copy.c_str()) != 0) {
      fprintf(stderr, "salt: failed to copy local source %s\n", srcpath.c_str());
      salt_toml_free(t);
      return 1;
    }
  } else {
    std::string tarball = path_join(dl, "source");
    printf("==> fetching %s\n", url.c_str());
    if (salt_fetch_to_file(url.c_str(), tarball.c_str()) != SALT_OK) {
      fprintf(stderr, "salt: fetch failed: %s\n", salt_last_error());
      salt_toml_free(t);
      return 1;
    }
    char hex[SALT_SHA256_HEXLEN + 1];
    if (salt_sha256_file(tarball.c_str(), hex) != SALT_OK || sha != hex) {
      fprintf(stderr, "salt: SOURCE HASH MISMATCH (expected %s, got %s)\n", sha.c_str(), hex);
      salt_toml_free(t);
      return 1;
    }
    printf("==> source hash verified\n");
    std::string ex = "tar -C '" + src + "' -xf '" + tarball + "' 2>/dev/null";
    if (system(ex.c_str()) != 0) {
      std::string base = url.substr(url.find_last_of('/') + 1);
      size_t q = base.find_first_of("?#");
      if (q != std::string::npos) base.erase(q);
      if (base.empty()) base = "source";
      std::string cp = "cp -f '" + tarball + "' '" + path_join(src, base) + "'";
      if (system(cp.c_str()) != 0) {
        fprintf(stderr, "salt: failed to place source file %s\n", base.c_str());
        salt_toml_free(t);
        return 1;
      }
    }
    std::string strip = "set -- '" + src +
                        "'/*; if [ $# -eq 1 ] && [ -d \"$1\" ]; then mv \"$1\"/* \"$1\"/.[!.]* '" +
                        src + "'/ 2>/dev/null; rmdir \"$1\" 2>/dev/null; fi";
    system(strip.c_str());
  }

  std::string body = script.empty() ? default_build(build_system) : script;
  if (body.empty()) {
    fprintf(stderr, "salt: no build.script and unknown build.system '%s'\n", build_system.c_str());
    salt_toml_free(t);
    return 1;
  }
  std::vector<std::string> env;
  const char *rootenv = getenv("SALT_BUILD_ROOT");
  std::string root = rootenv ? rootenv : "";
  std::string envsrc = abssrc, envdest = absdest;
  if (!root.empty() &&
      (!chroot_path(root, abssrc, envsrc) || !chroot_path(root, absdest, envdest))) {
    fprintf(stderr, "salt: SALT_WORK (%s) must lie inside SALT_BUILD_ROOT (%s)\n", work.c_str(),
            root.c_str());
    salt_toml_free(t);
    return 1;
  }
  env.push_back("SALT_SRC='" + envsrc + "'");
  env.push_back("SALT_DEST='" + envdest + "'");
  env.push_back("SALT_ARCH='" + arch + "'");
  env.push_back("SALT_JOBS=" + std::string(getenv("SALT_JOBS") ? getenv("SALT_JOBS") : "4"));
  env.push_back("SALT_NO_NETWORK=1");
  printf("==> running build (%s%s)\n", build_system.c_str(), root.empty() ? "" : ", chrooted");
  if (run_shell(abssrc, body, env, root) != SALT_OK) {
    fprintf(stderr, "salt: build failed\n");
    salt_toml_free(t);
    return 1;
  }
  unlink(path_join(dest, "usr/share/info/dir").c_str());
  bool do_strip = salt_toml_bool(t, "build.strip", true);
  printf("==> finalizing payload (%s)\n", do_strip ? "hardlinks, strip" : "hardlinks, no strip");
  if (finalize_payload(work, absdest, do_strip) != SALT_OK) {
    fprintf(stderr, "salt: payload finalization failed\n");
    salt_toml_free(t);
    return 1;
  }

  salt_pkg_meta meta;
  salt_pkg_meta_init(&meta);
  meta.name = salt_strdup(name.c_str());
  meta.version = salt_strdup(version.c_str());
  meta.release = release;
  meta.arch = salt_strdup(arch.c_str());
  meta.summary = salt_strdup(salt_toml_string(t, "summary", ""));
  meta.license = salt_strdup(salt_toml_string(t, "license", ""));
  meta.repro_status = salt_strdup(salt_toml_string(t, "reproducibility.status", "unverified"));
  const char *reason = salt_toml_string(t, "reproducibility.reason", nullptr);
  meta.repro_reason = reason ? salt_strdup(reason) : nullptr;
  salt_toml_string_array(t, "package.deps", &meta.deps);
  salt_toml_string_array(t, "package.conflicts", &meta.conflicts);

  const salt_toml *hooks = salt_toml_get(t, "hooks");
  for (size_t i = 0; hooks && i < salt_toml_table_len(hooks); i++) {
    const char *key = salt_toml_table_key(hooks, i);
    int kind = salt_hook_from_name(key);
    const char *hbody = salt_toml_as_string(salt_toml_table_val(hooks, i));
    if (kind < 0 || !hbody) {
      fprintf(stderr,
              "salt: [hooks] %s is not a declared hook (post_install, post_upgrade, pre_remove, "
              "post_remove) or not a string\n",
              key ? key : "?");
      salt_pkg_meta_free(&meta);
      salt_toml_free(t);
      return 1;
    }
    meta.hooks[kind] = salt_strdup(hbody);
    printf("==> declared %s hook\n", key);
  }

  salt_archive ar;
  int rc = salt_archive_build_from_dir(dest.c_str(), &meta, nullptr, &ar);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: packaging failed: %s\n", salt_last_error());
    salt_pkg_meta_free(&meta);
    salt_toml_free(t);
    return 1;
  }
  char *fn = salt_pkg_filename(&meta);
  const char *outenv = getenv("SALT_OUT");
  std::string outdir = (outenv ? std::string(outenv) : "out") + "/" + arch + "/packages";
  salt_mkdirs(outdir.c_str(), 0755);
  std::string outpath = path_join(outdir, fn);
  rc = salt_archive_write(&ar, outpath.c_str());
  if (rc == SALT_OK) printf("==> wrote %s (%zu files)\n", outpath.c_str(), ar.manifest.len);
  free(fn);
  salt_archive_free(&ar);
  salt_pkg_meta_free(&meta);
  salt_toml_free(t);
  return rc == SALT_OK ? 0 : 1;
}

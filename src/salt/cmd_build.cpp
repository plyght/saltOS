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
    case SALT_RISK_BLOCK: return "BLOCK";
    case SALT_RISK_WARN: return "WARN";
    default: return "INFO";
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

static std::string recipe_file(const std::string &dir) {
  std::string p = dir;
  if (salt_is_dir(dir.c_str())) p = path_join(dir, "recipe.toml");
  return p;
}

static int run_shell(const std::string &workdir, const std::string &script,
                     const std::vector<std::string> &env) {
  std::string tmp = path_join(workdir, ".salt-build.sh");
  std::string full = "set -e\n" + script + "\n";
  if (salt_write_file(tmp.c_str(), full.c_str(), full.size(), 0755) != SALT_OK) return SALT_ERR;
  std::string cmd;
  for (auto &e : env) cmd += e + " ";
  cmd += "sh -e .salt-build.sh";
  std::string run = "cd '" + workdir + "' && " + cmd;
  int rc = system(run.c_str());
  return rc == 0 ? SALT_OK : SALT_ERR;
}

static std::string default_build(const std::string &system) {
  if (system == "autotools")
    return "./configure --prefix=/usr\nmake -j\"$SALT_JOBS\"\nmake DESTDIR=\"$SALT_DEST\" install";
  if (system == "make")
    return "make -j\"$SALT_JOBS\"\nmake PREFIX=/usr DESTDIR=\"$SALT_DEST\" install";
  if (system == "cmake")
    return "cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release\ncmake --build "
           "build -j\"$SALT_JOBS\"\nDESTDIR=\"$SALT_DEST\" cmake --install build";
  if (system == "meson")
    return "meson setup build --prefix=/usr\nninja -C build -j\"$SALT_JOBS\"\nDESTDIR=\"$SALT_DEST\" "
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
  std::string system = salt_toml_string(t, "build.system", "custom");
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

  printf("==> building %s %s-%d for %s\n", name.c_str(), version.c_str(), release, arch.c_str());

  std::string localpath = url.rfind("file://", 0) == 0 ? url.substr(7) : "";
  bool local = !localpath.empty() && salt_is_dir(localpath.c_str());
  if (local) {
    std::string srcpath = localpath;
    std::string copy = "cp -a '" + srcpath + "/.' '" + src + "/'";
    if (::system(copy.c_str()) != 0) {
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
    if (!sha.empty() && sha != "TODO-sha256") {
      char hex[SALT_SHA256_HEXLEN + 1];
      if (salt_sha256_file(tarball.c_str(), hex) != SALT_OK || sha != hex) {
        fprintf(stderr, "salt: SOURCE HASH MISMATCH (expected %s, got %s)\n", sha.c_str(), hex);
        salt_toml_free(t);
        return 1;
      }
      printf("==> source hash verified\n");
    } else {
      fprintf(stderr, "warning: source hash not verified (%s)\n",
              sha.empty() ? "missing" : "placeholder");
    }
    std::string ex = "tar -C '" + src + "' -xf '" + tarball + "' 2>/dev/null || true";
    ::system(ex.c_str());
    std::string strip = "set -- '" + src +
                        "'/*; if [ $# -eq 1 ] && [ -d \"$1\" ]; then mv \"$1\"/* \"$1\"/.[!.]* '" +
                        src + "'/ 2>/dev/null; rmdir \"$1\" 2>/dev/null; fi";
    ::system(strip.c_str());
  }

  std::string body = script.empty() ? default_build(system) : script;
  if (body.empty()) {
    fprintf(stderr, "salt: no build.script and unknown build.system '%s'\n", system.c_str());
    salt_toml_free(t);
    return 1;
  }
  std::vector<std::string> env;
  std::string absdest = dest;
  std::string abssrc = src;
  {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      if (!dest.empty() && dest[0] != '/') absdest = std::string(cwd) + "/" + dest;
      if (!src.empty() && src[0] != '/') abssrc = std::string(cwd) + "/" + src;
    }
  }
  env.push_back("SALT_SRC='" + abssrc + "'");
  env.push_back("SALT_DEST='" + absdest + "'");
  env.push_back("SALT_ARCH='" + arch + "'");
  env.push_back("SALT_JOBS=" + std::string(getenv("SALT_JOBS") ? getenv("SALT_JOBS") : "4"));
  env.push_back("SALT_NO_NETWORK=1");
  printf("==> running build (%s)\n", system.c_str());
  if (run_shell(src, body, env) != SALT_OK) {
    fprintf(stderr, "salt: build failed\n");
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

  std::string scripts_dir = path_join(rdir, "scripts");
  salt_archive ar;
  int rc = salt_archive_build_from_dir(dest.c_str(), &meta,
                                       salt_is_dir(scripts_dir.c_str()) ? scripts_dir.c_str()
                                                                        : nullptr,
                                       &ar);
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

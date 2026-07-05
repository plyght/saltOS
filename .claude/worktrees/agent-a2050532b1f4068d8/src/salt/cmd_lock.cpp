#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/repo.h"
#include "salt/hash.h"
#include "salt/stratum.h"
}

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

static std::string iso_now() {
  time_t t = time(nullptr);
  struct tm g{};
  gmtime_r(&t, &g);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
  return std::string(buf);
}

static std::string system_lock_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.lock.toml");
}

static std::string system_config_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.toml");
}

static void emit_native(const Options &o, salt_buf *out, int *count) {
  salt_db *db = nullptr;
  if (salt_db_open(db_path_for(o).c_str(), &db) != SALT_OK) return;

  salt_repo_index idx;
  bool have_index = salt_repo_index_load(index_path_for(o).c_str(), &idx) == SALT_OK;

  char index_hash[SALT_SHA256_HEXLEN + 1] = {0};
  bool have_index_hash = salt_sha256_file(index_path_for(o).c_str(), index_hash) == SALT_OK;

  salt_db_pkglist installed;
  salt_db_pkglist_init(&installed);
  salt_db_list_installed(db, &installed);

  for (size_t i = 0; i < installed.len; i++) {
    const salt_db_pkg &p = installed.items[i];
    const salt_repo_entry *e = have_index ? salt_repo_index_find(&idx, p.name) : nullptr;

    salt_buf_printf(out, "[[native]]\nname = \"%s\"\n", p.name);
    salt_buf_printf(out, "version = \"%s\"\nrelease = %d\n", p.version ? p.version : "", p.release);
    salt_buf_printf(out, "arch = \"%s\"\n", p.arch ? p.arch : "");
    if (e && e->sha256)
      salt_buf_printf(out, "grain_sha256 = \"sha256:%s\"\n", e->sha256);
    salt_buf_printf(out, "repo = \"%s\"\n", p.repo ? p.repo : "current");
    if (have_index_hash)
      salt_buf_printf(out, "repo_index_sha256 = \"sha256:%s\"\n", index_hash);
    if (!e)
      salt_buf_printf(out, "coverage = \"partial\"\nreason = \"not present in the local signed index\"\n");
    if (e && e->deps.len) {
      salt_buf_append_str(out, "deps = [");
      for (size_t d = 0; d < e->deps.len; d++)
        salt_buf_printf(out, "%s\"%s\"", d ? ", " : "", e->deps.items[d]);
      salt_buf_append_str(out, "]\n");
    }
    salt_buf_append_str(out, "\n");
    (*count)++;
  }

  salt_db_pkglist_free(&installed);
  if (have_index) salt_repo_index_free(&idx);
  salt_db_close(db);
}

static void emit_strata(const Options &o, salt_buf *out, int *count) {
  salt_strata_db *db = nullptr;
  if (salt_strata_db_open(o.root.c_str(), &db) != SALT_OK) return;

  salt_stratum_list list;
  salt_stratum_list_init(&list);
  if (salt_stratum_list_all(db, &list) != SALT_OK) {
    salt_stratum_list_free(&list);
    salt_strata_db_close(db);
    return;
  }

  for (size_t i = 0; i < list.len; i++) {
    const salt_stratum &s = list.items[i];
    salt_buf_printf(out, "[[stratum]]\nname = \"%s\"\n", s.name ? s.name : "");
    if (s.family) salt_buf_printf(out, "family = \"%s\"\n", s.family);
    if (s.package_manager) salt_buf_printf(out, "package_manager = \"%s\"\n", s.package_manager);

    std::string rp = resolve_stratum_recipe(o, s.name ? s.name : "");
    if (!rp.empty()) {
      salt_stratum_recipe r;
      salt_stratum_recipe_init(&r);
      if (salt_stratum_recipe_load(rp.c_str(), &r) == SALT_OK) {
        if (r.rootfs_url && r.rootfs_url[0])
          salt_buf_printf(out, "bootstrap_url = \"%s\"\n", r.rootfs_url);
        if (r.rootfs_sha256 && r.rootfs_sha256[0])
          salt_buf_printf(out, "bootstrap_sha256 = \"%s\"\n", r.rootfs_sha256);
        for (size_t u = 0; u < r.repo_urls.len; u++)
          salt_buf_printf(out, "repo_snapshot = \"%s\"\n", r.repo_urls.items[u]);
      }
      salt_stratum_recipe_free(&r);
    }
    salt_buf_append_str(out,
        "coverage = \"partial\"\nreason = \"stratum identity and bootstrap pinned; "
        "per-package capture needs a capturing stratum-run\"\n\n");
    (*count)++;
  }

  salt_stratum_list_free(&list);
  salt_strata_db_close(db);
}

int cmd_lock(const Options &o, const std::vector<std::string> &args) {
  std::string out_path = system_lock_path(o);
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--output" && i + 1 < args.size())
      out_path = args[++i];
    else if (args[i] == "--update")
      continue;
    else {
      fprintf(stderr, "usage: salt lock [--output <path>] [--update]\n");
      return 2;
    }
  }

  salt_buf lock;
  salt_buf_init(&lock);
  salt_buf_printf(&lock, "schema = 1\ngenerated = \"%s\"\n", iso_now().c_str());

  char cfg_hash[SALT_SHA256_HEXLEN + 1] = {0};
  if (salt_sha256_file(system_config_path(o).c_str(), cfg_hash) == SALT_OK)
    salt_buf_printf(&lock, "config_hash = \"sha256:%s\"\n", cfg_hash);
  salt_buf_append_str(&lock, "\n");

  int native = 0, strata = 0;
  emit_native(o, &lock, &native);
  emit_strata(o, &lock, &strata);

  std::string dir = out_path.substr(0, out_path.find_last_of('/'));
  if (!dir.empty()) salt_mkdirs(dir.c_str(), 0755);
  if (salt_write_file(out_path.c_str(), lock.data, lock.len, 0644) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_buf_free(&lock);
    return 1;
  }
  salt_buf_free(&lock);

  printf("locked %d native package(s), %d stratum(s) -> %s\n", native, strata, out_path.c_str());
  return 0;
}

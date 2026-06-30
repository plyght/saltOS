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

#ifdef __cplusplus
}
#endif

#endif

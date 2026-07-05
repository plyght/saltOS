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

void salt_run_opts_default(salt_run_opts *o);

int salt_stratum_run(const salt_stratum *s, const salt_run_opts *opts, char *const argv[],
                     int *status);

int salt_stratum_pkg(const salt_stratum *s, const char *op, char *const pkgs[], int npkgs,
                     int *status);

#ifdef __cplusplus
}
#endif

#endif

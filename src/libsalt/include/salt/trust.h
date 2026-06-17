#ifndef SALT_TRUST_H
#define SALT_TRUST_H

#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SALT_TRUST_UNKNOWN = 0,
  SALT_TRUST_VOUCHED = 1,
  SALT_TRUST_MAINTAINER = 2,
  SALT_TRUST_DENOUNCED = -1,
} salt_trust_level;

const char *salt_trust_level_name(salt_trust_level lvl);
salt_trust_level salt_trust_level_parse(const char *s);

typedef enum {
  SALT_RISK_INFO,
  SALT_RISK_WARN,
  SALT_RISK_BLOCK,
} salt_risk_severity;

typedef struct {
  salt_risk_severity severity;
  char *code;
  char *message;
} salt_finding;

typedef struct {
  salt_finding *items;
  size_t len;
  size_t cap;
} salt_findings;

void salt_findings_init(salt_findings *f);
int salt_findings_push(salt_findings *f, salt_risk_severity sev, const char *code, const char *msg);
void salt_findings_free(salt_findings *f);
bool salt_findings_has_block(const salt_findings *f);

typedef struct {
  char *recipe_path;
  const char *prev_recipe_text;
  salt_trust_level author_level;
} salt_scan_input;

int salt_recipe_lint(const char *recipe_path, salt_findings *out);
int salt_recipe_admission_check(const char *recipe_path, salt_findings *out);
int salt_supplychain_scan(const salt_scan_input *in, salt_findings *out);

salt_trust_level salt_trust_lookup(const char *trustdb_path, const char *contributor);
int salt_trust_set(const char *trustdb_path, const char *contributor, salt_trust_level lvl,
                   const char *by, const char *reason);
int salt_trust_vouch(const char *trustdb_path, const char *voucher, const char *contributor,
                     const char *reason);

#ifdef __cplusplus
}
#endif

#endif

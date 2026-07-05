#include "salt/trust.h"
#include "salt/toml.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <time.h>

const char *salt_trust_level_name(salt_trust_level lvl) {
  switch (lvl) {
    case SALT_TRUST_MAINTAINER: return "maintainer";
    case SALT_TRUST_VOUCHED: return "vouched";
    case SALT_TRUST_DENOUNCED: return "denounced";
    default: return "unknown";
  }
}

salt_trust_level salt_trust_level_parse(const char *s) {
  if (!s) return SALT_TRUST_UNKNOWN;
  if (strcmp(s, "maintainer") == 0) return SALT_TRUST_MAINTAINER;
  if (strcmp(s, "vouched") == 0) return SALT_TRUST_VOUCHED;
  if (strcmp(s, "denounced") == 0) return SALT_TRUST_DENOUNCED;
  return SALT_TRUST_UNKNOWN;
}

void salt_findings_init(salt_findings *f) {
  f->items = NULL;
  f->len = 0;
  f->cap = 0;
}

int salt_findings_push(salt_findings *f, salt_risk_severity sev, const char *code, const char *msg) {
  if (f->len == f->cap) {
    size_t nc = f->cap ? f->cap * 2 : 8;
    salt_finding *ni = realloc(f->items, nc * sizeof(*ni));
    if (!ni) return SALT_ERR;
    f->items = ni;
    f->cap = nc;
  }
  f->items[f->len].severity = sev;
  f->items[f->len].code = salt_strdup(code);
  f->items[f->len].message = salt_strdup(msg);
  f->len++;
  return SALT_OK;
}

void salt_findings_free(salt_findings *f) {
  for (size_t i = 0; i < f->len; i++) {
    free(f->items[i].code);
    free(f->items[i].message);
  }
  free(f->items);
  f->items = NULL;
  f->len = 0;
  f->cap = 0;
}

bool salt_findings_has_block(const salt_findings *f) {
  for (size_t i = 0; i < f->len; i++)
    if (f->items[i].severity == SALT_RISK_BLOCK) return true;
  return false;
}

static char *recipe_toml_path(const char *recipe_path) {
  if (salt_is_dir(recipe_path)) return salt_join_path(recipe_path, "recipe.toml");
  return salt_strdup(recipe_path);
}

static bool is_hex64(const char *s) {
  if (!s) return false;
  if (strlen(s) != 64) return false;
  for (const char *p = s; *p; p++)
    if (!isxdigit((unsigned char)*p)) return false;
  return true;
}

static int lint_common(const char *recipe_path, salt_findings *out, bool strict) {
  char *path = recipe_toml_path(recipe_path);
  salt_buf text;
  if (salt_read_file(path, &text) != SALT_OK) {
    salt_findings_push(out, SALT_RISK_BLOCK, "no-recipe", "recipe.toml not found");
    free(path);
    return SALT_ERR_NOTFOUND;
  }
  salt_toml *t = salt_toml_parse(text.data, text.len);
  if (!t) {
    salt_findings_push(out, SALT_RISK_BLOCK, "parse", "recipe.toml is not valid TOML");
    salt_buf_free(&text);
    free(path);
    return SALT_ERR_FORMAT;
  }

  if (!salt_toml_string(t, "name", NULL))
    salt_findings_push(out, SALT_RISK_BLOCK, "name", "missing name");
  if (!salt_toml_string(t, "version", NULL))
    salt_findings_push(out, SALT_RISK_BLOCK, "version", "missing version");
  if (salt_toml_int(t, "release", 0) < 1)
    salt_findings_push(out, SALT_RISK_WARN, "release", "release should be >= 1");

  salt_strlist arch;
  salt_strlist_init(&arch);
  salt_toml_string_array(t, "arch", &arch);
  if (arch.len == 0)
    salt_findings_push(out, SALT_RISK_BLOCK, "arch", "no arch declared");
  for (size_t i = 0; i < arch.len; i++)
    if (strcmp(arch.items[i], "x86_64") != 0 && strcmp(arch.items[i], "aarch64") != 0)
      salt_findings_push(out, SALT_RISK_WARN, "arch-unknown", arch.items[i]);
  salt_strlist_free(&arch);

  const char *license = salt_toml_string(t, "license", NULL);
  if (!license || !license[0])
    salt_findings_push(out, SALT_RISK_BLOCK, "license", "missing license");

  const char *url = salt_toml_string(t, "source.url", NULL);
  if (!url || !url[0])
    salt_findings_push(out, SALT_RISK_BLOCK, "source-url", "missing source.url");

  const char *sha = salt_toml_string(t, "source.sha256", NULL);
  if (!sha || !sha[0]) {
    salt_findings_push(out, SALT_RISK_BLOCK, "source-sha", "missing source.sha256");
  } else if (strcmp(sha, "TODO-sha256") == 0) {
    salt_findings_push(out, strict ? SALT_RISK_BLOCK : SALT_RISK_WARN, "source-sha-todo",
                       "source.sha256 is a placeholder");
  } else if (!is_hex64(sha) && strncmp(url ? url : "", "file://", 7) != 0) {
    salt_findings_push(out, SALT_RISK_WARN, "source-sha-fmt", "source.sha256 is not 64 hex chars");
  }

  const salt_toml *bdeps = salt_toml_path(t, "build.deps");
  if (!bdeps)
    salt_findings_push(out, SALT_RISK_WARN, "build-deps", "no build deps declared");

  const char *repro = salt_toml_string(t, "reproducibility.status", NULL);
  if (!repro) {
    salt_findings_push(out, SALT_RISK_WARN, "repro", "missing reproducibility.status");
  } else if (strcmp(repro, "unverified") == 0) {
    const char *reason = salt_toml_string(t, "reproducibility.reason", NULL);
    if (!reason || !reason[0])
      salt_findings_push(out, SALT_RISK_WARN, "repro-reason",
                         "unverified reproducibility needs a reason");
  } else if (strcmp(repro, "verified") != 0) {
    salt_findings_push(out, SALT_RISK_WARN, "repro-status", "unknown reproducibility.status");
  }

  salt_toml_free(t);
  salt_buf_free(&text);
  free(path);
  return SALT_OK;
}

int salt_recipe_lint(const char *recipe_path, salt_findings *out) {
  return lint_common(recipe_path, out, false);
}

int salt_recipe_admission_check(const char *recipe_path, salt_findings *out) {
  return lint_common(recipe_path, out, true);
}

static bool regex_search(const char *pattern, const char *text) {
  regex_t re;
  if (regcomp(&re, pattern, REG_EXTENDED) != 0) return false;
  int rc = regexec(&re, text, 0, NULL, 0);
  regfree(&re);
  return rc == 0;
}

int salt_supplychain_scan(const salt_scan_input *in, salt_findings *out) {
  char *path = recipe_toml_path(in->recipe_path);
  salt_buf text;
  if (salt_read_file(path, &text) != SALT_OK) {
    salt_findings_push(out, SALT_RISK_BLOCK, "no-recipe", "recipe.toml not found");
    free(path);
    return SALT_ERR_NOTFOUND;
  }
  const char *body = text.data ? text.data : "";

  char *masked = salt_strdup(body);
  if (masked) {
    size_t n = strlen(masked);
    size_t i = 0;
    while (i < n) {
      if (isxdigit((unsigned char)masked[i])) {
        size_t j = i;
        while (j < n && isxdigit((unsigned char)masked[j])) j++;
        if (j - i >= 32)
          for (size_t k = i; k < j; k++) masked[k] = ' ';
        i = j;
      } else {
        i++;
      }
    }
  }
  const char *scan = masked ? masked : body;
  if (regex_search("0x[0-9a-fA-F]{40}([^0-9a-fA-F]|$)", body) ||
      regex_search("(^|[^1-9A-HJ-NP-Za-km-z])[13][1-9A-HJ-NP-Za-km-z]{25,34}([^1-9A-HJ-NP-Za-km-z]|$)",
                   scan) ||
      regex_search("bc1[ac-hj-np-z02-9]{11,71}", scan))
    salt_findings_push(out, SALT_RISK_BLOCK, "wallet",
                       "possible crypto wallet address in recipe");
  free(masked);

  if (strstr(body, "base64 -d") || strstr(body, "base64 --decode") || strstr(body, "eval ") ||
      strstr(body, "| sh") || strstr(body, "|sh") || strstr(body, "| bash"))
    salt_findings_push(out, SALT_RISK_WARN, "obfuscation",
                       "recipe contains eval/pipe-to-shell/base64 decoding");

  salt_toml *t = salt_toml_parse(body, text.len);
  const char *script = t ? salt_toml_string(t, "build.script", NULL) : NULL;
  if (script &&
      (strstr(script, "curl") || strstr(script, "wget") || strstr(script, "git clone") ||
       strstr(script, "http://") || strstr(script, "https://")))
    salt_findings_push(out, SALT_RISK_WARN, "build-network",
                       "build script appears to access the network after fetch");

  char *dir = salt_is_dir(in->recipe_path) ? salt_strdup(in->recipe_path) : NULL;
  if (dir) {
    char *scripts = salt_join_path(dir, "scripts");
    if (salt_is_dir(scripts))
      salt_findings_push(out, SALT_RISK_WARN, "install-scripts",
                         "package ships maintainer scripts (discouraged)");
    free(scripts);
    free(dir);
  }

  if (in->prev_recipe_text && t) {
    salt_toml *prev = salt_toml_parse(in->prev_recipe_text, strlen(in->prev_recipe_text));
    if (prev) {
      const char *old_url = salt_toml_string(prev, "source.url", "");
      const char *new_url = salt_toml_string(t, "source.url", "");
      if (strcmp(old_url, new_url) != 0)
        salt_findings_push(out, SALT_RISK_WARN, "source-url-change", "source.url changed");
      const char *old_sha = salt_toml_string(prev, "source.sha256", "");
      const char *new_sha = salt_toml_string(t, "source.sha256", "");
      if (strcmp(old_url, new_url) == 0 && strcmp(old_sha, new_sha) != 0)
        salt_findings_push(out, SALT_RISK_BLOCK, "sha-change-no-url",
                           "source.sha256 changed without a source.url change");
      salt_toml_free(prev);
    }
  }

  if (in->author_level == SALT_TRUST_DENOUNCED)
    salt_findings_push(out, SALT_RISK_BLOCK, "denounced", "author is denounced");
  else if (in->author_level == SALT_TRUST_UNKNOWN)
    salt_findings_push(out, SALT_RISK_WARN, "unknown-author",
                       "author is unknown; requires maintainer review");

  if (t) salt_toml_free(t);
  salt_buf_free(&text);
  free(path);
  return SALT_OK;
}

salt_trust_level salt_trust_lookup(const char *trustdb_path, const char *contributor) {
  salt_toml *t = salt_toml_parse_file(trustdb_path);
  if (!t) return SALT_TRUST_UNKNOWN;
  const salt_toml *arr = salt_toml_get(t, "contributor");
  size_t n = salt_toml_array_len(arr);
  salt_trust_level lvl = SALT_TRUST_UNKNOWN;
  for (size_t i = 0; i < n; i++) {
    const salt_toml *c = salt_toml_array_at(arr, i);
    const char *name = salt_toml_string(c, "name", "");
    if (strcmp(name, contributor) == 0)
      lvl = salt_trust_level_parse(salt_toml_string(c, "level", "unknown"));
  }
  salt_toml_free(t);
  return lvl;
}

int salt_trust_set(const char *trustdb_path, const char *contributor, salt_trust_level lvl,
                   const char *by, const char *reason) {
  salt_buf out;
  salt_buf_init(&out);
  bool replaced = false;
  salt_toml *t = salt_toml_parse_file(trustdb_path);
  if (t) {
    const salt_toml *arr = salt_toml_get(t, "contributor");
    size_t n = salt_toml_array_len(arr);
    for (size_t i = 0; i < n; i++) {
      const salt_toml *c = salt_toml_array_at(arr, i);
      const char *name = salt_toml_string(c, "name", "");
      salt_buf_append_str(&out, "[[contributor]]\n");
      if (strcmp(name, contributor) == 0) {
        replaced = true;
        salt_buf_printf(&out, "name = \"%s\"\n", contributor);
        salt_buf_printf(&out, "level = \"%s\"\n", salt_trust_level_name(lvl));
        salt_buf_printf(&out, "by = \"%s\"\n", by ? by : "");
        salt_buf_printf(&out, "reason = \"%s\"\n", reason ? reason : "");
        salt_buf_printf(&out, "time = %lld\n\n", (long long)time(NULL));
      } else {
        salt_buf_printf(&out, "name = \"%s\"\n", name);
        salt_buf_printf(&out, "level = \"%s\"\n", salt_toml_string(c, "level", "unknown"));
        salt_buf_printf(&out, "by = \"%s\"\n", salt_toml_string(c, "by", ""));
        salt_buf_printf(&out, "reason = \"%s\"\n", salt_toml_string(c, "reason", ""));
        salt_buf_printf(&out, "time = %lld\n\n", salt_toml_int(c, "time", 0));
      }
    }
    salt_toml_free(t);
  }
  if (!replaced) {
    salt_buf_append_str(&out, "[[contributor]]\n");
    salt_buf_printf(&out, "name = \"%s\"\n", contributor);
    salt_buf_printf(&out, "level = \"%s\"\n", salt_trust_level_name(lvl));
    salt_buf_printf(&out, "by = \"%s\"\n", by ? by : "");
    salt_buf_printf(&out, "reason = \"%s\"\n", reason ? reason : "");
    salt_buf_printf(&out, "time = %lld\n\n", (long long)time(NULL));
  }
  int rc = salt_write_file(trustdb_path, out.data ? out.data : "", out.len, 0644);
  salt_buf_free(&out);
  return rc;
}

int salt_trust_vouch(const char *trustdb_path, const char *voucher, const char *contributor,
                     const char *reason) {
  salt_trust_level vl = salt_trust_lookup(trustdb_path, voucher);
  if (vl != SALT_TRUST_MAINTAINER && vl != SALT_TRUST_VOUCHED) {
    salt_set_error("voucher '%s' is not trusted enough to vouch", voucher);
    return SALT_ERR;
  }
  salt_trust_level cur = salt_trust_lookup(trustdb_path, contributor);
  if (cur == SALT_TRUST_DENOUNCED) {
    salt_set_error("cannot vouch for a denounced contributor");
    return SALT_ERR;
  }
  salt_buf r;
  salt_buf_init(&r);
  salt_buf_printf(&r, "vouched by %s: %s", voucher, reason ? reason : "");
  int rc = salt_trust_set(trustdb_path, contributor, SALT_TRUST_VOUCHED, voucher, r.data);
  salt_buf_free(&r);
  return rc;
}

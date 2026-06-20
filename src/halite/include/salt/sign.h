#ifndef SALT_SIGN_H
#define SALT_SIGN_H

#include <stddef.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SALT_PUBKEY_HEXLEN 64
#define SALT_SECKEY_HEXLEN 128
#define SALT_SIG_HEXLEN 128

int salt_sign_init(void);
int salt_keypair_generate(char pub_hex[SALT_PUBKEY_HEXLEN + 1], char sec_hex[SALT_SECKEY_HEXLEN + 1]);
int salt_keypair_write(const char *dir, const char *name);
int salt_sign_buf(const void *data, size_t len, const char *sec_hex, char sig_hex[SALT_SIG_HEXLEN + 1]);
int salt_sign_file(const char *path, const char *sec_hex, char sig_hex[SALT_SIG_HEXLEN + 1]);
int salt_verify_buf(const void *data, size_t len, const char *sig_hex, const char *pub_hex);
int salt_verify_file(const char *path, const char *sig_hex, const char *pub_hex);

#ifdef __cplusplus
}
#endif

#endif

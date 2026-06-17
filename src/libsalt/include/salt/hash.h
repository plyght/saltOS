#ifndef SALT_HASH_H
#define SALT_HASH_H

#include <stddef.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SALT_SHA256_HEXLEN 64

int salt_sha256_buf(const void *data, size_t len, char hex_out[SALT_SHA256_HEXLEN + 1]);
int salt_sha256_file(const char *path, char hex_out[SALT_SHA256_HEXLEN + 1]);

#ifdef __cplusplus
}
#endif

#endif

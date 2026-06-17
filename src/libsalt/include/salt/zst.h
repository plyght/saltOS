#ifndef SALT_ZST_H
#define SALT_ZST_H

#include <stddef.h>
#include "salt/util.h"

#ifdef __cplusplus
extern "C" {
#endif

int salt_zst_compress(const void *src, size_t src_len, int level, salt_buf *out);
int salt_zst_decompress(const void *src, size_t src_len, salt_buf *out);

#ifdef __cplusplus
}
#endif

#endif

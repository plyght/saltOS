#include "salt/zst.h"

#include <stdlib.h>
#include <zstd.h>

int salt_zst_compress(const void *src, size_t src_len, int level, salt_buf *out) {
  salt_buf_init(out);
  size_t bound = ZSTD_compressBound(src_len);
  char *tmp = malloc(bound);
  if (!tmp) {
    salt_set_error("out of memory");
    return SALT_ERR;
  }
  size_t r = ZSTD_compress(tmp, bound, src, src_len, level);
  if (ZSTD_isError(r)) {
    salt_set_error("zstd compress: %s", ZSTD_getErrorName(r));
    free(tmp);
    return SALT_ERR;
  }
  int rc = salt_buf_append(out, tmp, r);
  free(tmp);
  return rc;
}

int salt_zst_decompress(const void *src, size_t src_len, salt_buf *out) {
  salt_buf_init(out);
  unsigned long long usize = ZSTD_getFrameContentSize(src, src_len);
  if (usize == ZSTD_CONTENTSIZE_ERROR) {
    salt_set_error("not a zstd frame");
    return SALT_ERR_FORMAT;
  }
  if (usize != ZSTD_CONTENTSIZE_UNKNOWN) {
    char *tmp = malloc(usize ? usize : 1);
    if (!tmp) {
      salt_set_error("out of memory");
      return SALT_ERR;
    }
    size_t r = ZSTD_decompress(tmp, usize, src, src_len);
    if (ZSTD_isError(r)) {
      salt_set_error("zstd decompress: %s", ZSTD_getErrorName(r));
      free(tmp);
      return SALT_ERR;
    }
    int rc = salt_buf_append(out, tmp, r);
    free(tmp);
    return rc;
  }
  ZSTD_DStream *ds = ZSTD_createDStream();
  if (!ds) {
    salt_set_error("zstd dstream");
    return SALT_ERR;
  }
  ZSTD_initDStream(ds);
  ZSTD_inBuffer in = {src, src_len, 0};
  char chunk[65536];
  while (in.pos < in.size) {
    ZSTD_outBuffer ob = {chunk, sizeof(chunk), 0};
    size_t r = ZSTD_decompressStream(ds, &ob, &in);
    if (ZSTD_isError(r)) {
      salt_set_error("zstd decompress: %s", ZSTD_getErrorName(r));
      ZSTD_freeDStream(ds);
      return SALT_ERR;
    }
    if (salt_buf_append(out, chunk, ob.pos) != SALT_OK) {
      ZSTD_freeDStream(ds);
      return SALT_ERR;
    }
    if (r == 0) break;
  }
  ZSTD_freeDStream(ds);
  return SALT_OK;
}

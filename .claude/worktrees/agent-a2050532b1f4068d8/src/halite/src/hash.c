#include "salt/hash.h"

#include <stdio.h>
#include <sodium.h>

static void hex_encode(const unsigned char *in, size_t n, char *out) {
  static const char *d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = d[in[i] >> 4];
    out[i * 2 + 1] = d[in[i] & 0xf];
  }
  out[n * 2] = '\0';
}

int salt_sha256_buf(const void *data, size_t len, char hex_out[SALT_SHA256_HEXLEN + 1]) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  if (crypto_hash_sha256(digest, data, len) != 0) {
    salt_set_error("sha256 failed");
    return SALT_ERR;
  }
  hex_encode(digest, sizeof(digest), hex_out);
  return SALT_OK;
}

int salt_sha256_file(const char *path, char hex_out[SALT_SHA256_HEXLEN + 1]) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    salt_set_error("open %s", path);
    return SALT_ERR_IO;
  }
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  unsigned char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    crypto_hash_sha256_update(&st, chunk, n);
  int err = ferror(f);
  fclose(f);
  if (err) {
    salt_set_error("read %s", path);
    return SALT_ERR_IO;
  }
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, digest);
  hex_encode(digest, sizeof(digest), hex_out);
  return SALT_OK;
}

#include "salt/sign.h"

#include <string.h>
#include <sodium.h>

static void hex_encode(const unsigned char *in, size_t n, char *out) {
  static const char *d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = d[in[i] >> 4];
    out[i * 2 + 1] = d[in[i] & 0xf];
  }
  out[n * 2] = '\0';
}

static int hex_decode(const char *hex, unsigned char *out, size_t out_len) {
  size_t hl = strlen(hex);
  if (hl != out_len * 2) return SALT_ERR_FORMAT;
  for (size_t i = 0; i < out_len; i++) {
    int hi, lo;
    char c = hex[i * 2];
    char d = hex[i * 2 + 1];
    if (c >= '0' && c <= '9') hi = c - '0';
    else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
    else return SALT_ERR_FORMAT;
    if (d >= '0' && d <= '9') lo = d - '0';
    else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
    else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
    else return SALT_ERR_FORMAT;
    out[i] = (unsigned char)((hi << 4) | lo);
  }
  return SALT_OK;
}

int salt_sign_init(void) {
  if (sodium_init() < 0) {
    salt_set_error("libsodium init failed");
    return SALT_ERR;
  }
  return SALT_OK;
}

int salt_keypair_generate(char pub_hex[SALT_PUBKEY_HEXLEN + 1], char sec_hex[SALT_SECKEY_HEXLEN + 1]) {
  if (salt_sign_init() != SALT_OK) return SALT_ERR;
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  crypto_sign_keypair(pk, sk);
  hex_encode(pk, sizeof(pk), pub_hex);
  hex_encode(sk, sizeof(sk), sec_hex);
  return SALT_OK;
}

int salt_keypair_write(const char *dir, const char *name) {
  char pub_hex[SALT_PUBKEY_HEXLEN + 1];
  char sec_hex[SALT_SECKEY_HEXLEN + 1];
  if (salt_keypair_generate(pub_hex, sec_hex) != SALT_OK) return SALT_ERR;
  if (salt_mkdirs(dir, 0700) != SALT_OK) return SALT_ERR_IO;
  salt_buf pp;
  salt_buf_init(&pp);
  salt_buf_printf(&pp, "%s/%s.pub", dir, name);
  salt_buf sp;
  salt_buf_init(&sp);
  salt_buf_printf(&sp, "%s/%s.sec", dir, name);
  int r = salt_write_file(pp.data, pub_hex, strlen(pub_hex), 0644);
  if (r == SALT_OK) r = salt_write_file(sp.data, sec_hex, strlen(sec_hex), 0600);
  salt_buf_free(&pp);
  salt_buf_free(&sp);
  return r;
}

int salt_sign_buf(const void *data, size_t len, const char *sec_hex, char sig_hex[SALT_SIG_HEXLEN + 1]) {
  if (salt_sign_init() != SALT_OK) return SALT_ERR;
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  if (hex_decode(sec_hex, sk, sizeof(sk)) != SALT_OK) {
    salt_set_error("invalid secret key");
    return SALT_ERR_FORMAT;
  }
  unsigned char sig[crypto_sign_BYTES];
  unsigned long long siglen = 0;
  crypto_sign_detached(sig, &siglen, data, len, sk);
  hex_encode(sig, sizeof(sig), sig_hex);
  return SALT_OK;
}

int salt_sign_file(const char *path, const char *sec_hex, char sig_hex[SALT_SIG_HEXLEN + 1]) {
  salt_buf b;
  if (salt_read_file(path, &b) != SALT_OK) return SALT_ERR_IO;
  int r = salt_sign_buf(b.data, b.len, sec_hex, sig_hex);
  salt_buf_free(&b);
  return r;
}

int salt_verify_buf(const void *data, size_t len, const char *sig_hex, const char *pub_hex) {
  if (salt_sign_init() != SALT_OK) return SALT_ERR;
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sig[crypto_sign_BYTES];
  if (hex_decode(pub_hex, pk, sizeof(pk)) != SALT_OK) {
    salt_set_error("invalid public key");
    return SALT_ERR_FORMAT;
  }
  if (hex_decode(sig_hex, sig, sizeof(sig)) != SALT_OK) {
    salt_set_error("invalid signature");
    return SALT_ERR_FORMAT;
  }
  if (crypto_sign_verify_detached(sig, data, len, pk) != 0) {
    salt_set_error("signature verification failed");
    return SALT_ERR_VERIFY;
  }
  return SALT_OK;
}

int salt_verify_file(const char *path, const char *sig_hex, const char *pub_hex) {
  salt_buf b;
  if (salt_read_file(path, &b) != SALT_OK) return SALT_ERR_IO;
  int r = salt_verify_buf(b.data, b.len, sig_hex, pub_hex);
  salt_buf_free(&b);
  return r;
}

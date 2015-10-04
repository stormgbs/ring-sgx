/* Copyright (c) 2014, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/aead.h>

#include <string.h>

#include <openssl/chacha.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/poly1305.h>

#include "internal.h"


#define POLY1305_TAG_LEN 16
#define CHACHA20_NONCE_LEN 12

struct aead_chacha20_poly1305_ctx {
  unsigned char key[32];
};

static int aead_chacha20_poly1305_init(EVP_AEAD_CTX *ctx, const uint8_t *key,
                                       size_t key_len) {
  aead_assert_init_preconditions(ctx, key, key_len);

  struct aead_chacha20_poly1305_ctx *c20_ctx;

  c20_ctx = OPENSSL_malloc(sizeof(struct aead_chacha20_poly1305_ctx));
  if (c20_ctx == NULL) {
    return 0;
  }

  memcpy(c20_ctx->key, key, key_len);
  ctx->aead_state = c20_ctx;

  return 1;
}

static void aead_chacha20_poly1305_cleanup(EVP_AEAD_CTX *ctx) {
  struct aead_chacha20_poly1305_ctx *c20_ctx = ctx->aead_state;
  OPENSSL_cleanse(c20_ctx->key, sizeof(c20_ctx->key));
  OPENSSL_free(c20_ctx);
}

static void poly1305_update_length(poly1305_state *poly1305, size_t data_len) {
  size_t j = data_len;
  uint8_t length_bytes[8];
  unsigned i;

  for (i = 0; i < sizeof(length_bytes); i++) {
    length_bytes[i] = j;
    j >>= 8;
  }

  CRYPTO_poly1305_update(poly1305, length_bytes, sizeof(length_bytes));
}

#if defined(__arm__)
#define ALIGNED __attribute__((aligned(16)))
#else
#define ALIGNED
#endif

typedef void (*aead_poly1305_update)(poly1305_state *ctx, const uint8_t *ad,
                                     size_t ad_len, const uint8_t *ciphertext,
                                     size_t ciphertext_len);

/* aead_poly1305 fills |tag| with the authentication tag for the given
 * inputs, using |update| to control the order and format that the inputs are
 * signed/authenticated. */
static void aead_poly1305(aead_poly1305_update update,
                          uint8_t tag[POLY1305_TAG_LEN],
                          const struct aead_chacha20_poly1305_ctx *c20_ctx,
                          const uint8_t nonce[CHACHA20_NONCE_LEN],
                          const uint8_t *ad, size_t ad_len,
                          const uint8_t *ciphertext, size_t ciphertext_len) {
  uint8_t poly1305_key[32] ALIGNED;
  memset(poly1305_key, 0, sizeof(poly1305_key));
  CRYPTO_chacha_20(poly1305_key, poly1305_key, sizeof(poly1305_key),
                   c20_ctx->key, nonce, 0);
  poly1305_state ctx;
  CRYPTO_poly1305_init(&ctx, poly1305_key);
  update(&ctx, ad, ad_len, ciphertext, ciphertext_len);
  CRYPTO_poly1305_finish(&ctx, tag);
}

static int aead_chacha20_poly1305_seal(aead_poly1305_update poly1305_update,
                                       const EVP_AEAD_CTX *ctx, uint8_t *out,
                                       size_t *out_len, size_t max_out_len,
                                       const uint8_t nonce[12],
                                       const uint8_t *in, size_t in_len,
                                       const uint8_t *ad, size_t ad_len) {
  aead_assert_open_seal_preconditions(ctx, out, out_len, in, in_len, ad, ad_len);

  const struct aead_chacha20_poly1305_ctx *c20_ctx = ctx->aead_state;

  if (!aead_seal_out_max_out_in_tag_len(out_len, max_out_len, in_len,
                                        POLY1305_TAG_LEN)) {
    /* |aead_seal_out_max_out_in_tag_len| already called |OPENSSL_PUT_ERROR|. */
    return 0;
  }

  CRYPTO_chacha_20(out, in, in_len, c20_ctx->key, nonce, 1);

  uint8_t tag[POLY1305_TAG_LEN] ALIGNED;
  aead_poly1305(poly1305_update, tag, c20_ctx, nonce, ad, ad_len, out, in_len);

  /* TODO: Does |tag| really need to be |ALIGNED|? If not, we can avoid this
   * call to |memcpy|. */
  memcpy(out + in_len, tag, POLY1305_TAG_LEN);

  return 1;
}

static int aead_chacha20_poly1305_open(aead_poly1305_update poly1305_update,
                                       const EVP_AEAD_CTX *ctx, uint8_t *out,
                                       size_t *out_len, size_t max_out_len,
                                       const uint8_t nonce[12],
                                       const uint8_t *in, size_t in_len,
                                       const uint8_t *ad, size_t ad_len) {
  aead_assert_open_seal_preconditions(ctx, out, out_len, in, in_len, ad, ad_len);

  const struct aead_chacha20_poly1305_ctx *c20_ctx = ctx->aead_state;

  if (!aead_open_out_max_out_in_tag_len(out_len, max_out_len, in_len,
                                        POLY1305_TAG_LEN)) {
    /* |aead_open_out_max_out_in_tag_len| already called
     * |OPENSSL_PUT_ERROR|. */
    return 0;
  }

  size_t plaintext_len;

  plaintext_len = in_len - POLY1305_TAG_LEN;
  uint8_t tag[POLY1305_TAG_LEN] ALIGNED;
  aead_poly1305(poly1305_update, tag, c20_ctx, nonce, ad, ad_len, in,
                plaintext_len);
  if (CRYPTO_memcmp(tag, in + plaintext_len, POLY1305_TAG_LEN) != 0) {
    OPENSSL_PUT_ERROR(CIPHER, CIPHER_R_BAD_DECRYPT);
    return 0;
  }

  CRYPTO_chacha_20(out, in, plaintext_len, c20_ctx->key, nonce, 1);
  *out_len = plaintext_len;
  return 1;
}

static void poly1305_update_padded_16(poly1305_state *poly1305,
                                      const uint8_t *data, size_t data_len) {
  static const uint8_t padding[16] = { 0 }; /* Padding is all zeros. */

  CRYPTO_poly1305_update(poly1305, data, data_len);
  if (data_len % 16 != 0) {
    CRYPTO_poly1305_update(poly1305, padding, sizeof(padding) - (data_len % 16));
  }
}

static void poly1305_update_rfc7539(poly1305_state *ctx, const uint8_t *ad,
                                    size_t ad_len, const uint8_t *ciphertext,
                                    size_t ciphertext_len) {
  poly1305_update_padded_16(ctx, ad, ad_len);
  poly1305_update_padded_16(ctx, ciphertext, ciphertext_len);
  poly1305_update_length(ctx, ad_len);
  poly1305_update_length(ctx, ciphertext_len);
}

static int aead_chacha20_poly1305_rfc7539_seal(const EVP_AEAD_CTX *ctx,
                                               uint8_t *out, size_t *out_len,
                                               size_t max_out_len,
                                               const uint8_t *nonce,
                                               const uint8_t *in, size_t in_len,
                                               const uint8_t *ad,
                                               size_t ad_len) {
  return aead_chacha20_poly1305_seal(poly1305_update_rfc7539, ctx, out, out_len,
                                     max_out_len, nonce, in, in_len, ad, ad_len);
}

static int aead_chacha20_poly1305_rfc7539_open(const EVP_AEAD_CTX *ctx,
                                               uint8_t *out, size_t *out_len,
                                               size_t max_out_len,
                                               const uint8_t *nonce,
                                               const uint8_t *in, size_t in_len,
                                               const uint8_t *ad, size_t ad_len) {
  return aead_chacha20_poly1305_open(poly1305_update_rfc7539, ctx, out, out_len,
                                     max_out_len, nonce, in, in_len, ad, ad_len);
}

static const EVP_AEAD aead_chacha20_poly1305_rfc7539 = {
    32,                 /* key len */
    CHACHA20_NONCE_LEN, /* nonce len */
    POLY1305_TAG_LEN,   /* overhead */
    POLY1305_TAG_LEN,   /* max tag length */
    aead_chacha20_poly1305_init,
    NULL, /* init_with_direction */
    aead_chacha20_poly1305_cleanup,
    aead_chacha20_poly1305_rfc7539_seal,
    aead_chacha20_poly1305_rfc7539_open,
};

const EVP_AEAD *EVP_aead_chacha20_poly1305_rfc7539(void) {
  return &aead_chacha20_poly1305_rfc7539;
}

static void poly1305_update_deprecated(poly1305_state *ctx, const uint8_t *ad,
                                       size_t ad_len, const uint8_t *ciphertext,
                                       size_t ciphertext_len) {
  CRYPTO_poly1305_update(ctx, ad, ad_len);
  poly1305_update_length(ctx, ad_len);
  CRYPTO_poly1305_update(ctx, ciphertext, ciphertext_len);
  poly1305_update_length(ctx, ciphertext_len);
}

static int aead_chacha20_poly1305_deprecated_seal(const EVP_AEAD_CTX *ctx,
                                                  uint8_t *out, size_t *out_len,
                                                  size_t max_out_len,
                                                  const uint8_t *nonce,
                                                  const uint8_t *in,
                                                  size_t in_len,
                                                  const uint8_t *ad,
                                                  size_t ad_len) {
  return aead_chacha20_poly1305_seal(poly1305_update_deprecated, ctx, out,
                                     out_len, max_out_len, nonce, in, in_len,
                                     ad, ad_len);
}

static int aead_chacha20_poly1305_deprecated_open(const EVP_AEAD_CTX *ctx,
                                                  uint8_t *out, size_t *out_len,
                                                  size_t max_out_len,
                                                  const uint8_t *nonce,
                                                  const uint8_t *in,
                                                  size_t in_len,
                                                  const uint8_t *ad,
                                                  size_t ad_len) {
  return aead_chacha20_poly1305_open(poly1305_update_deprecated, ctx, out,
                                     out_len, max_out_len, nonce, in, in_len,
                                     ad, ad_len);
}

static const EVP_AEAD aead_chacha20_poly1305_deprecated = {
    32,                 /* key len */
    CHACHA20_NONCE_LEN, /* nonce len */
    POLY1305_TAG_LEN,   /* overhead */
    POLY1305_TAG_LEN,   /* max tag length */
    aead_chacha20_poly1305_init,
    NULL, /* init_with_direction */
    aead_chacha20_poly1305_cleanup,
    aead_chacha20_poly1305_deprecated_seal,
    aead_chacha20_poly1305_deprecated_open,
};

const EVP_AEAD *EVP_aead_chacha20_poly1305_deprecated(void) {
  return &aead_chacha20_poly1305_deprecated;
}

#include "secret_hash.h"
#include <string.h>
#include "esp_random.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

#define ITERATIONS 20000

void secret_hash_make(const char *secret, const uint8_t salt[SECRET_SALT_LEN],
                      uint8_t out[SECRET_HASH_LEN])
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char *)secret, strlen(secret),
                                  salt, SECRET_SALT_LEN, ITERATIONS,
                                  SECRET_HASH_LEN, out);
}

bool secret_hash_equal(const uint8_t a[SECRET_HASH_LEN], const uint8_t b[SECRET_HASH_LEN])
{
    uint8_t diff = 0;
    for (int i = 0; i < SECRET_HASH_LEN; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

void secret_hash_new_salt(uint8_t salt[SECRET_SALT_LEN])
{
    esp_fill_random(salt, SECRET_SALT_LEN);
}

uint32_t secret_backoff_s(uint32_t fails, uint32_t free_tries)
{
    if (fails <= free_tries) return 0;
    uint32_t s = 30;
    for (uint32_t i = free_tries + 1; i < fails && s < 3600; i++) s *= 2;
    return s > 3600 ? 3600 : s;
}

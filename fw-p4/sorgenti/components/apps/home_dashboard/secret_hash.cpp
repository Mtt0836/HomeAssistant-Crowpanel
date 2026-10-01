#include "secret_hash.h"
#include <string.h>
#include "esp_random.h"
#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "secret_hash";

/* Venti mila giri. Questo numero NON si tocca.

   L'impronta di una password e' PBKDF2-HMAC-SHA256(password, sale, giri), e
   quello che c'e' salvato nella memoria del pannello e' il risultato di questo
   conto con questi parametri. Cambiare i giri - anche solo alzarli, che di per
   se' sarebbe un miglioramento - vuol dire che le password gia' salvate non
   corrisponderanno piu': chi ha il pannello al muro si ritrova chiuso fuori
   dalla sua pagina, e l'unico modo di rientrare e' azzerare tutto dal PIN
   sullo schermo. Se un giorno si vorranno piu' giri, va salvato accanto
   all'impronta anche il numero usato per calcolarla. */
#define ITERATIONS 20000

/* ESP-IDF 6 porta mbedTLS 4, dove la crittografia sta dietro le PSA Crypto API
   e mbedtls/pkcs5.h e' diventato privato (sta sotto mbedtls/private/). Questa
   e' la stessa cosa scritta con le API nuove.

   PBKDF2 pero' e' uno standard: stessa password, stesso sale, stesso numero di
   giri danno gli stessi byte, chiunque faccia il conto. Quindi le impronte gia'
   salvate continuano a corrispondere e nessuno resta chiuso fuori. E' il
   motivo per cui questa migrazione si poteva fare senza toccare la memoria dei
   pannelli gia' installati. */
void secret_hash_make(const char *secret, const uint8_t salt[SECRET_SALT_LEN],
                      uint8_t out[SECRET_HASH_LEN])
{
    memset(out, 0, SECRET_HASH_LEN);

    static bool avviata = false;
    if (!avviata) {
        psa_status_t s = psa_crypto_init();
        if (s != PSA_SUCCESS) { ESP_LOGE(TAG, "psa_crypto_init: %d", (int)s); return; }
        avviata = true;
    }

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t s = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (s == PSA_SUCCESS)
        s = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, ITERATIONS);
    if (s == PSA_SUCCESS)
        s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                           salt, SECRET_SALT_LEN);
    if (s == PSA_SUCCESS)
        s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                           (const uint8_t *)secret, strlen(secret));
    if (s == PSA_SUCCESS)
        s = psa_key_derivation_output_bytes(&op, out, SECRET_HASH_LEN);
    psa_key_derivation_abort(&op);

    /* Se qualcosa va storto out resta a zero. Meglio un'impronta che non
       corrisponde a niente che una calcolata a meta': la prima nega l'accesso,
       la seconda lo regalerebbe a chiunque faccia sbagliare il conto allo
       stesso modo. */
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 fallito: %d", (int)s);
        memset(out, 0, SECRET_HASH_LEN);
    }
}

bool secret_hash_equal(const uint8_t a[SECRET_HASH_LEN], const uint8_t b[SECRET_HASH_LEN])
{
    uint8_t diff = 0;
    for (int i = 0; i < SECRET_HASH_LEN; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

bool secret_str_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    /* Le lunghezze si possono confrontare in chiaro: quella dell'identificativo
       di sessione e' fissa e nota a tutti, non e' un segreto. Sono i caratteri
       a esserlo, e quelli si guardano fino in fondo anche quando il primo e'
       gia' sbagliato. */
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
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

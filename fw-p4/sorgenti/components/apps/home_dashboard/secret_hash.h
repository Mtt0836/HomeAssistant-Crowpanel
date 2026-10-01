#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Impronta di un segreto (PIN dello schermo, password della pagina web).
//
// Di PIN e password non si salva mai il testo: si salva il risultato di
// PBKDF2-HMAC-SHA256 con un sale casuale diverso per ognuno. Chi leggesse la
// memoria del pannello non ne ricaverebbe le cifre, e due utenti con la stessa
// password hanno impronte diverse.

#ifdef __cplusplus
extern "C" {
#endif

#define SECRET_SALT_LEN 16
#define SECRET_HASH_LEN 32

void secret_hash_make(const char *secret, const uint8_t salt[SECRET_SALT_LEN],
                      uint8_t out[SECRET_HASH_LEN]);

// Confronto a tempo costante: non deve trapelare quanto ci si e' avvicinati.
bool secret_hash_equal(const uint8_t a[SECRET_HASH_LEN], const uint8_t b[SECRET_HASH_LEN]);

// Lo stesso per due stringhe segrete (l'identificativo di sessione del cookie).
// strcmp si ferma al primo carattere diverso, e il tempo che ci mette racconta
// quanti ne aveva gia' indovinati: qui si guardano sempre tutti.
bool secret_str_equal(const char *a, const char *b);

void secret_hash_new_salt(uint8_t salt[SECRET_SALT_LEN]);

// Attesa dopo i tentativi sbagliati: 0 fino a "free_tries", poi 30 s che
// raddoppiano fino a un'ora. E' questa, non la lunghezza del segreto, a
// rendere inutile provarli tutti.
uint32_t secret_backoff_s(uint32_t fails, uint32_t free_tries);

#ifdef __cplusplus
}
#endif

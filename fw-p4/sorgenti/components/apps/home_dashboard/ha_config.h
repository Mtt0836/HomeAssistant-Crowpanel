#pragma once
#include <stddef.h>
#include <stdbool.h>

// Config persistente (NVS): URL di Home Assistant + long-lived token.
// URL nel formato "ws://192.168.1.20:8123/api/websocket" (o wss:// con HTTPS).

#define HA_URL_MAX    128
#define HA_TOKEN_MAX  400   // il JWT di HA e' lungo (~180+ char), stiamo larghi

#ifdef __cplusplus
extern "C" {
#endif

bool ha_config_is_set(void);                                   // true se url+token presenti
bool ha_config_load(char *url, size_t url_sz,
                    char *token, size_t token_sz);             // true se caricati
bool ha_config_load_url(char *url, size_t sz);                 // solo URL, senza toccare il token

// Identificativo dell'istanza di Home Assistant, come lo annuncia lei stessa
// sulla rete. Serve a riconoscerla se un giorno cambia indirizzo: il nome non
// basterebbe, di "Casa" ce n'e' una per casa.
#define HA_UUID_MAX 40
bool ha_config_load_uuid(char *uuid, size_t sz);
bool ha_config_save_uuid(const char *uuid);
bool ha_config_save(const char *url, const char *token);      // salva in NVS
bool ha_config_save_url(const char *url);                      // solo URL, token intatto
void ha_config_clear(void);

// Dashboard Lovelace da mostrare: url_path (es. "casa", "lovelace" o vuoto =
// quella predefinita di HA) e indice della vista. Default: "lovelace", vista 0.
#define HA_DASH_MAX   64
void ha_config_load_dash(char *path, size_t sz, int *view);
bool ha_config_save_dash(const char *path, int view);

// Quale motore vocale di Home Assistant usare per leggere gli avvisi ad alta
// voce ("tts.piper", "tts.google_it_it", ...). Era scritto dentro il codice, e
// chi non aveva Piper si ritrovava la voce muta senza che niente lo dicesse.
// Vuoto in memoria = si usa il valore predefinito.
/* Il certificato di chi non usa un'autorita' pubblica.

   Con "wss://" il pannello controlla che il certificato di Home Assistant sia
   firmato da un'autorita' che conosce: il pacchetto compilato dentro ESP-IDF
   copre quelle pubbliche, quindi Nabu Casa, Let's Encrypt e simili funzionano
   senza fare niente.

   Chi invece si e' fatto il certificato da se' - che e' la norma per un HA in
   casa dietro HTTPS - ha un certificato che nessuna autorita' pubblica ha
   firmato, e il collegamento verrebbe rifiutato. La soluzione non e'
   smettere di controllare: e' dire al pannello di chi fidarsi, incollandogli
   quel certificato. Vuoto = si usano le autorita' pubbliche. */
#define HA_CA_MAX 2600
bool ha_config_load_ca(char *pem, size_t sz);     // false se non c'e'
bool ha_config_save_ca(const char *pem);          // NULL o vuoto = toglilo
bool ha_config_has_ca(void);

#define HA_TTS_MAX 64
void ha_config_load_tts(char *id, size_t sz);
bool ha_config_save_tts(const char *id);

#ifdef __cplusplus
}
#endif

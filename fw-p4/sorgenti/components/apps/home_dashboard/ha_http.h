#pragma once
#include <stdbool.h>
#include <stddef.h>

// Pezzi comuni per parlare con Home Assistant fuori dal WebSocket: indirizzi,
// codifica e una POST in form. Li usano sia l'abbinamento (ha_oauth) sia il
// rinnovo del permesso (ha_token).

#ifdef __cplusplus
extern "C" {
#endif

// Da "ws://host:porta/api/websocket" a "http://host:porta" (wss -> https).
bool ha_http_base(char *out, size_t sz);

// Indirizzo del pannello, con cui si presenta a HA: "https://<ip><path>".
bool ha_http_panel_url(char *out, size_t sz, const char *path);

void ha_http_url_encode(const char *in, char *out, size_t sz);

// POST application/x-www-form-urlencoded. Ritorna il codice HTTP, -1 se non
// e' nemmeno partita. La risposta finisce in resp.
int ha_http_post_form(const char *url, const char *body, char *resp, size_t resp_sz);

// GET con il permesso di Home Assistant gia' attaccato. Serve alle parti
// dell'API che il WebSocket non espone: il calendario e il registro degli
// eventi si chiedono solo via HTTP. La risposta puo' essere lunga, quindi il
// buffer lo alloca chi chiama. Ritorna il codice HTTP, -1 se non parte.
int ha_http_get_auth(const char *url, char *resp, size_t resp_sz);

/* Il certificato da usare per parlare in sicurezza con Home Assistant.

   Ritorna il PEM dell'autorita' che l'utente ha indicato, oppure NULL se non
   ne ha indicata nessuna - e allora si usa il pacchetto di autorita'
   pubbliche compilato dentro ESP-IDF.

   Il testo restituito resta valido finche' qualcuno non cambia il
   certificato: si puo' passare direttamente a esp-tls senza copiarlo. E' un
   posto solo perche' i client che parlano con HA sono quattro - il
   WebSocket, la voce, le immagini e le prove della console - e tenerne
   quattro copie vorrebbe dire che un giorno tre sono aggiornate e una no. */
const char *ha_tls_ca(void);

/* Rilegge il certificato dalla memoria: la chiama chi lo cambia. */
void ha_tls_ricarica(void);

#ifdef __cplusplus
}
#endif

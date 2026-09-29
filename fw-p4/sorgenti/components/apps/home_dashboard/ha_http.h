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

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdbool.h>
#include <stddef.h>

// Accesso alla pagina del pannello con l'account di Home Assistant.
//
// HA espone lo stesso meccanismo che usa l'app ufficiale (IndieAuth/OAuth2):
// il browser va sulla schermata di login di HA, l'utente autorizza, e HA
// rimanda al pannello un codice usa e getta. Il pannello lo scambia con HA,
// chiede chi ha autorizzato e apre la sessione.
//
// Chi in HA e' amministratore lo e' anche qui; gli altri entrano come ospiti,
// cioe' possono riordinare e nascondere le card e basta.
//
// Del token dell'utente non resta niente: serve solo per chiedere il nome e
// se e' amministratore, poi viene revocato. La password locale resta come
// accesso di riserva per quando HA non risponde.

#ifdef __cplusplus
extern "C" {
#endif

// true se HA e' configurato e il pannello ha un indirizzo: senza, il pulsante
// "Entra con Home Assistant" non ha senso.
bool ha_oauth_available(void);

// Indirizzo della schermata di autorizzazione di HA, con lo stato monouso gia'
// dentro. false se non si puo' costruire.
bool ha_oauth_start_url(char *out, size_t out_sz);

// Ritorno da HA: verifica lo stato, scambia il codice, scopre chi e'.
// A buon fine riempie sid (>= 33 byte) e name; err spiega cosa non ha
// funzionato. Blocca per qualche secondo: va chiamata dal task del server web.
bool ha_oauth_finish(const char *code, const char *state,
                     char *sid, size_t sid_sz,
                     char *name, size_t name_sz,
                     char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif

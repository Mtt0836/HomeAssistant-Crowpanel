#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

// Ritocchi locali alla dashboard di Home Assistant.
//
// La dashboard resta quella di HA: qui teniamo solo le differenze volute sul
// pannello (card nascoste, ordine diverso, card aggiunte solo qui). Vengono
// applicate sopra la configurazione appena arriva da HA, cosi' se su HA
// cambia qualcosa il resto si aggiorna lo stesso e le card che non
// riconosciamo piu' ricompaiono semplicemente in fondo, visibili.
//
// I ritocchi stanno in /spiffs/overrides.json, uno per ogni vista.
//
// Ogni card ha una "chiave" ricavata dal suo contenuto (tipo + entita' o
// titolo, con un contatore per i doppioni): e' l'unico appiglio stabile,
// visto che Lovelace non da' un id alle card. La chiave la calcola solo
// questo modulo e la manda all'editor web dentro l'elenco, cosi' il browser
// non deve reimplementarla.

#ifdef __cplusplus
extern "C" {
#endif

void dash_override_init(void);

// Chiamata quando arriva la configurazione da HA: tiene da parte la vista
// originale e restituisce {"views":[vista ritoccata]} da passare a
// ll_set_config(.., 0, ..). Da liberare con cJSON_Delete. NULL se la vista
// non esiste.
cJSON *dash_override_prepare(const cJSON *config, int view, const char *dash);

// Rigenera la vista ritoccata dall'originale gia' in memoria (dopo un
// salvataggio dall'editor), stesso formato di dash_override_prepare.
cJSON *dash_override_rebuild(void);
bool   dash_override_has_source(void);

// Elenco per l'editor web: contenitori, card, etichette, chi e' nascosto.
// Da liberare con cJSON_Delete.
cJSON *dash_override_layout(void);

// Ritocchi della vista corrente, come li rimanda l'editor. Da liberare.
cJSON *dash_override_current(void);

// Salva i ritocchi della vista corrente (JSON: order, hidden, extra).
bool dash_override_save(const char *json);

#ifdef __cplusplus
}
#endif

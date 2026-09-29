#pragma once
#include "lvgl.h"
#include "cJSON.h"

// Rendering di una vista Lovelace di Home Assistant con LVGL.
// La configurazione arriva da lovelace/config, gli stati da subscribe_entities.
// Tutte le funzioni vanno chiamate con il lock del display preso.
//
// Card supportate (fase 1): heading, tile, button, entity, sensor, entities,
// glance, gauge, statistics-graph, history-graph,
// vertical-stack, horizontal-stack, grid. Le altre compaiono come
// riquadro grigio con il loro tipo.

// Sostituisce la vista corrente con config.views[view] (copia interna).
// Ritorna false e riempie err se la vista non e' utilizzabile.
bool ll_set_config(const cJSON *config, int view, char *err, size_t err_sz);
bool ll_has_view(void);
// Vista corrente come JSON (da liberare con free), NULL se assente. Per debug.
char *ll_view_json(void);

// Entita' citate dalla vista: array NULL-terminated valido fino al prossimo ll_set_config.
const char *const *ll_entity_ids(void);

// Costruisce la vista dentro parent (larghezza w). ll_unbind() prima di
// cancellare gli oggetti LVGL creati.
void ll_build(lv_obj_t *parent, int w);
void ll_unbind(void);

// Avvia il task che scarica i dati dei grafici (una richiesta alla volta,
// aggiornamento ogni 5 minuti). Prende da solo il lock del display.
void ll_charts_start(void);
// Forza il riscaricamento di tutti i grafici (comando UART "refresh").
void ll_charts_refresh(void);

// Entita' da seguire in piu' rispetto alla vista (es. sovrimpressione dello
// standby). Dopo la chiamata va riletto ll_entity_ids() per l'iscrizione.
void ll_set_extra_entities(const char *const *ids);

// Colonne (1-12) e righe che la card occuperebbe sulla griglia, tenendo conto
// di grid_options e dei valori predefiniti per tipo. Le usa l'editor web per
// mostrare le dimensioni vere di partenza. 0 righe = altezza automatica.
int ll_card_columns(const cJSON *card);
int ll_card_rows(const cJSON *card);
// Altezza di una riga della griglia e spazio fra le card, in pixel: servono
// all'editor per disegnare con le stesse proporzioni del pannello.
void ll_grid_metrics(int *row_h, int *gap, int *screen_w, int *screen_h);

// Nome e stato gia' formattati (come sulla dashboard). false se ancora ignota.
bool ll_entity_text(const char *entity_id, char *name, size_t name_sz, char *value, size_t value_sz);

// Aggiorna lo stato memorizzato e i widget collegati a quell'entita'.
void ll_entity_update(const char *entity_id, const char *state, const cJSON *attrs);

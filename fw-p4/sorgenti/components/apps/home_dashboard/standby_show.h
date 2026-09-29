#pragma once
#include <stdbool.h>
#include "cJSON.h"

// Standby del pannello: slideshow di foto dalla SD con sovrimpressione di ora,
// data e alcuni valori di Home Assistant. Overlay su lv_layer_top(): finche'
// e' presente assorbe i tocchi, quindi il tocco che sveglia il pannello non
// preme niente sotto. La logica dei tempi sta in idle_manager.

#define STANDBY_MAX_OVERLAY 4

typedef struct {
    int  slideshow_after_s;              // inattivita' prima dello slideshow
    int  screen_off_after_s;             // durata dello slideshow prima di spegnere
    int  photo_seconds;                  // permanenza di ogni foto
    bool shuffle;                        // ordine casuale
    char folder[64];                     // cartella delle foto (JPEG)
    char overlay[STANDBY_MAX_OVERLAY][64];   // entita' HA in sovrimpressione
    /* Nome da scrivere sopra al valore. Vuoto = quello di Home Assistant.
       Serve perche' i nomi veri sono spesso lunghi e tecnici ("Inverter
       potenza fotovoltaica istantanea") e sullo slideshow c'e' poco posto. */
    char overlay_label[STANDBY_MAX_OVERLAY][40];
    int  n_overlay;
} standby_cfg_t;

#ifdef __cplusplus
extern "C" {
#endif

// Valori predefiniti, poi eventuale /spiffs/standby.json (o il vecchio
// /sdcard/standby.json scritto a mano, se in flash non c'e' ancora niente).
void standby_config_load(standby_cfg_t *cfg);
// Scrive /spiffs/standby.json (la pagina web). Perche' abbia effetto subito
// serve idle_manager_reload_config().
bool standby_config_save(const standby_cfg_t *cfg);
// Legge l'elenco "sovrimpressione" (voci "sensor.x" oppure {"id","nome"}).
// La usa anche il server web per non ripetere le due forme.
void standby_overlay_from_json(const cJSON *arr, standby_cfg_t *cfg);

void standby_show_start(const standby_cfg_t *cfg);   // overlay + rotazione foto
void standby_show_set_dark(bool dark);               // ferma le foto, schermo nero
void standby_show_stop(void);                        // rimuove l'overlay
bool standby_show_active(void);

#ifdef __cplusplus
}
#endif

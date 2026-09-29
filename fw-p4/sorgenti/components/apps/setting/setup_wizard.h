#pragma once
#include <stdbool.h>

// Configurazione guidata del primo avvio.
//
// Passi: Wi-Fi, nome e stanza, indirizzo di Home Assistant, autorizzazione
// (QR da inquadrare col telefono), scelta della dashboard, PIN dello schermo,
// password della pagina web.
//
// Wi-Fi e Home Assistant sono obbligatori, il resto si puo' saltare e fare
// dopo dalle impostazioni, dove resta anche la voce per rifare tutto da capo.
//
// Vive in una finestra sopra a tutto (lv_layer_top), quindi copre il launcher
// senza interferire con le app.

#ifdef __cplusplus
extern "C" {
#endif

bool setup_wizard_needed(void);
bool setup_wizard_active(void);     // finestra aperta adesso     // true finche' non e' stato completato
// phone: ESP_Brookesia_Phone*, serve per aprire l'app Impostazioni al passo
// Wi-Fi. Da chiamare col lock del display.
void setup_wizard_start_with(void *phone, int settings_app_id);
void setup_wizard_start(void);
void setup_wizard_reset(void);      // "rifai la configurazione guidata"

// Per le prove e per chiudere dall'esterno.
void setup_wizard_goto(int step);
void setup_wizard_next(void);
void setup_wizard_close(bool mark_done);

#ifdef __cplusplus
}
#endif

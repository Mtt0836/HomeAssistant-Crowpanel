#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// PIN dello schermo: protegge le schermate che possono rompere il collegamento
// o mostrare le credenziali di casa (rete, Home Assistant, debug, WiFi).
//
// E' una cosa diversa dalla password della pagina web: questo difende da chi ha
// il pannello davanti, quella da chi e' sulla rete. Un PIN di poche cifre non
// puo' valere come credenziale di rete, quindi i due non si scambiano mai.
//
// Del PIN non salviamo le cifre ma solo la loro impronta (PBKDF2-HMAC-SHA256
// con sale casuale). Poiche' un PIN e' corto per definizione, dopo tre
// tentativi sbagliati parte un'attesa che raddoppia ogni volta: e' quella, piu'
// della lunghezza, a rendere inutile provarli tutti.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_AREA_NETWORK = 0,   // Rete
    PIN_AREA_HA,            // Home Assistant
    PIN_AREA_DEBUG,         // Debug
    PIN_AREA_WIFI,          // WiFi
    PIN_AREA_SECURITY,      // questa stessa sezione
    PIN_AREA_COUNT
} pin_area_t;

#define PIN_MIN_LEN 4
#define PIN_MAX_LEN 8

void pin_lock_init(void);

bool pin_lock_is_set(void);
bool pin_lock_set(const char *pin);        // NULL o "" rimuove il PIN
bool pin_lock_verify(const char *pin);     // conta i tentativi sbagliati
uint32_t pin_lock_wait_s(void);            // secondi di attesa residui, 0 = libero

bool pin_lock_required(pin_area_t area);
void pin_lock_set_required(pin_area_t area, bool on);
const char *pin_lock_area_name(pin_area_t area);

// Chiede il PIN con la tastiera a schermo e chiama cb(true) se e' giusto.
// Se l'area non e' protetta (o il PIN e' gia' stato dato di recente) chiama
// subito cb(true) senza disturbare.
typedef void (*pin_lock_cb_t)(bool ok, void *ctx);
void pin_lock_guard(pin_area_t area, pin_lock_cb_t cb, void *ctx);

// Chiede il PIN attuale e lo verifica, anche se l'area non e' protetta
// (serve per cambiarlo o toglierlo).
void pin_lock_ask_verify(const char *title, pin_lock_cb_t cb, void *ctx);

// Chiede una sequenza di cifre senza verificarla (per impostare o confermare
// un PIN nuovo). cb riceve NULL se l'utente annulla.
typedef void (*pin_lock_entry_cb_t)(const char *pin, void *ctx);
void pin_lock_ask_digits(const char *title, const char *hint,
                         pin_lock_entry_cb_t cb, void *ctx);

// Dimentica lo sblocco corrente (alla chiusura dell'app impostazioni).
void pin_lock_forget(void);

// true mentre il tastierino e' a schermo.
bool pin_lock_dialog_active(void);

#ifdef __cplusplus
}
#endif

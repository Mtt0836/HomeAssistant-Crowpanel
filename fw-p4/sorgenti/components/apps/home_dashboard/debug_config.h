#pragma once
#include <stdbool.h>

/* Interruttori di debug (NVS, namespace "dbgcfg"), modificabili dalla
   sezione Debug delle Impostazioni e applicati subito. */

typedef enum { DBG_LOG_NONE = 0, DBG_LOG_ERROR, DBG_LOG_WARN, DBG_LOG_INFO, DBG_LOG_DEBUG } dbg_log_level_t;

typedef struct {
    dbg_log_level_t log_level;   // livello del log sulla seriale
    bool uart_console;           // comandi dalla seriale abilitati
    bool perf_monitor;           // FPS e CPU dell'interfaccia in sovrimpressione
    bool ha_verbose;             // stampa i messaggi scambiati con HA (mai il token)
    bool lvgl_psram;             // oggetti LVGL in PSRAM (true) o RAM interna; vale dal riavvio
} debug_config_t;

#ifdef __cplusplus
extern "C" {
#endif

// Prima di avviare il display: sceglie dove LVGL allochera' i suoi oggetti.
void debug_config_early(void);

// Carica da NVS e applica (da chiamare una volta, dopo l'avvio del display).
void debug_config_init(void);

const debug_config_t *debug_config_get(void);
void debug_config_set(const debug_config_t *c);   // salva e applica

#ifdef __cplusplus
}
#endif

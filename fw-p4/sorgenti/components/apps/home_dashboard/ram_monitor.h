#pragma once
#include <stddef.h>

/* Monitor della memoria: un campione 5 minuti dopo l'avvio e poi ogni ora,
   scritto in /spiffs/ramlog.csv (sopravvive ai riavvii, tenuto sotto i 16 KB).
   Serve a vedere se la RAM interna cala nel tempo (perdite, frammentazione). */

#ifdef __cplusplus
extern "C" {
#endif

void ram_monitor_start(void);

// Registra subito un campione fuori orario (comando UART "ramlog ora").
void ram_monitor_sample_now(void);

// Copia in buf le ultime righe del registro (CSV con intestazione).
size_t ram_monitor_read(char *buf, size_t sz);

#ifdef __cplusplus
}
#endif

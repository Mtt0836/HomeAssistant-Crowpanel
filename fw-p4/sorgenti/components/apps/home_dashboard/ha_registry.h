#pragma once
#include <stdbool.h>

// Elenco delle entita' di Home Assistant raggruppate per dispositivo, per la
// pagina web: serve a scegliere cosa mostrare senza doversi ricordare a
// memoria gli identificativi tipo "sensor.potenza_istantanea".
//
// I nomi dei dispositivi e delle stanze stanno nei registri di HA
// (config/device_registry/list e amici), che pero' sono riservati agli
// amministratori: se il pannello e' abbinato a un utente normale si ripiega
// sull'elenco degli stati, che tutti possono leggere, e i dispositivi non ci
// sono (una sola lista).
//
// Il risultato viene tenuto da parte qualche minuto: aprire la pagina non
// deve far ripartire tre richieste ogni volta.

#ifdef __cplusplus
extern "C" {
#endif

// JSON: {"gruppi":[{"nome":..,"stanza":..,"voci":[{"id":..,"nome":..}]}]}
// Da liberare con free(). NULL se HA non risponde. Blocca qualche secondo.
char *ha_registry_json(void);

// Butta via la copia tenuta da parte (dopo un cambio in HA).
void ha_registry_forget(void);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdbool.h>
#include <stddef.h>

// Lato pannello dell'integrazione per Home Assistant.
//
// Il pannello e' il client: e' lui che tiene aperto il WebSocket verso HA e
// che sa riconnettersi da solo. Quindi non e' HA a cercare il pannello sulla
// rete (niente porte da aprire, niente indirizzo da scrivere, niente
// certificato autofirmato di mezzo): e' il pannello che, appena il
// collegamento e' pronto, si presenta.
//
//   pannello -> HA   "crowpanel/announce"   chi sono e cosa so fare
//                    "crowpanel/state"      come sto adesso
//   HA -> pannello   evento "crowpanel_command" sul bus
//
// Se l'integrazione non e' installata, HA risponde "comando sconosciuto":
// il pannello lo annota una volta e continua a funzionare come sempre.

#ifdef __cplusplus
extern "C" {
#endif

// Da chiamare una volta all'avvio, dopo ha_ws_start.
void ha_plugin_start(void);

// true se l'ultimo annuncio e' andato a buon fine: l'integrazione c'e'.
bool ha_plugin_paired(void);

// true se la configurazione dello slideshow la decide Home Assistant.
// La pagina web mostra quella parte in sola lettura.
bool ha_plugin_owns_slideshow(void);

// "Riprendi il comando da qui": il pannello torna a gestirsi lo slideshow
// (serve se l'integrazione viene tolta da HA e nessuno lo dice al pannello).
void ha_plugin_release_slideshow(void);

// Identificativo del pannello per HA (MAC senza separatori). Sempre lo stesso.
void ha_plugin_id(char *out, size_t sz);

// Perche' il pannello si e' riavviato l'ultima volta, in parole ("crash",
// "accensione", "riavvio richiesto"...). Lo manda a HA e lo stampa "info":
// dopo un crash il pannello riparte da solo, e senza questo nessuno se ne
// accorgerebbe.
const char *ha_plugin_reset_reason(void);

#ifdef __cplusplus
}
#endif

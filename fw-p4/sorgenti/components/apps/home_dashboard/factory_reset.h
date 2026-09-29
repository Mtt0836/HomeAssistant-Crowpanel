#pragma once
#include <stdbool.h>

// Ritorno alle impostazioni iniziali: il pannello dimentica tutto e riparte
// come appena tolto dalla scatola (configurazione guidata al primo avvio).
//
// Viene cancellato:
//   - Wi-Fi memorizzato (sia quello del driver sia la copia usata dal recupero)
//   - indirizzo di Home Assistant, abbinamento, token, dashboard scelta
//   - nome, stanza e impostazioni di rete
//   - PIN dello schermo e password della pagina web
//   - certificato HTTPS (ne verra' generato uno nuovo, con impronta diversa)
//   - ritocchi locali della dashboard, preferenze del launcher, log
//
// Il lavoro avviene in un task a parte: cancellare la memoria e fermare la
// rete puo' richiedere qualche istante e non deve bloccare il disegno dello
// schermo. Al termine il pannello si riavvia da solo.

#ifdef __cplusplus
extern "C" {
#endif

// Avvia la cancellazione. Ritorna subito; il riavvio arriva dopo ~1 secondo.
// Chiamabile col lock del display preso.
void factory_reset_start(void);

// Cancella e riavvia nel task chiamante (non ritorna). Per la console.
void factory_reset_now(void);

#ifdef __cplusplus
}
#endif

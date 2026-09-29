#pragma once

// Un messaggio di Home Assistant scritto sullo schermo del pannello.
//
// Il servizio crowpanel.avviso sapeva solo leggere ad alta voce, e quella e'
// una strada sola: chi non e' nella stanza lo perde, e chi non ha collegato
// l'altoparlante non lo riceve affatto. Adesso l'avviso compare anche scritto,
// in alto, e se ne va da solo dopo qualche secondo. Si puo' togliere prima
// toccandolo.
//
// Si puo' chiamare da qualunque task: prende il lock del display per conto suo.

#ifdef __cplusplus
extern "C" {
#endif

void avviso_ui_mostra(const char *testo);

#ifdef __cplusplus
}
#endif

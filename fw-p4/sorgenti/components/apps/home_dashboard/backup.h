#pragma once

/* Salvataggio e ripristino della configurazione del pannello.

   COSA C'E' DENTRO. Tutto quello che rende questo pannello *questo* pannello:
   l'indirizzo di Home Assistant e il permesso per entrarci, la dashboard
   scelta, le password della pagina web, il PIN dello schermo, le impostazioni
   di rete, il certificato HTTPS, i ritocchi alle card e la configurazione
   dello slideshow.

   La configurazione non si raccoglie da un elenco scritto a mano: si percorre
   la memoria NVS voce per voce. Un elenco andrebbe aggiornato ogni volta che
   qualcuno aggiunge un'impostazione, e il giorno in cui qualcuno se ne
   dimentica il salvataggio diventa incompleto senza dirlo. Percorrendo la
   memoria, quello che c'e' viene portato via, punto.

   DUE MODI, E NON SONO EQUIVALENTI.

   - Cifrato: contiene tutto, comprese le cose segrete. Serve la chiave di
     recupero per riaprirlo. E' il salvataggio con cui si rimette in piedi un
     pannello da zero.

   - In chiaro: leggibile con un editor di testo, ma senza le cose segrete -
     il permesso di Home Assistant, la chiave privata del certificato, le
     impronte di password e PIN. Nel file c'e' scritto cosa e' stato lasciato
     fuori, cosi' non si scopre al momento del ripristino.

   Il motivo di questa differenza e' semplice: il permesso di Home Assistant
   apre tutta la casa. Un file che lo contiene in chiaro finisce per posta, in
   un archivio, in un repository - e da quel momento chiunque lo legga entra.
   Chi vuole il salvataggio completo lo cifra; chi vuole solo leggere le
   proprie impostazioni non ha bisogno dei segreti.

   LA CHIAVE DI RECUPERO nasce al primo avvio, resta nel pannello e si scarica
   quando si vuole. Non finisce mai dentro un salvataggio - sarebbe come
   chiudere una cassetta e lasciarci sopra la chiave.
*/

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vero se questa voce della NVS e' un segreto: il permesso di Home Assistant,
   la chiave privata del certificato, le impronte di password e PIN.

   Sta qui, in chiaro nell'intestazione, perche' l'elenco dei segreti deve
   esistere in UN SOLO posto. Lo usa il salvataggio in chiaro per lasciarli
   fuori, e lo usa chiunque mostri la configurazione a qualcuno - per esempio
   l'esploratore della NVS nella pagina web, che dei segreti mostra che
   esistono e non il valore. Con due elenchi separati, il giorno che si
   aggiunge un segreto se ne aggiorna uno e si dimentica l'altro, e il permesso
   di Home Assistant finisce su una pagina web. */
bool backup_e_segreto(const char *ns, const char *chiave);

/* La chiave di recupero in forma leggibile (gruppi di quattro caratteri).
   La crea al primo uso. Il buffer vuole almeno BACKUP_CHIAVE_MAX byte. */
#define BACKUP_CHIAVE_MAX 80
bool backup_chiave_testo(char *out, size_t out_sz);

/* Vero se l'utente l'ha gia' scaricata almeno una volta: serve alla
   configurazione guidata per insistere finche' non l'ha messa al sicuro. */
bool backup_chiave_gia_presa(void);
void backup_chiave_segna_presa(void);

/* Costruisce il salvataggio. Il testo va liberato con free().
   Ritorna NULL se qualcosa va storto. */
char *backup_esporta(bool cifrato);

/* Rimette in piedi la configurazione da un salvataggio.
   "chiave" serve solo per i file cifrati: se e' NULL o vuota si usa quella
   del pannello. Ritorna false e riempie "err" se non si puo' fare.
   Dopo un ripristino riuscito il pannello va riavviato. */
bool backup_importa(const char *json, const char *chiave, char *err, size_t err_sz);

/* Prova la catena completa senza scrivere niente: esporta, cifra, decifra,
   rilegge, e controlla che quello che torna indietro sia quello che era
   uscito. Prova anche che una chiave sbagliata venga rifiutata.
   Scrive il resoconto in "out" (nessun contenuto della configurazione: solo
   quante voci e se torna). */
bool backup_autoprova(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Esploratore: cosa c'e' sulla scheda SD, in SPIFFS e nella memoria NVS.

   A COSA SERVE. A guardare dentro il pannello senza smontarlo. Durante una
   taratura della batteria, per dire, il pannello e' proprio la cosa che non si
   deve toccare, e la scheda SD non si puo' estrarre; ma il registro lo si vuole
   leggere. Lo stesso valeva per le foto che non partivano: la cartella era
   vuota e per scoprirlo e' servito un comando sulla seriale.

   DUE FACCE, UN MOTORE. La pagina web elenca, scarica e carica; l'app sul
   pannello elenca e mostra lo spazio libero, e non scrive niente - su un
   touch senza tastiera non c'e' niente che valga il rischio di sbagliare.
   Quello che sta qui dentro non sa niente ne' di HTTP ne' di LVGL: elenca,
   misura, e dice se un percorso e' ammesso.

   CANCELLARE SI PUO', MA CHIEDENDO. All'inizio qui non c'era nessuna funzione
   per cancellare, di proposito: da un elenco si clicca sulla riga sbagliata con
   una facilita' che non ha pari, e qui non c'e' nessun cestino. Il divieto
   pero' costringeva a estrarre la scheda per togliere una foto, che e' peggio -
   si spegne il pannello per una cosa da due secondi. Quindi la funzione c'e',
   e la difesa non e' il divieto ma la conferma: chi chiama DEVE chiedere, e
   l'interfaccia mostra il nome di quello che sta per sparire.

   Solo file e cartelle vuote: una cartella piena non si cancella per sbaglio
   con un dito.

   I SEGRETI DELLA NVS NON SI MOSTRANO. Dentro la NVS ci sono il permesso di
   Home Assistant, la chiave privata del certificato e le impronte di password
   e PIN. Il permesso di Home Assistant apre tutta la casa: su una pagina web
   non ci va. Di quelle voci si mostra che esistono, il tipo e la dimensione,
   mai il valore. L'elenco di cosa sia segreto NON e' ricopiato qui: si chiede
   a backup_e_segreto(), che e' lo stesso elenco che usa il salvataggio in
   chiaro. Un elenco solo, in un posto solo. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     nome[72];
    bool     cartella;
    uint32_t byte;
    time_t   quando;        // 0 se il filesystem non la sa (SPIFFS non la sa)
} esplora_voce_t;

/* Le radici consentite, per costruire l'interfaccia senza cablarle due volte.
   esplora_radice(i) ritorna NULL quando sono finite. */
const char *esplora_radice(int i);

/* Vero se il percorso si puo' aprire: dentro una delle radici, senza ".." e
   senza caratteri di controllo. Tutto quello che arriva da fuori - un indirizzo
   web, un tocco - passa da qui prima di diventare una fopen o una opendir. */
bool esplora_permesso(const char *percorso);

/* Elenca una cartella, cartelle prima e poi file, in ordine alfabetico.
   Ritorna il numero di voci scritte, oppure -1 se il percorso non e' ammesso o
   non si apre. Se c'erano piu' di "max" voci mette *troncato a true: meglio un
   elenco incompleto e dichiarato che un elenco che finisce la memoria. */
int esplora_elenca(const char *percorso, esplora_voce_t *v, int max, bool *troncato);

/* Cancella un file, o una cartella se e' vuota. Ritorna false - e lascia tutto
   com'era - se il percorso non e' ammesso, se non esiste, o se e' una cartella
   con dentro qualcosa.

   Chi chiama deve aver gia' chiesto conferma all'utente: qui non si chiede
   niente a nessuno e non si torna indietro. */
bool esplora_elimina(const char *percorso);

// Spazio della radice che contiene il percorso. false se non si sa.
bool esplora_spazio(const char *percorso, uint64_t *totale, uint64_t *libero);

/* Una voce della NVS. "valore" e' testo pronto da mostrare; per i segreti e'
   vuoto e "segreto" e' vero. */
typedef struct {
    char     spazio[20];     // namespace
    char     chiave[20];
    char     tipo[12];       // "u8", "i32", "testo", "blob", ...
    bool     segreto;
    uint32_t byte;           // dimensione per testi e blob, 0 per i numeri
    char     valore[112];
} esplora_nvs_t;

/* Percorre tutta la NVS. Come il salvataggio, non segue un elenco scritto a
   mano: quello che c'e' si vede. Ritorna quante voci, -1 se non si puo'
   leggere. */
int esplora_nvs(esplora_nvs_t *v, int max, bool *troncato);

#ifdef __cplusplus
}
#endif

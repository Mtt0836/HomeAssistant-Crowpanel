#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Aggiornamento del firmware via rete.

   COME FUNZIONA UN OTA, IN DUE RIGHE. Un ESP non puo' riscrivere la partizione
   da cui sta eseguendo. Ce ne sono due, ota_0 e ota_1: si scrive in quella
   ferma, e solo quando l'immagine e' completa e verificata si dice al
   bootloader di partire da li'. Finche' la verifica non passa, il pannello
   continua a girare su quella di prima e non si e' rischiato niente.

   LA VERIFICA, E PERCHE' E' DOPPIA. ESP-IDF controlla per conto suo che
   l'immagine sia un'applicazione valida per questo chip. Quel controllo pero'
   dice "e' un firmware", non "e' IL firmware che e' stato mandato". Qui si
   aggiunge un'impronta SHA-256 calcolata da chi manda, prima di spedire, e
   ricalcolata dal pannello sui byte man mano che arrivano. Se le due non
   coincidono l'immagine viene buttata e il bootloader non la vede nemmeno.
   Copre la trasmissione da un capo all'altro: rete, WiFi, SDIO, scrittura in
   flash.

   SHA-256 e non CRC32: un controllo a 32 bit su quattro megabyte e mezzo e'
   poco, e soprattutto non distingue un'immagine rovinata da una sostituita. Il
   P4 ha l'acceleratore hardware, quindi su 4,4 MB sono frazioni di secondo.

   IL RITORNO INDIETRO. Un firmware appena installato parte "in prova". Diventa
   definitivo solo dopo che il pannello e' rimasto in piedi e collegato a Home
   Assistant per due minuti. Se si riavvia prima - crash, boot loop, rete che
   non sale - il bootloader rimette quello di prima da solo. Senza questo, il
   primo aggiornamento sbagliato costringerebbe a staccare il pannello dal muro
   e riflasharlo via USB: un aggiornamento via rete che puo' murare il
   dispositivo non e' un aggiornamento via rete. E' lo stesso meccanismo che il
   C6 ha gia' nel suo rollback_guard. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_FERMO = 0,
    OTA_SCARICA,
    OTA_VERIFICA,
    OTA_PRONTO,      // installato, aspetta il riavvio
    OTA_FALLITO,
} ota_stato_t;

typedef struct {
    ota_stato_t stato;
    int      pct;
    uint32_t byte_fatti;
    uint32_t byte_attesi;
    char     messaggio[112];
    bool     in_prova;        // questa immagine deve ancora essere confermata
} ota_info_t;

/* Da chiamare all'avvio. Se l'immagine in esecuzione e' in prova, resta in
   attesa che il pannello dimostri di funzionare e poi la conferma. */
void ota_update_init(void);

/* Comincia un aggiornamento. "sha256" sono 64 caratteri esadecimali;
   "byte" e' la dimensione attesa, 0 se non la si sa.
   Ritorna false subito se non si puo' nemmeno cominciare. */
bool ota_update_avvia(const char *url, const char *sha256, uint32_t byte);

void ota_update_stato(ota_info_t *out);

/* Lo stato in una parola, per Home Assistant e per la console: "fermo",
   "scarica", "verifica", "pronto", "fallito". */
const char *ota_parole(ota_stato_t s);
bool ota_update_in_corso(void);

// Versione e data del firmware in esecuzione, per l'elenco in Home Assistant.
void ota_update_versione(char *out, size_t sz);

#ifdef __cplusplus
}
#endif

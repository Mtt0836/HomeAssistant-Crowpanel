/*
 * Ripristino automatico dell'immagine precedente (rollback applicativo).
 *
 * Il bootloader di fabbrica del C6 non ha CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 * e via SDIO non si puo' riscrivere in sicurezza: questo modulo fa lo stesso
 * lavoro dall'applicazione. Un'immagine appena installata resta "in prova"
 * finche' il collegamento col P4 non lavora stabilmente; se non ci riesce per
 * RB_MAX_BOOTS avvii di fila, il C6 torna all'altro slot.
 * Un'immagine confermata una volta non viene mai piu' toccata: in produzione
 * (P4 spento, flash del P4, riavvii a catena) il ripristino non puo' scattare.
 */
#pragma once
#include <stdbool.h>

/* Valore del campo "build" nella versione firmware letta dal P4 (comando fw):
   distingue questa immagine da quella di fabbrica/precedente (-1). */
#define RB_FW_BUILD 2

/* Da chiamare in app_main subito dopo nvs_flash_init(). Puo' non ritornare
   (riavvio sull'altro slot). */
void rollback_guard_init(void);

/* Il P4 ha aperto/chiuso il canale dati. Sicura da ISR. */
void rollback_guard_datapath(bool open);

/* Arrivato un pacchetto dal P4. Sicura da qualunque task. */
void rollback_guard_host_pkt(void);

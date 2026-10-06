#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* La batteria del pannello: lettura, stima della carica e autotaratura.

   COSA LEGGE E COSA NO. Sulla scheda c'e' un coprocessore STC8H1Kxx che
   sorveglia l'alimentazione e risponde sull'I2C. Il suo firmware non e' nostro
   e non lo tocchiamo: da qui si legge e basta. Manda tensione, una sua
   percentuale, lo stato di carica e lo stato del led.

   PERCHE' NON USIAMO LA SUA PERCENTUALE. Due motivi, indipendenti.

   Primo: mentre la batteria carica, la tensione ai morsetti non e' quella
   della cella. E' quella del caricatore. Una litio sotto carica sta a 4,2 V
   molto prima di essere piena, quindi qualunque mappa tensione->percentuale
   legge alto; e appena si stacca l'alimentazione la tensione torna al valore
   vero e la percentuale crolla. Chi guarda vede un numero che balla.

   Secondo: anche a riposo la curva di una litio e' piatta. Fra 3,9 e 3,6 V ci
   sta circa l'80% della capacita'. Una mappa lineare sbaglia in tutta la
   parte centrale, che e' proprio quella dove si passa la vita.

   COME LA CALCOLIAMO NOI. Con una tabella tensione->percentuale a 21 punti,
   interpolata. Di partenza e' quella standard di una litio a cella singola,
   che sbaglia meno di una retta ma non sa niente di questo pannello.

   L'AUTOTARATURA. Manca un sensore di corrente, ma non serve. Un pannello a
   riposo assorbe sempre piu' o meno la stessa corrente, e con corrente
   costante il tempo E' la misura della carica consumata: al 40% del tempo di
   una scarica completa si e' al 60% di carica. Quindi una scarica integrale
   registrata minuto per minuto da' la curva vera di QUESTA cella su QUESTA
   scheda - capacita' e consumo compresi, che nessuna tabella standard puo'
   conoscere. Quando il modulo riconosce una scarica completa e credibile, ne
   ricava i 21 punti e se li tiene in NVS.

   Il limite, scritto chiaro: l'assunzione e' che il consumo resti costante.
   Durante una scarica vera lo schermo si spegne dopo pochi minuti e da li'
   non cambia piu' niente per ore, quindi regge; per non buttare via i primi
   minuti il tempo e' pesato secondo lo stato dello schermo (vedi PESO_ACCESO).
   Il peso e' una stima, e sbagliarlo sposta solo quei pochi minuti su molte
   ore. Il registro su SD tiene comunque le colonne grezze, cosi' la curva si
   puo' ricontrollare a mano sul computer: l'autotaratura non e' una scatola
   chiusa. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATT_SCONOSCIUTO = 0,   // non ancora letta, o il coprocessore non risponde
    BATT_IN_CARICA,
    BATT_CARICA,            // il coprocessore la da' per piena
    BATT_A_BATTERIA,        // alimentazione staccata, scarica in corso
    BATT_ERRORE,
} batt_stato_t;

#define BATT_PUNTI 21       // 0%, 5%, ... 100%

typedef struct {
    bool         letta;             // l'ultima lettura I2C e' andata a buon fine
    uint32_t     mv;                // tensione grezza del coprocessore
    uint32_t     mv_filtrato;       // la stessa, senza il tremolio
    /* La tensione "a riposo": quella misurata, corretta per cio' che in questo
       momento la falsa - il caricatore che la alza, lo schermo acceso che la
       abbassa. E' questa che si confronta con la curva, perche' la curva e'
       fatta di tensioni a riposo. */
    uint32_t     mv_riposo;
    uint32_t     caduta_schermo_mv; // quanto costa lo schermo acceso, misurato
    uint8_t      pct;               // la nostra stima
    uint8_t      pct_stc8;          // quella del coprocessore, per confronto
    batt_stato_t stato;
    uint8_t      stato_grezzo;      // il numero come arriva, per non perderlo
    uint8_t      led_grezzo;
    bool         curva_imparata;
    uint32_t     curva_quando;      // unix time della taratura, 0 = mai
    uint32_t     curva_durata_s;    // quanto e' durata la scarica che l'ha prodotta
    uint32_t     offset_carica_mv;  // di quanto la carica gonfia la tensione
    bool         corsa_attiva;      // una scarica e' in corso di registrazione
    /* Le due correnti misurate a pinza, se qualcuno le ha date (0 = no).
       Trasformano i secondi pesati in mAh, e quindi danno le due cose che su
       un pannello appeso al muro contano piu' della percentuale: la capacita'
       vera del pacco e quanto tempo resta durante un black-out. */
    uint16_t     ma_spento;
    uint16_t     ma_acceso;
    uint16_t     capacita_mah;      // misurata sulla scarica di taratura, 0 se ignota
    uint16_t     autonomia_min;     // stima col consumo attuale, 0 se ignota
} batt_info_t;

/* Avvia la lettura periodica, il registro su SD e l'autotaratura.
   Sostituisce il task battery_info_task di main.cpp. */
void batteria_avvia(void);

// Lo stato corrente. Si puo' chiamare da qualunque task.
void batteria_leggi(batt_info_t *out);

// "in carica", "carica", "a batteria", ... per Home Assistant e per la console.
const char *batteria_stato_parole(batt_stato_t s);

// Diagnostica leggibile (comando console "batteria").
void batteria_diagnostica(char *out, size_t sz);

/* Butta via la curva imparata e torna alla tabella standard
   (comando console "batteria azzera"). */
void batteria_azzera_curva(void);

/* Le due correnti assorbite dal pannello, in mA, misurate a pinza sul filo
   della batteria: una a schermo spento e una a schermo acceso (comando
   console "batteria ma <spento> <acceso>"). Zero le dimentica.

   Non servono alla percentuale - quella viene dalla curva - ma trasformano il
   tempo in carica reale: da qui escono la capacita' misurata del pacco e
   l'autonomia residua. Il rapporto fra le due sostituisce anche PESO_ACCESO,
   che senza di loro resta una stima. */
void batteria_imposta_correnti(uint16_t ma_spento, uint16_t ma_acceso);

#ifdef __cplusplus
}
#endif

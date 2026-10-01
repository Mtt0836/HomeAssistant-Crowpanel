#pragma once

/* Il modello dati del pannello Energia di Home Assistant.

   Questo file non disegna niente. Legge le preferenze Energia di HA, chiede le
   statistiche del periodo scelto e ne ricava i numeri; il disegno sta altrove.
   La separazione non e' un vezzo: questa e' la parte che deve valere per
   chiunque. Ogni installazione di Home Assistant combina rete, fotovoltaico,
   batteria, gas e acqua a modo suo - c'e' chi ha solo il contatore, chi ha i
   pannelli senza batteria, chi ha tre inverter - e nessuna di queste forme puo'
   essere data per scontata.

   Cosa NON si fa qui: giudicare la configurazione dell'utente. Se qualcuno ha
   messo i sensori del fotovoltaico al posto di quelli della rete, il pannello
   mostrera' quello che mostra anche la pagina Energia di Home Assistant. Il
   pannello e' uno specchio, non un revisore.
*/

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <vector>

/* Le finestre che si possono scegliere. Corrispondono a quelle della pagina
   Energia di HA, e ognuna porta con se' il passo con cui si chiedono i dati:
   a ore per un giorno, a giorni per il resto. */
enum EnergyPeriodo {
    EN_OGGI = 0,
    EN_SETTIMANA,
    EN_MESE,
    EN_ANNO,
};

/* Una voce della tabella delle sorgenti o dei dispositivi. */
struct EnergyVoce {
    std::string id;        // statistic_id
    std::string nome;      // come lo chiama l'utente, o l'id se non ha nome
    double      totale = 0;
    bool        visto = false;   // HA ha risposto per questo id
};

struct EnergyModel {
    // --- stato ---
    bool        prefs_lette = false;
    bool        dati_pronti = false;
    std::string errore;            // vuoto = nessun errore

    EnergyPeriodo periodo = EN_OGGI;
    int64_t     t0 = 0, t1 = 0;    // finestra in secondi UNIX
    int64_t     aggiornato_s = 0;  // 0 = mai

    // --- cosa c'e' in questa installazione ---
    bool c_e_rete = false, c_e_solare = false, c_e_batteria = false;
    bool c_e_gas = false, c_e_acqua = false;
    bool c_e_dispositivi = false;

    // --- totali del periodo, in kWh (gas e acqua nella loro unita') ---
    double rete_presa = 0;         // dalla rete verso casa
    double rete_immessa = 0;       // da casa verso la rete
    double solare = 0;
    double batteria_scarica = 0;   // dalla batteria verso casa
    double batteria_carica = 0;    // da casa verso la batteria
    double gas = 0, acqua = 0;
    std::string unita_gas, unita_acqua;   // come le chiama HA: m3, ft3, L, gal...
    double costo = 0, compenso = 0;
    bool   c_e_costo = false;

    /* Il consumo di casa non e' un sensore: si ricava. E' la stessa formula
       della pagina Energia di HA. */
    double casa = 0;

    // --- percentuali, gia' limitate a 0..100 (negative = non calcolabili) ---
    double autosufficienza = -1;   // quanta parte dei consumi NON viene dalla rete
    double solare_usato = -1;      // quanta parte del prodotto e' rimasta in casa
    double neutralita = -1;        // immesso rispetto al totale scambiato

    // --- tabella ---
    std::vector<EnergyVoce> sorgenti;
    std::vector<EnergyVoce> dispositivi;

    // --- serie per i grafici: npunti fasce larghe passo_s ---
    int     npunti = 0;
    int64_t passo_s = 3600;
    std::vector<float> g_rete_presa, g_rete_immessa, g_solare;
    std::vector<float> g_batt_scarica, g_batt_carica, g_gas, g_acqua;
};

/* La fotografia corrente. Non e' mai NULL. Va letta con il lock del display
   preso, perche' e' lo stesso filo che la riscrive. */
const EnergyModel *energy_model_get(void);

/* Cambia finestra e fa ripartire la raccolta. */
void energy_model_set_periodo(EnergyPeriodo p);

/* Butta i dati e li richiede: lo chiama chi si ricollega a HA o chi preme
   "aggiorna". Le preferenze si rileggono solo se "anche_prefs". */
void energy_model_invalida(bool anche_prefs);

/* Accende la raccolta anche se nella vista non c'e' nessuna card: serve al
   comando "energia" della console per provare la configurazione di chi non ha
   ancora messo le card, o per capire perche' i numeri non tornano. Una volta
   acceso resta acceso fino al riavvio. */
void energy_model_forza(void);

/* Vero se in questa vista c'e' almeno una card dell'Energia: finche' non ce
   n'e', non si chiede niente a Home Assistant. */
void energy_model_serve(bool serve);

/* Chiamata dal task che fa le richieste, uno alla volta (le raffiche verso il
   C6 sono proprio cio' che il collegamento SDIO regge peggio).
   Riempie "body" e restituisce true se c'e' qualcosa da chiedere. */
bool energy_model_prossima_richiesta(std::string &body);

/* Chi vuole essere avvisato quando i numeri cambiano, per ridisegnare. */
typedef void (*energy_model_cb_t)(void);
void energy_model_on_change(energy_model_cb_t cb);

/* Nome leggibile di un periodo, per i pulsanti e i titoli. */
const char *energy_periodo_nome(EnergyPeriodo p);

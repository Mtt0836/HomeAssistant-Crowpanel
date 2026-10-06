#pragma once
#include "lvgl.h"
#include "esp_brookesia.hpp"

/* App "Esplora" per il launcher: cosa c'e' sulla scheda SD e in SPIFFS.

   PERCHE' SUL PANNELLO E NON SOLO NELLA PAGINA WEB. Perche' la domanda "le
   foto ci sono?" o "quanto spazio e' rimasto?" viene in mente stando davanti
   al pannello, non davanti al computer. Rispondere senza andare a prendere un
   portatile e' il valore di questa app.

   COSA NON FA, E DI PROPOSITO. Non scarica, non carica, non cancella, non
   mostra la NVS.

   Non scarica ne' carica perche' un pannello appeso al muro non ha dove mettere
   le cose ne' da dove prenderle: quelle operazioni hanno senso solo dalla
   pagina web, dove c'e' un computer dall'altra parte.

   Non cancella perche' un dito su uno schermo da dieci pollici sbaglia riga
   facilmente, e qui non c'e' nessun cestino.

   Non mostra la NVS perche' li' dentro ci sono i segreti - il permesso di Home
   Assistant, le impronte delle password - e il pannello e' appeso in casa dove
   chiunque passi lo puo' toccare. La pagina web almeno chiede una password.
   Quindi la NVS si guarda da li', dove l'accesso e' protetto. */
class EsploraApp : public ESP_Brookesia_PhoneApp {
public:
    EsploraApp();
    ~EsploraApp();
    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_obj_t *_root = nullptr;
    lv_obj_t *_dove = nullptr;      // il percorso in alto
    lv_obj_t *_lista = nullptr;
    lv_obj_t *_spazio = nullptr;    // lo spazio libero in basso
    char      _percorso[256] = "/sdcard";

    /* Il visualizzatore di testo. Non carica il file intero: tiene una
       finestra e la sposta. Un registro della batteria da novanta kilobyte
       dentro una lv_label vorrebbe dire novanta kilobyte di testo piu' tutto
       il lavoro di disegno a ogni scorrimento. */
    lv_obj_t *_vis = nullptr;       // lo strato del visualizzatore
    lv_obj_t *_vis_testo = nullptr;
    lv_obj_t *_vis_titolo = nullptr;
    char      _vis_file[256] = "";
    long      _vis_da = 0;          // byte da cui comincia la pagina mostrata
    long      _vis_dim = 0;         // dimensione del file
    /* Il font: di partenza quello normale, lo stesso dell'elenco, perche' e'
       quello che si legge. Il tasto "Aa" passa a quello a larghezza fissa, che
       serve quando contano le colonne - un CSV, un log - ma che a schermo e'
       piu' faticoso. All'inizio era il contrario e il csv si leggeva male. */
    bool      _vis_fisso = false;
    lv_obj_t *_vis_img = nullptr;   // quando si sta guardando una figura

    void costruisci(void);
    void aggiorna(void);
    void vai(const char *percorso);

    bool e_testo(const char *nome) const;
    bool e_figura(const char *nome) const;
    void apri_testo(const char *percorso);
    void apri_figura(const char *percorso);
    lv_obj_t *telaio_vista(const char *titolo);   // la cornice comune alle due viste
    void chiedi_elimina(const char *percorso);
    void mostra_pagina(long da);
    void chiudi_testo(void);

    static void su_voce(lv_event_t *e);
    static void su_vis(lv_event_t *e);
    static void su_cestino(lv_event_t *e);
    static void su_conferma(lv_event_t *e);
};

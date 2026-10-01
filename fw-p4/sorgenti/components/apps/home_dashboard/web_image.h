#pragma once

/* Le immagini che arrivano da Home Assistant.

   Le card "picture" non mostrano uno stato: mostrano una figura, che va
   scaricata. Questo modulo la prende, la mette da parte e la consegna pronta
   da appendere a un oggetto LVGL.

   DOVE FINISCONO. In una cartella sulla scheda SD, non in memoria e non in
   flash. Sono dati voluminosi e di dimensione imprevedibile - una foto di una
   telecamera, lo sfondo di una planimetria - e tenerli nella RAM del pannello
   vorrebbe dire litigare con tutto il resto. Sulla SD invece ci stanno, e
   restano fra un riavvio e l'altro: la seconda volta la card si disegna
   subito, senza chiedere niente a nessuno.

   Senza scheda SD si scarica lo stesso, ogni volta, in PSRAM. Il pannello deve
   funzionare anche senza: la SD qui e' una comodita', non un pezzo portante.

   DUE FORMATI, DUE STRADE. Il JPEG lo decodifica l'acceleratore dentro il P4,
   che e' veloce e non costa flash. Il PNG no, non c'e' un acceleratore: lo fa
   LVGL a software, leggendolo direttamente dal file. Per questo il modulo
   restituisce un percorso e non dei pixel: chi disegna sceglie la strada
   secondo il formato.
*/

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_IGNOTA = 0,
    IMG_JPEG,
    IMG_PNG,
} web_image_tipo_t;

#define WEB_IMAGE_PATH_MAX 96

/* Dove sta l'immagine, una volta presa. */
typedef struct {
    char             percorso[WEB_IMAGE_PATH_MAX];  // file locale, vuoto se non c'e'
    char             lvgl[WEB_IMAGE_PATH_MAX + 4];  // lo stesso con la lettera di LVGL
    web_image_tipo_t tipo;
} web_image_t;

/* Prende l'immagine indicata dalla card.

   "src" puo' essere un indirizzo intero, oppure un percorso che comincia per
   "/" - come lo scrive Home Assistant per le sue cose ("/local/casa.png",
   "/api/camera_proxy/camera.ingresso") - e allora ci si attacca davanti
   l'indirizzo di HA e il permesso.

   Non blocca: se l'immagine non e' ancora pronta ritorna false e la mette in
   coda; chiamarla ancora piu' tardi la trovera'. Chi disegna puo' intanto
   mostrare un riquadro vuoto.

   "subito" salta la cache e riscarica: serve alle telecamere, che cambiano. */
bool web_image_prendi(const char *src, web_image_t *out, bool subito);

/* Appende al contenitore l'immagine, scegliendo da se' la strada giusta per
   il formato. Ritorna l'oggetto creato, o NULL. */
lv_obj_t *web_image_mostra(lv_obj_t *parent, const web_image_t *img, int w, int h);

/* Avvisa quando un'immagine chiesta prima e' arrivata, cosi' la card si
   ridisegna. */
typedef void (*web_image_cb_t)(void);
void web_image_on_arrivo(web_image_cb_t cb);

/* Scarica un'immagine di Home Assistant dentro un file preciso.

   La usa il trasferimento delle foto dello slideshow, che non vanno nella
   cache ma nella cartella delle foto e ci restano. Lavora senza tornare
   finche' non ha finito, quindi va chiamata da un task suo.

   Il file prende il suo nome solo a scaricamento riuscito: una caduta di rete
   non deve lasciare mezza foto in mezzo alle altre, che poi lo slideshow
   proverebbe a mostrare a ogni giro. */
bool web_image_scarica_file(const char *src, const char *destinazione);

/* Butta la cache: la chiama il ripristino di fabbrica. */
void web_image_svuota(void);

#ifdef __cplusplus
}
#endif

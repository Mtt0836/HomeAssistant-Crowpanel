#include "EsploraApp.hpp"
#include "home_dashboard/esplora.h"
#include "home_dashboard/web_image.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "esplora_app";

LV_IMG_DECLARE(img_app_esplora);

// Quante righe si disegnano. Oltre, la lista si taglia e lo dice.
#define RIGHE_MAX 120

EsploraApp::EsploraApp():
    ESP_Brookesia_PhoneApp("Esplora", &img_app_esplora, true) {}
EsploraApp::~EsploraApp() {}

bool EsploraApp::init(void) { return true; }

// ------------------------------------------------------------------ misure

/* Byte in una forma che si legge a colpo d'occhio. Una scheda da 32 GB scritta
   in megabyte non si legge: 30436 MB non dice niente a nessuno. */
static void misura(uint64_t n, char *out, size_t sz)
{
    if (n < 1024ull)                     snprintf(out, sz, "%u B", (unsigned)n);
    else if (n < 1024ull * 1024)         snprintf(out, sz, "%.1f kB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024)  snprintf(out, sz, "%.1f MB", n / 1048576.0);
    else                                 snprintf(out, sz, "%.2f GB", n / 1073741824.0);
}

// ------------------------------------------------------------------ interfaccia

void EsploraApp::costruisci(void)
{
    _root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(_root);
    lv_obj_set_size(_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_root, lv_color_hex(0x16181d), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_root, 12, 0);
    lv_obj_set_style_pad_row(_root, 8, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    // riga delle radici
    lv_obj_t *barra = lv_obj_create(_root);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(barra, 8, 0);
    lv_obj_clear_flag(barra, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; esplora_radice(i); i++) {
        const char *r = esplora_radice(i);
        lv_obj_t *b = lv_btn_create(barra);
        lv_obj_set_height(b, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3142), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, r);
        lv_obj_center(l);
        /* Il percorso viaggia nello user_data del bottone: un puntatore a una
           costante del programma, che non muore e non va liberato. */
        lv_obj_add_event_cb(b, su_voce, LV_EVENT_CLICKED, (void *)r);
    }

    _dove = lv_label_create(_root);
    lv_obj_set_style_text_color(_dove, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(_dove, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_dove, LV_PCT(100));
    lv_label_set_text(_dove, "");

    _lista = lv_list_create(_root);
    lv_obj_set_width(_lista, LV_PCT(100));
    lv_obj_set_flex_grow(_lista, 1);
    lv_obj_set_style_bg_color(_lista, lv_color_hex(0x1b2030), 0);
    lv_obj_set_style_border_width(_lista, 0, 0);

    _spazio = lv_label_create(_root);
    lv_obj_set_style_text_color(_spazio, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_text(_spazio, "");
}

/* Il clic su una riga. Nello user_data c'e' il percorso da aprire, oppure NULL
   per le righe che non si aprono (i file). */
void EsploraApp::su_voce(lv_event_t *e)
{
    const char *dove = (const char *)lv_event_get_user_data(e);
    if (!dove) return;

    /* L'istanza si ritrova risalendo fino all'oggetto che la tiene nel proprio
       user_data, cioe' il root (vedi run()). Dentro una callback statica non
       c'e' altro modo, e un singleton nascosto qui impedirebbe di avere due
       finestre un domani. */
    lv_obj_t *o = lv_event_get_target(e);
    while (o && !lv_obj_get_user_data(o)) o = lv_obj_get_parent(o);
    EsploraApp *app = o ? (EsploraApp *)lv_obj_get_user_data(o) : nullptr;
    if (app) app->vai(dove);
}

/* La X di una riga. Il percorso sta nello user_data del tasto, come per le
   righe stesse. */
void EsploraApp::su_cestino(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    while (o && !lv_obj_get_user_data(o)) o = lv_obj_get_parent(o);
    EsploraApp *app = o ? (EsploraApp *)lv_obj_get_user_data(o) : nullptr;
    const char *dove = (const char *)lv_event_get_user_data(e);
    if (app && dove) app->chiedi_elimina(dove);
}

/* La risposta alla domanda. Il bottone 0 e' "Elimina", l'1 "Annulla". */
void EsploraApp::su_conferma(lv_event_t *e)
{
    lv_obj_t *mb = lv_event_get_current_target(e);
    char *dove = (char *)lv_event_get_user_data(e);
    uint16_t scelto = lv_msgbox_get_active_btn(mb);

    lv_obj_t *o = mb;
    while (o && !lv_obj_get_user_data(o)) o = lv_obj_get_parent(o);
    EsploraApp *app = o ? (EsploraApp *)lv_obj_get_user_data(o) : nullptr;

    if (scelto == 0 && dove && app) {
        if (esplora_elimina(dove)) app->aggiorna();
    }
    lv_msgbox_close(mb);
}

/* Chiede prima di cancellare, e nella domanda c'e' il nome: "sei sicuro?" da
   solo non fa decidere niente, perche' non dice su cosa si sta decidendo. */
void EsploraApp::chiedi_elimina(const char *percorso)
{
    static char scelto[256];
    snprintf(scelto, sizeof(scelto), "%s", percorso);

    const char *nome = strrchr(scelto, '/');
    static char domanda[320];
    snprintf(domanda, sizeof(domanda),
             "Cancello\n\n%s\n\nNon si torna indietro: qui non c'e' nessun cestino.",
             nome ? nome + 1 : scelto);

    static const char *tasti[] = {"Elimina", "Annulla", ""};
    lv_obj_t *mb = lv_msgbox_create(lv_layer_top(), "Eliminare?", domanda, tasti, false);
    lv_obj_center(mb);
    lv_obj_set_user_data(mb, this);
    lv_obj_add_event_cb(mb, su_conferma, LV_EVENT_VALUE_CHANGED, scelto);
    /* "Elimina" in rosso: il colore e' l'ultima cosa che si guarda prima di
       premere, e deve dire da sola quale dei due tasti e' quello che fa male. */
    lv_obj_t *b = lv_msgbox_get_btns(mb);
    if (b) lv_obj_set_style_bg_color(b, lv_color_hex(0xb3261e), LV_PART_ITEMS | LV_STATE_CHECKED);
}

void EsploraApp::vai(const char *percorso)
{
    if (!percorso || !esplora_permesso(percorso)) return;

    /* Cartella o file lo decide stat, non il chiamante: cosi' c'e' un punto
       solo in cui si sceglie, e le righe dell'elenco non devono sapere con
       che cosa hanno a che fare. */
    struct stat st = {};
    if (stat(percorso, &st) == 0 && !S_ISDIR(st.st_mode)) {
        if (e_figura(percorso))     apri_figura(percorso);
        else if (e_testo(percorso)) apri_testo(percorso);
        return;                      // un file non e' una cartella in cui entrare
    }
    snprintf(_percorso, sizeof(_percorso), "%s", percorso);
    aggiorna();
}

void EsploraApp::aggiorna(void)
{
    if (!_lista) return;
    lv_obj_clean(_lista);
    lv_label_set_text(_dove, _percorso);

    esplora_voce_t *v = (esplora_voce_t *)heap_caps_malloc(
        sizeof(esplora_voce_t) * RIGHE_MAX, MALLOC_CAP_SPIRAM);
    if (!v) return;

    bool troncato = false;
    int n = esplora_elenca(_percorso, v, RIGHE_MAX, &troncato);
    if (n < 0) {
        lv_obj_t *b = lv_list_add_text(_lista, "Non si apre (scheda assente?)");
        lv_obj_set_style_text_color(b, lv_color_hex(0xe0786f), 0);
        heap_caps_free(v);
        lv_label_set_text(_spazio, "");
        return;
    }

    /* La riga per risalire, se non siamo gia' su una radice. Il percorso del
       padre sta in un buffer che vive quanto la riga: lo si attacca alla riga
       stessa, cosi' sparisce con lei. */
    bool radice = false;
    for (int i = 0; esplora_radice(i); i++)
        if (!strcmp(_percorso, esplora_radice(i))) radice = true;
    if (!radice) {
        char *su = (char *)lv_mem_alloc(sizeof(_percorso));
        if (su) {
            snprintf(su, sizeof(_percorso), "%s", _percorso);
            char *taglio = strrchr(su, '/');
            if (taglio && taglio != su) *taglio = 0;
            lv_obj_t *b = lv_list_add_btn(_lista, LV_SYMBOL_LEFT, "..");
            lv_obj_add_event_cb(b, su_voce, LV_EVENT_CLICKED, su);
            lv_obj_add_event_cb(b, [](lv_event_t *e) {
                lv_mem_free(lv_event_get_user_data(e));
            }, LV_EVENT_DELETE, su);
        }
    }

    for (int i = 0; i < n; i++) {
        char riga[128], dim[24];
        if (v[i].cartella) {
            snprintf(riga, sizeof(riga), "%s", v[i].nome);
            lv_obj_t *b = lv_list_add_btn(_lista, LV_SYMBOL_DIRECTORY, riga);
            char *dove = (char *)lv_mem_alloc(sizeof(_percorso));
            if (dove) {
                snprintf(dove, sizeof(_percorso), "%s%s%s", _percorso,
                         _percorso[strlen(_percorso) - 1] == '/' ? "" : "/", v[i].nome);
                lv_obj_add_event_cb(b, su_voce, LV_EVENT_CLICKED, dove);
                lv_obj_add_event_cb(b, [](lv_event_t *e) {
                    lv_mem_free(lv_event_get_user_data(e));
                }, LV_EVENT_DELETE, dove);
            }
        } else {
            misura(v[i].byte, dim, sizeof(dim));
            snprintf(riga, sizeof(riga), "%s   (%s)", v[i].nome, dim);
            /* Un file di testo si legge, una figura si guarda; gli altri
               restano in elenco senza aprirsi - vedere che ci sono e quanto
               pesano e' gia' meta' del motivo per cui esiste questa app.
               Tutti pero' si possono cancellare, con la X in fondo alla riga. */
            bool figura = e_figura(v[i].nome);
            bool apribile = figura || e_testo(v[i].nome);
            const char *icona = figura ? LV_SYMBOL_IMAGE
                                       : (apribile ? LV_SYMBOL_EDIT : LV_SYMBOL_FILE);
            lv_obj_t *b = lv_list_add_btn(_lista, icona, riga);

            /* Il percorso completo serve sia alla riga sia alla X: se ne tiene
               una copia sola, liberata quando la riga sparisce. La X e' figlia
               della riga, quindi non le sopravvive mai. */
            char *dove = (char *)lv_mem_alloc(sizeof(_percorso));
            if (dove) {
                snprintf(dove, sizeof(_percorso), "%s%s%s", _percorso,
                         _percorso[strlen(_percorso) - 1] == '/' ? "" : "/", v[i].nome);
                lv_obj_add_event_cb(b, [](lv_event_t *e) {
                    lv_mem_free(lv_event_get_user_data(e));
                }, LV_EVENT_DELETE, dove);
            }
            if (apribile && dove) lv_obj_add_event_cb(b, su_voce, LV_EVENT_CLICKED, dove);
            else                  lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);

            if (dove) {
                /* La X in fondo. Non eredita i clic della riga - in LVGL un
                   figlio cliccabile se li tiene - quindi premerla non apre
                   anche il file. */
                lv_obj_t *x = lv_btn_create(b);
                lv_obj_set_size(x, 44, 36);
                lv_obj_set_style_bg_color(x, lv_color_hex(0x5a2a2a), 0);
                lv_obj_set_style_bg_color(x, lv_color_hex(0xb3261e), LV_STATE_PRESSED);
                lv_obj_t *lx = lv_label_create(x);
                lv_label_set_text(lx, LV_SYMBOL_CLOSE);
                lv_obj_center(lx);
                lv_obj_add_event_cb(x, su_cestino, LV_EVENT_CLICKED, dove);
            }
        }
    }
    if (!n) lv_list_add_text(_lista, "Qui non c'e' niente.");
    if (troncato) lv_list_add_text(_lista, "Elenco troppo lungo: mostrate solo le prime voci.");

    heap_caps_free(v);

    uint64_t tot = 0, lib = 0;
    if (esplora_spazio(_percorso, &tot, &lib)) {
        char a[24], b[24];
        misura(lib, a, sizeof(a));
        misura(tot, b, sizeof(b));
        lv_label_set_text_fmt(_spazio, "%s liberi su %s", a, b);
    } else {
        lv_label_set_text(_spazio, "");
    }
}

// ------------------------------------------------------------------ ciclo di vita

bool EsploraApp::run(void)
{
    costruisci();
    lv_obj_set_user_data(_root, this);     // come si ritrova l'istanza dalle callback
    snprintf(_percorso, sizeof(_percorso), "%s", esplora_radice(0));
    aggiorna();
    return true;
}

bool EsploraApp::back(void)
{
    // Prima si chiude quello che si sta leggendo, poi si risale.
    if (_vis) { chiudi_testo(); return true; }

    /* Il tasto indietro risale di una cartella finche' ce n'e', e solo dalla
       radice chiude l'app: e' quello che ci si aspetta da un esploratore, e
       chiudere tutto al primo tocco sarebbe seccante. */
    for (int i = 0; esplora_radice(i); i++) {
        if (!strcmp(_percorso, esplora_radice(i))) return close();
    }
    char su[sizeof(_percorso)];
    snprintf(su, sizeof(su), "%s", _percorso);
    char *taglio = strrchr(su, '/');
    if (taglio && taglio != su) {
        *taglio = 0;
        vai(su);
        return true;
    }
    return close();
}

bool EsploraApp::close(void)
{
    chiudi_testo();
    if (_root) { lv_obj_del(_root); _root = nullptr; }
    _dove = _lista = _spazio = nullptr;
    return true;
}

// ------------------------------------------------------ visualizzatore di testo

LV_FONT_DECLARE(lv_font_unscii_16);

/* Quanto si legge per pagina. Ottomila byte sono una ventina di schermate da
   scorrere e restano comodi in memoria; il file vero puo' essere di megabyte. */
#define PAGINA 8000

/* Le estensioni che si aprono come testo. L'elenco e' esplicito e non un
   tentativo di indovinare: aprire per sbaglio un jpeg da due megabyte come se
   fosse testo riempirebbe lo schermo di spazzatura e la memoria con lei. */
/* Le figure che si sanno mostrare: il JPEG lo decodifica l'acceleratore del
   P4, il PNG lo apre LVGL dal file. Le due strade le sceglie gia'
   web_image_mostra(), che e' lo stesso codice che disegna le immagini delle
   card di Home Assistant: una figura sulla SD e una scaricata da HA sono la
   stessa cosa una volta che sono un file. */
bool EsploraApp::e_figura(const char *nome) const
{
    static const char *const EXT[] = {".jpg", ".jpeg", ".png", nullptr};
    const char *p = strrchr(nome, '.');
    if (!p) return false;
    for (int i = 0; EXT[i]; i++) if (!strcasecmp(p, EXT[i])) return true;
    return false;
}

bool EsploraApp::e_testo(const char *nome) const
{
    static const char *const EXT[] = {
        ".csv", ".txt", ".ini", ".cfg", ".conf", ".json", ".log",
        ".md", ".yaml", ".yml", ".xml", ".html", ".css", ".js", ".py", nullptr
    };
    const char *p = strrchr(nome, '.');
    if (!p) return false;
    for (int i = 0; EXT[i]; i++) if (!strcasecmp(p, EXT[i])) return true;
    return false;
}

void EsploraApp::mostra_pagina(long da)
{
    if (!_vis_testo) return;          // si sta guardando una figura
    if (da < 0) da = 0;
    if (da >= _vis_dim && _vis_dim > 0) da = _vis_dim - 1;

    FILE *f = fopen(_vis_file, "rb");
    if (!f) {
        lv_label_set_text(_vis_testo, "Il file non si apre piu'.");
        return;
    }

    /* Si parte sempre da inizio riga: una pagina che comincia a meta' di una
       riga di CSV sembra un file rovinato. Si indietreggia fino al ritorno a
       capo precedente, al massimo di una riga lunga. */
    if (da > 0) {
        long cerca = da > 400 ? da - 400 : 0;
        fseek(f, cerca, SEEK_SET);
        char tampone[400];
        size_t n = fread(tampone, 1, (size_t)(da - cerca), f);
        long indietro = 0;
        for (long i = (long)n - 1; i >= 0; i--) {
            if (tampone[i] == '\n') { indietro = (long)n - 1 - i; break; }
        }
        da -= indietro;
    }

    char *buf = (char *)heap_caps_malloc(PAGINA + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return; }
    fseek(f, da, SEEK_SET);
    size_t letti = fread(buf, 1, PAGINA, f);
    fclose(f);
    buf[letti] = 0;

    /* Si taglia all'ultima riga intera, cosi' la pagina dopo riparte da li'
       senza spezzare niente a meta'. */
    if (letti == PAGINA) {
        for (size_t i = letti; i > 0; i--) {
            if (buf[i - 1] == '\n') { buf[i] = 0; letti = i; break; }
        }
    }

    /* I byte che non sono testo diventano punti: un file binario aperto per
       sbaglio si vede che e' binario, invece di mandare in confusione il
       disegnatore con byte che non sono caratteri. */
    for (size_t i = 0; i < letti; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\t') buf[i] = ' ';
        else if (c < 0x20 && c != '\n' && c != '\r') buf[i] = '.';
    }

    _vis_da = da;
    lv_label_set_text(_vis_testo, buf);
    heap_caps_free(buf);

    const char *nome = strrchr(_vis_file, '/');
    lv_label_set_text_fmt(_vis_titolo, "%s   byte %ld-%ld di %ld",
                          nome ? nome + 1 : _vis_file,
                          _vis_da, _vis_da + (long)letti, _vis_dim);
    lv_obj_scroll_to(lv_obj_get_parent(_vis_testo), 0, 0, LV_ANIM_OFF);
}

/* I tasti del visualizzatore. Quale sia lo dice il numero nello user_data:
   0 chiudi, 1 pagina indietro, 2 pagina avanti, 3 cambia font. */
void EsploraApp::su_vis(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    while (o && !lv_obj_get_user_data(o)) o = lv_obj_get_parent(o);
    EsploraApp *app = o ? (EsploraApp *)lv_obj_get_user_data(o) : nullptr;
    if (!app) return;
    int quale = (int)(intptr_t)lv_event_get_user_data(e);

    switch (quale) {
    case 0: app->chiudi_testo(); break;
    case 1: app->mostra_pagina(app->_vis_da - PAGINA); break;
    case 2: app->mostra_pagina(app->_vis_da + PAGINA); break;
    case 3:
        if (!app->_vis_testo) break;          // su una figura non c'e' niente da cambiare
        app->_vis_fisso = !app->_vis_fisso;
        lv_obj_set_style_text_font(app->_vis_testo,
                                   app->_vis_fisso ? &lv_font_unscii_16 : LV_FONT_DEFAULT, 0);
        break;
    default: break;
    }
}

/* La cornice che testo e figure si dividono: fondo, barra dei tasti e riga
   del titolo. Ritorna il contenitore dove mettere il contenuto.
   "con_pagine" aggiunge i tasti che servono solo al testo. */
lv_obj_t *EsploraApp::telaio_vista(const char *titolo)
{
    _vis = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(_vis);
    lv_obj_set_size(_vis, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_vis, lv_color_hex(0x0e1016), 0);
    lv_obj_set_style_bg_opa(_vis, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(_vis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_vis, 10, 0);
    lv_obj_set_style_pad_row(_vis, 8, 0);
    lv_obj_clear_flag(_vis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(_vis, this);

    lv_obj_t *barra = lv_obj_create(_vis);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(barra, 8, 0);
    lv_obj_clear_flag(barra, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *testo; int quale; } tasti[] = {
        {LV_SYMBOL_CLOSE " Chiudi", 0},
        {LV_SYMBOL_UP   " Pagina", 1},
        {LV_SYMBOL_DOWN " Pagina", 2},
        {"Aa", 3},
    };
    /* Per una figura serve solo "Chiudi": pagine e font non hanno senso. */
    int quanti = titolo ? 4 : 1;
    for (int i = 0; i < quanti; i++) {
        lv_obj_t *b = lv_btn_create(barra);
        lv_obj_set_height(b, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3142), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, tasti[i].testo);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, su_vis, LV_EVENT_CLICKED, (void *)(intptr_t)tasti[i].quale);
    }

    _vis_titolo = lv_label_create(_vis);
    lv_obj_set_style_text_color(_vis_titolo, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(_vis_titolo, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_vis_titolo, LV_PCT(100));

    lv_obj_t *cont = lv_obj_create(_vis);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1b2030), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    return cont;
}

void EsploraApp::apri_testo(const char *percorso)
{
    if (!esplora_permesso(percorso)) return;
    snprintf(_vis_file, sizeof(_vis_file), "%s", percorso);

    struct stat st = {};
    _vis_dim = (stat(percorso, &st) == 0) ? (long)st.st_size : 0;

    lv_obj_t *cont = telaio_vista("testo");

    /* Righe NON mandate a capo: una riga di CSV spezzata su tre perde
       l'incolonnamento. Si scorre di lato, come in un foglio di calcolo. */
    _vis_testo = lv_label_create(cont);
    lv_label_set_long_mode(_vis_testo, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(_vis_testo, lv_color_hex(0xdfe3e8), 0);
    lv_label_set_text(_vis_testo, "");

    /* Si parte dal font normale, che e' quello dell'elenco e quello che si
       legge. Il tasto "Aa" passa a quello a larghezza fissa per quando contano
       le colonne. */
    _vis_fisso = false;
    lv_obj_set_style_text_font(_vis_testo, LV_FONT_DEFAULT, 0);
    mostra_pagina(0);
}

void EsploraApp::apri_figura(const char *percorso)
{
    if (!esplora_permesso(percorso)) return;
    snprintf(_vis_file, sizeof(_vis_file), "%s", percorso);

    struct stat st = {};
    long dim = (stat(percorso, &st) == 0) ? (long)st.st_size : 0;

    lv_obj_t *cont = telaio_vista(nullptr);
    lv_obj_center(cont);

    /* web_image_t vuole percorsi corti: e' nata per la cache, dove i nomi li
       sceglie il pannello. Un file con un nome lunghissimo non si mostra, ma
       lo si dice invece di disegnare un riquadro vuoto. */
    web_image_t im = {};
    if (strlen(percorso) >= WEB_IMAGE_PATH_MAX) {
        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_text(l, "Il percorso di questo file e' troppo lungo per mostrarlo.");
        lv_obj_center(l);
        lv_label_set_text_fmt(_vis_titolo, "%s", percorso);
        return;
    }
    snprintf(im.percorso, sizeof(im.percorso), "%s", percorso);
    snprintf(im.lvgl, sizeof(im.lvgl), "S:%s", percorso);
    const char *est = strrchr(percorso, '.');
    im.tipo = (est && !strcasecmp(est, ".png")) ? IMG_PNG : IMG_JPEG;

    /* La misura e' quella del contenitore, cosi' una foto grande entra nello
       schermo invece di uscirne da tutti i lati. Il ridimensionamento a
       proporzioni rispettate lo fa gia' web_image_mostra. */
    lv_obj_update_layout(cont);
    _vis_img = web_image_mostra(cont, &im,
                                lv_obj_get_content_width(cont),
                                lv_obj_get_content_height(cont));
    const char *nome = strrchr(percorso, '/');
    if (_vis_img) {
        lv_obj_center(_vis_img);
        lv_label_set_text_fmt(_vis_titolo, "%s   %ld kB   %dx%d",
                              nome ? nome + 1 : percorso, dim / 1024,
                              (int)lv_obj_get_width(_vis_img),
                              (int)lv_obj_get_height(_vis_img));
    } else {
        lv_obj_t *l = lv_label_create(cont);
        /* Il motivo piu' probabile e' la memoria: una foto da dodici megapixel
           vuole ventiquattro megabyte per essere decodificata. */
        lv_label_set_text(l, "Non riesco a mostrarla: forse e' troppo grande.\n"
                             "Le foto si riducono sul computer con\n"
                             "strumenti/converti_foto.bat");
        lv_obj_center(l);
        lv_label_set_text_fmt(_vis_titolo, "%s   %ld kB",
                              nome ? nome + 1 : percorso, dim / 1024);
    }
}

void EsploraApp::chiudi_testo(void)
{
    /* Basta cancellare lo strato: l'immagine JPEG decodificata si libera da
       sola, perche' web_image_mostra le ha attaccato un gestore di
       LV_EVENT_DELETE che restituisce i pixel. */
    if (_vis) { lv_obj_del(_vis); _vis = nullptr; }
    _vis_testo = _vis_titolo = _vis_img = nullptr;
    _vis_file[0] = 0;
}

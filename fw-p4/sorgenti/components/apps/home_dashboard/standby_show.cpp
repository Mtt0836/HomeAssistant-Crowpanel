#include "standby_show.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_dma_utils.h"
#include "esp_random.h"
#include "cJSON.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"    // decoder JPEG hardware del P4
#include "driver/ppa.h"            // Pixel Processing Accelerator (scaling hardware)
#include "lovelace_ui.h"

static const char *TAG = "standby";
/* La configurazione vive in flash: la pagina web puo' cambiarla anche senza
   SD inserita, e non si perde togliendo la scheda. Il vecchio file sulla SD
   resta valido e viene ricopiato in flash la prima volta (chi l'aveva scritto
   a mano se lo ritrova, e da li' in poi comanda la pagina). */
#define CONFIG_PATH "/spiffs/standby.json"
#define CONFIG_OLD  "/sdcard/standby.json"
// Tetto per la decodifica NATIVA transitoria in PSRAM (evita OOM su foto assurde).
#define MAX_DECODE_BYTES (24u * 1024 * 1024)

static standby_cfg_t s_cfg;
static volatile bool s_running = false;
static volatile bool s_dark    = false;
static TaskHandle_t  s_task = NULL;
static lv_obj_t     *s_overlay = NULL;
static lv_obj_t     *s_img = NULL;
static lv_obj_t     *s_clock = NULL, *s_date = NULL, *s_values = NULL;
static lv_obj_t     *s_val_name[STANDBY_MAX_OVERLAY], *s_val_text[STANDBY_MAX_OVERLAY];
static lv_timer_t   *s_timer = NULL;
static lv_img_dsc_t  s_dsc;
static uint8_t      *s_disp = NULL;        // buffer DA MOSTRARE (persistente)
static jpeg_decoder_handle_t s_jpg = NULL;
static ppa_client_handle_t   s_ppa = NULL;
static const size_t  s_align = 64;         // allineamento cache per i buffer PPA

// ------------------------------------------------------------ configurazione

/* Una voce della sovrimpressione: "sensor.x" oppure {"id":"sensor.x",
   "nome":"Sole"}. La forma corta resta valida per chi ha scritto il file a
   mano prima che esistessero i nomi propri. */
void standby_overlay_from_json(const cJSON *arr, standby_cfg_t *cfg)
{
    if (!cJSON_IsArray(arr)) return;
    cfg->n_overlay = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        if (cfg->n_overlay >= STANDBY_MAX_OVERLAY) break;
        const char *id = NULL, *nome = NULL;
        if (cJSON_IsString(v)) {
            id = v->valuestring;
        } else if (cJSON_IsObject(v)) {
            const cJSON *a = cJSON_GetObjectItem(v, "id");
            const cJSON *b = cJSON_GetObjectItem(v, "nome");
            if (cJSON_IsString(a)) id = a->valuestring;
            if (cJSON_IsString(b)) nome = b->valuestring;
        }
        if (!id || !id[0]) continue;
        int k = cfg->n_overlay++;
        strlcpy(cfg->overlay[k], id, sizeof(cfg->overlay[k]));
        strlcpy(cfg->overlay_label[k], nome ? nome : "", sizeof(cfg->overlay_label[k]));
    }
}

void standby_config_load(standby_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->slideshow_after_s  = 120;
    cfg->screen_off_after_s = 600;
    cfg->photo_seconds      = 10;
    cfg->shuffle            = true;
    strlcpy(cfg->folder, "/sdcard/foto", sizeof(cfg->folder));
    /* Nessun valore predefinito: quali sensori mostrare dipende da che casa
       e', e un elenco inventato qui darebbe solo tre caselle vuote. Finche'
       non se ne sceglie uno, in sovrimpressione restano ora e data. */

    /* Tutto si cambia senza ricompilare, dalla pagina web o scrivendo il file:
       {"slideshow_dopo_min":2, "spegni_dopo_min":10, "secondi_foto":10,
        "casuale":true, "cartella":"/sdcard/foto",
        "sovrimpressione":["sensor.a", {"id":"sensor.b", "nome":"Sole"}]}
       Una voce puo' essere il solo identificativo, e allora il nome lo
       decide Home Assistant, oppure identificativo piu' nome scelto qui. */
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) f = fopen(CONFIG_OLD, "r");          // configurazione scritta a mano sulla SD
    if (!f) {
        ESP_LOGI(TAG, "%s assente: valori predefiniti", CONFIG_PATH);
        return;
    }
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        ESP_LOGW(TAG, "%s non e' JSON valido: valori predefiniti", CONFIG_PATH);
        return;
    }
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "slideshow_dopo_min"))) cfg->slideshow_after_s  = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "spegni_dopo_min")))    cfg->screen_off_after_s = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "secondi_foto")))       cfg->photo_seconds      = v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "casuale")))              cfg->shuffle            = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "cartella")))           strlcpy(cfg->folder, v->valuestring, sizeof(cfg->folder));
    standby_overlay_from_json(cJSON_GetObjectItem(j, "sovrimpressione"), cfg);
    cJSON_Delete(j);
    if (cfg->slideshow_after_s < 10)  cfg->slideshow_after_s = 10;
    if (cfg->screen_off_after_s < 0)  cfg->screen_off_after_s = 0;
    if (cfg->photo_seconds < 3)       cfg->photo_seconds = 3;
    ESP_LOGI(TAG, "configurazione da %s: slideshow dopo %d s, spento dopo altri %d s",
             CONFIG_PATH, cfg->slideshow_after_s, cfg->screen_off_after_s);
}

bool standby_config_save(const standby_cfg_t *cfg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "slideshow_dopo_min", cfg->slideshow_after_s / 60.0);
    cJSON_AddNumberToObject(j, "spegni_dopo_min",    cfg->screen_off_after_s / 60.0);
    cJSON_AddNumberToObject(j, "secondi_foto",       cfg->photo_seconds);
    cJSON_AddBoolToObject(j,   "casuale",            cfg->shuffle);
    cJSON_AddStringToObject(j, "cartella",           cfg->folder);
    cJSON *ov = cJSON_AddArrayToObject(j, "sovrimpressione");
    for (int i = 0; i < cfg->n_overlay; i++) {
        /* Senza nome proprio scrivo la forma corta: il file resta leggibile
           per chi ci mette le mani, e dice da solo che il nome lo fa HA. */
        if (!cfg->overlay_label[i][0]) {
            cJSON_AddItemToArray(ov, cJSON_CreateString(cfg->overlay[i]));
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id",   cfg->overlay[i]);
        cJSON_AddStringToObject(e, "nome", cfg->overlay_label[i]);
        cJSON_AddItemToArray(ov, e);
    }
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return false;

    FILE *f = fopen(CONFIG_PATH, "w");
    bool ok = f != NULL;
    if (ok) {
        ok = fwrite(txt, 1, strlen(txt), f) == strlen(txt);
        fclose(f);
    }
    ESP_LOGI(TAG, "configurazione %s in %s", ok ? "salvata" : "NON salvata", CONFIG_PATH);
    free(txt);
    return ok;
}

// ------------------------------------------------------------ foto

// Alloca un buffer PSRAM allineato alla cache (richiesto dal PPA in output).
static uint8_t *psram_aligned(size_t bytes, size_t *alloc_out)
{
    size_t sz = (bytes + s_align - 1) / s_align * s_align;
    if (alloc_out) *alloc_out = sz;
    return (uint8_t *)heap_caps_aligned_alloc(s_align, sz, MALLOC_CAP_SPIRAM);
}

/* Il ponte per leggere dalla SD.

   Il buffer del JPEG sta in PSRAM, e la PSRAM il DMA della SD non la sa
   scrivere: il driver se ne accorge e si alloca da solo un buffer di appoggio
   in RAM interna, grande quanto la lettura. Leggendo un JPEG da 200 kB in un
   colpo solo chiede 200 kB interni contigui, e quelli non ci sono - il blocco
   piu' grande libero qui e' sui 36 kB. Da li' partiva la catena che ha ucciso
   il pannello: esp_dma_capable_malloc fallisce, sdmmc_read_blocks fallisce,
   il driver rientra a ripetere, e lo stack di idle_mgr finisce.

   Con un buffer interno nostro, preso una volta sola e riusato, la lettura
   arriva gia' in memoria che il DMA sa scrivere: il driver non alloca niente
   e non c'e' niente che possa mancare. Il file si legge a fette e ogni fetta
   si ricopia in PSRAM.

   Nota per chi cerca un "deframmentatore": non esiste e non puo' esistere, in
   C non si spostano blocchi gia' consegnati a qualcuno. L'unica cosa che si
   puo' fare e' non frammentare, ed e' questa. */
#define FETTA 8192
static uint8_t *s_ponte = NULL;

/* Il buffer si prende una volta all'avvio dello slideshow, non alla prima
   foto: e' il momento in cui la memoria e' messa meglio, e se non riesce lo
   si sa subito invece di riprovare a ogni immagine.

   esp_dma_capable_malloc e non heap_caps_aligned_alloc: e' la stessa funzione
   che usa il driver della SD, quindi chiede esattamente la memoria che a lui
   serve, allineamento della cache compreso. Con heap_caps_aligned_alloc e i
   flag messi a mano falliva sempre, e il buffer non c'era mai. */
static void ponte_prendi(void)
{
    if (s_ponte) return;
    esp_dma_mem_info_t mi = {};
    mi.extra_heap_caps     = MALLOC_CAP_INTERNAL;
    mi.dma_alignment_bytes = 64;
    void *p = NULL;
    if (esp_dma_capable_malloc(FETTA, &mi, &p, NULL) == ESP_OK && p) {
        s_ponte = (uint8_t *)p;
        return;
    }
    ESP_LOGW(TAG, "niente buffer di appoggio (liberi %u, blocco max %u): "
                  "leggo diretto in PSRAM",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

static bool leggi_tutto(FILE *f, uint8_t *dst, size_t n)
{
    if (!s_ponte) return fread(dst, 1, n, f) == n;
    size_t fatti = 0;
    while (fatti < n) {
        size_t quanti = n - fatti < FETTA ? n - fatti : FETTA;
        size_t letti = fread(s_ponte, 1, quanti, f);
        if (!letti) return false;
        memcpy(dst + fatti, s_ponte, letti);
        fatti += letti;
    }
    return true;
}

/*
 * Decodifica il JPEG a nativo; se piu' grande dello schermo lo riduce col PPA in
 * un buffer della dimensione a video, liberando subito il buffer nativo grande.
 * Riempie s_disp (persistente) e s_dsc. Ritorna true se pronto da mostrare.
 */
static bool decode_and_fit(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long insize = ftell(f); fseek(f, 0, SEEK_SET);
    if (insize <= 0) { fclose(f); return false; }

    jpeg_decode_memory_alloc_cfg_t in_mem = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t in_alloc = 0;
    uint8_t *in = (uint8_t *)jpeg_alloc_decoder_mem(insize, &in_mem, &in_alloc);
    if (!in) { fclose(f); return false; }
    bool letto = leggi_tutto(f, in, (size_t)insize);
    fclose(f);
    if (!letto) { ESP_LOGW(TAG, "%s: lettura incompleta", path); free(in); return false; }

    jpeg_decode_picture_info_t hdr;
    if (jpeg_decoder_get_info(in, insize, &hdr) != ESP_OK) { free(in); return false; }

    /* Il decoder hardware scrive a blocchi di 16 pixel: il buffer di uscita
       deve avere larghezza e altezza arrotondate a multipli di 16, altrimenti
       rifiuta la decodifica ("Given buffer size ... is smaller than actual"). */
    const uint32_t w_align = (hdr.width  + 15u) & ~15u;
    const uint32_t h_align = (hdr.height + 15u) & ~15u;
    size_t native_need = (size_t)w_align * h_align * 2;        // RGB565

    /* Alcuni JPEG (foto di telefono con anteprima EXIF) fanno leggere al
       decoder le dimensioni della miniatura: il file e' enorme rispetto a
       quelle dimensioni e la decodifica fallirebbe a meta'. */
    if (hdr.width <= 640 && insize > 1024 * 1024) {
        ESP_LOGW(TAG, "%s: intestazione incoerente (%ux%u per %ld byte): salto",
                 path, (unsigned)hdr.width, (unsigned)hdr.height, insize);
        free(in); return false;
    }
    /* Il decoder hardware non sa ridurre mentre decodifica: serve tutta la
       foto in PSRAM. Una da 12 Mpixel vuole 24 MB e non ci sta. */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (native_need > MAX_DECODE_BYTES || native_need + 2 * 1024 * 1024 > psram_free) {
        ESP_LOGW(TAG, "%s: %ux%u (%u MB da decodificare, liberi %u MB): troppo grande, "
                 "ridimensionala col programma strumenti/converti_foto.bat",
                 path, (unsigned)hdr.width, (unsigned)hdr.height,
                 (unsigned)(native_need / (1024 * 1024)), (unsigned)(psram_free / (1024 * 1024)));
        free(in); return false;
    }

    jpeg_decode_memory_alloc_cfg_t out_mem = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t native_alloc = 0;
    uint8_t *native = (uint8_t *)jpeg_alloc_decoder_mem(native_need, &out_mem, &native_alloc);
    if (!native) { free(in); return false; }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   // se colori invertiti: prova RGB
    };
    uint32_t out_len = 0;
    esp_err_t e = jpeg_decoder_process(s_jpg, &dcfg, in, insize, native, native_alloc, &out_len);
    free(in);
    if (e != ESP_OK) { free(native); return false; }

    /* Il buffer decodificato e' largo W_ALIGN: le colonne e le righe in piu'
       sono riempimento e vanno ritagliate. Lo fa il PPA, che nello stesso
       passaggio adatta la foto allo schermo. */
    float sx = (float)LV_HOR_RES / hdr.width;
    float sy = (float)LV_VER_RES / hdr.height;
    float scale = sx < sy ? sx : sy;
    if (scale > 1.0f) scale = 1.0f;              // le foto piccole non si ingrandiscono

    uint32_t dw = ((uint32_t)(hdr.width  * scale)) & ~1u;
    uint32_t dh = ((uint32_t)(hdr.height * scale)) & ~1u;
    if (dw < 2 || dh < 2) { free(native); return false; }
    size_t disp_need = (size_t)dw * dh * 2, disp_alloc = 0;
    uint8_t *new_disp = psram_aligned(disp_need, &disp_alloc);
    if (!new_disp) { free(native); return false; }

    ppa_srm_oper_config_t op = {};
    op.in.buffer       = native;
    op.in.pic_w        = w_align;                // larghezza reale del buffer
    op.in.pic_h        = h_align;
    op.in.block_w      = hdr.width;              // ritaglio della sola foto
    op.in.block_h      = hdr.height;
    op.in.srm_cm       = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = new_disp;
    op.out.buffer_size = disp_alloc;
    op.out.pic_w       = dw;
    op.out.pic_h       = dh;
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x         = (float)dw / hdr.width;
    op.scale_y         = (float)dh / hdr.height;
    op.mode            = PPA_TRANS_MODE_BLOCKING;   // in blocking il driver gestisce la cache

    esp_err_t pe = ppa_do_scale_rotate_mirror(s_ppa, &op);
    free(native);                                   // libero SUBITO il buffer grande
    if (pe != ESP_OK) { heap_caps_free(new_disp); ESP_LOGE(TAG, "PPA ko: %d", pe); return false; }

    uint8_t *old = s_disp;
    bsp_display_lock(0);
    if (!s_img || s_dark) {               // standby chiuso o spento nel frattempo
        bsp_display_unlock();
        heap_caps_free(new_disp);
        return false;
    }
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.w  = dw;
    s_dsc.header.h  = dh;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;   // RGB565 (LV_COLOR_DEPTH=16)
    s_dsc.data      = new_disp;
    s_dsc.data_size = (size_t)dw * dh * 2;
    /* Tutte le foto passano dallo stesso descrittore: senza invalidare la
       cache immagini di LVGL verrebbe ridisegnata la voce precedente, con le
       dimensioni vecchie e un puntatore a memoria gia' liberata. */
    lv_img_cache_invalidate_src(&s_dsc);
    lv_img_set_src(s_img, &s_dsc);
    lv_obj_center(s_img);
    s_disp = new_disp;
    bsp_display_unlock();
    ESP_LOGI(TAG, "%s: %ux%u (buffer %ux%u) mostrata a %ux%u", path,
             (unsigned)hdr.width, (unsigned)hdr.height, (unsigned)w_align, (unsigned)h_align,
             (unsigned)dw, (unsigned)dh);
    if (old) heap_caps_free(old);             // non piu' referenziato dall'immagine
    return true;
}

static std::vector<std::string> list_photos(void)
{
    std::vector<std::string> v;
    /* Se la cartella configurata non c'e', si guarda nella radice della SD:
       e' li' che finiscono le foto copiate al volo dal PC. */
    const char *dir = s_cfg.folder;
    DIR *d = opendir(dir);
    if (!d && strcmp(dir, "/sdcard") != 0) {
        dir = "/sdcard";
        d = opendir(dir);
        if (d) ESP_LOGW(TAG, "%s assente: uso %s", s_cfg.folder, dir);
    }
    if (!d) return v;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) continue;
        v.push_back(std::string(dir) + "/" + e->d_name);
    }
    closedir(d);
    if (s_cfg.shuffle) {
        for (size_t i = v.size(); i > 1; i--) std::swap(v[i - 1], v[esp_random() % i]);
    } else {
        std::sort(v.begin(), v.end());
    }
    return v;
}

static void sleep_while_running(int ms)
{
    for (int t = 0; t < ms && s_running; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
}

static void show_task(void *arg)
{
    bool warned = false;
    while (s_running) {
        if (s_dark) { sleep_while_running(500); continue; }
        std::vector<std::string> photos = list_photos();
        if (photos.empty()) {
            if (!warned) ESP_LOGW(TAG, "nessuna foto in %s ne' in /sdcard: solo sovrimpressione", s_cfg.folder);
            warned = true;
            sleep_while_running(10000);
            continue;
        }
        for (const std::string &p : photos) {
            if (!s_running || s_dark) break;
            if (decode_and_fit(p.c_str())) sleep_while_running(s_cfg.photo_seconds * 1000);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------ sovrimpressione

static const char *const GIORNI[] = {"domenica", "lunedi'", "martedi'", "mercoledi'", "giovedi'", "venerdi'", "sabato"};
static const char *const MESI[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
                                   "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};

// Gira nel task LVGL (lock gia' preso).
static void overlay_tick(lv_timer_t *t)
{
    if (!s_clock) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text_fmt(s_date, "%s %d %s", GIORNI[tm.tm_wday], tm.tm_mday, MESI[tm.tm_mon]);
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, "");
    }
    for (int i = 0; i < s_cfg.n_overlay; i++) {
        char name[48], value[40];
        if (ll_entity_text(s_cfg.overlay[i], name, sizeof(name), value, sizeof(value))) {
            /* Il nome scelto nella pagina web ha la precedenza su quello di
               Home Assistant, che spesso e' lungo e tecnico. */
            lv_label_set_text(s_val_name[i],
                              s_cfg.overlay_label[i][0] ? s_cfg.overlay_label[i] : name);
            lv_label_set_text(s_val_text[i], value);
        }
    }
}

static lv_obj_t *mk_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    return p;
}

static lv_obj_t *mk_text(lv_obj_t *parent, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static void build_overlay(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);    // assorbe il tocco di risveglio

    s_img = lv_img_create(s_overlay);
    lv_obj_center(s_img);

    lv_obj_t *left = mk_panel(s_overlay);
    lv_obj_align(left, LV_ALIGN_BOTTOM_LEFT, 24, -24);
    s_clock = mk_text(left, &lv_font_montserrat_48, lv_color_white());
    s_date  = mk_text(left, &lv_font_montserrat_22, lv_color_hex(0xdddddd));

    if (s_cfg.n_overlay > 0) {
        s_values = mk_panel(s_overlay);
        lv_obj_align(s_values, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
        lv_obj_set_style_pad_row(s_values, 2, 0);
        for (int i = 0; i < s_cfg.n_overlay; i++) {
            s_val_name[i] = mk_text(s_values, &lv_font_montserrat_14, lv_color_hex(0xbbbbbb));
            s_val_text[i] = mk_text(s_values, &lv_font_montserrat_26, lv_color_white());
            // il nome scelto si vede subito, anche prima che HA risponda
            lv_label_set_text(s_val_name[i], s_cfg.overlay_label[i]);
            lv_label_set_text(s_val_text[i], "--");
        }
    }
    overlay_tick(NULL);
    s_timer = lv_timer_create(overlay_tick, 1000, NULL);
}

// ------------------------------------------------------------ API

void standby_show_start(const standby_cfg_t *cfg)
{
    if (s_running) return;
    s_cfg = *cfg;
    s_dark = false;

    if (!s_jpg) {
        jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 1000 };
        if (jpeg_new_decoder_engine(&ecfg, &s_jpg) != ESP_OK) { ESP_LOGE(TAG, "jpeg engine ko"); s_jpg = NULL; }
    }
    if (!s_ppa) {
        ppa_client_config_t pcfg = {};
        pcfg.oper_type = PPA_OPERATION_SRM;
        if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) { ESP_LOGE(TAG, "PPA client ko"); s_ppa = NULL; }
    }

    bsp_display_lock(0);
    build_overlay();
    bsp_display_unlock();

    ponte_prendi();

    s_running = true;
    if (s_jpg && s_ppa) xTaskCreatePinnedToCore(show_task, "standby", 6144, NULL, 3, &s_task, 0);
    ESP_LOGI(TAG, "standby avviato");
}

void standby_show_set_dark(bool dark)
{
    if (!s_running) return;
    s_dark = dark;
    bsp_display_lock(0);
    if (s_overlay) {
        /* Da spento resta solo il nero dell'overlay: continua ad assorbire il
           tocco di risveglio. */
        if (dark) lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        if (s_timer) { if (dark) lv_timer_pause(s_timer); else lv_timer_resume(s_timer); }
    }
    bsp_display_unlock();
}

void standby_show_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    bsp_display_lock(0);
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_img = s_clock = s_date = s_values = NULL;
    bsp_display_unlock();
    if (s_disp) { heap_caps_free(s_disp); s_disp = NULL; }
    ESP_LOGI(TAG, "standby fermato");
}

bool standby_show_active(void) { return s_running; }

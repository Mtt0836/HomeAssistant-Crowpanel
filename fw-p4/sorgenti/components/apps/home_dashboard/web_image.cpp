#include "web_image.h"
#include "ha_http.h"
#include "ha_config.h"
#include "ha_token.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "web_img";

#define CARTELLA   "/sdcard/cache_img"
#define MAX_FILE   40            // quante immagini si tengono da parte
#define MAX_BYTE   (4 * 1024 * 1024)
#define GIU_MAX    (2 * 1024 * 1024)   // oltre questa non la si scarica nemmeno

struct Attesa { std::string src; bool subito; };

static std::vector<Attesa>  s_coda;
static SemaphoreHandle_t    s_mtx = NULL;
static TaskHandle_t         s_task = NULL;
static web_image_cb_t       s_cb = NULL;
static bool                 s_sd = false;     // la cartella si e' potuta creare?

// ------------------------------------------------------------------ nomi

/* FNV-1a: un nome corto e stabile per ogni indirizzo. Non serve che sia
   difficile da invertire, serve che due indirizzi diversi non si pestino. */
static uint32_t impronta(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static web_image_tipo_t tipo_da_nome(const char *s)
{
    const char *q = strrchr(s, '?');
    size_t n = q ? (size_t)(q - s) : strlen(s);
    auto finisce = [&](const char *ext) {
        size_t e = strlen(ext);
        return n >= e && strncasecmp(s + n - e, ext, e) == 0;
    };
    if (finisce(".png")) return IMG_PNG;
    if (finisce(".jpg") || finisce(".jpeg")) return IMG_JPEG;
    return IMG_IGNOTA;
}

/* I primi byte dicono la verita' meglio del nome: HA serve le telecamere da
   indirizzi che non finiscono per .jpg. */
static web_image_tipo_t tipo_da_contenuto(const unsigned char *b, size_t n)
{
    if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return IMG_PNG;
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return IMG_JPEG;
    return IMG_IGNOTA;
}

static void percorso_di(const char *src, web_image_tipo_t t, char *out, size_t sz)
{
    snprintf(out, sz, CARTELLA "/%08lx.%s", (unsigned long)impronta(src),
             t == IMG_PNG ? "png" : "jpg");
}

// ------------------------------------------------------------------ cache

static bool cartella_pronta(void)
{
    if (s_sd) return true;
    struct stat st;
    if (stat("/sdcard", &st) != 0) return false;        // nessuna scheda
    mkdir(CARTELLA, 0777);
    if (stat(CARTELLA, &st) != 0) return false;
    s_sd = true;
    return true;
}

/* Tiene la cartella entro misura, buttando le piu' vecchie. Una cache che
   cresce senza limite non e' una cache, e' una perdita lenta. */
static void pota(void)
{
    if (!s_sd) return;
    DIR *d = opendir(CARTELLA);
    if (!d) return;
    /* L'elenco sta nel heap, non sullo stack: ottanta voci con dentro una
       std::string fanno quasi quattro kilobyte, e questa funzione viene
       chiamata da scarica(), che di stack ne ha gia' impegnato parecchio. Con
       l'array qui dentro il task moriva di "stack protection fault" alla prima
       immagine scaricata. */
    struct Voce { std::string nome; time_t quando; off_t quanto; };
    std::vector<Voce> v;
    v.reserve(MAX_FILE * 2);
    size_t totale = 0;
    struct dirent *e;
    while ((e = readdir(d)) && v.size() < (size_t)(MAX_FILE * 2)) {
        char p[WEB_IMAGE_PATH_MAX];
        snprintf(p, sizeof(p), CARTELLA "/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        v.push_back({p, st.st_mtime, st.st_size});
        totale += st.st_size;
    }
    closedir(d);
    int n = (int)v.size();
    if (n <= MAX_FILE && totale <= MAX_BYTE) return;

    /* Ordine di anzianita', poi si butta dalla piu' vecchia finche' basta. */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j].quando < v[j-1].quando; j--)
            std::swap(v[j], v[j-1]);
    int i = 0;
    while (i < n && (n - i > MAX_FILE || totale > MAX_BYTE)) {
        unlink(v[i].nome.c_str());
        totale -= v[i].quanto;
        i++;
    }
    if (i) ESP_LOGI(TAG, "cache potata: %d immagini buttate", i);
}

// ------------------------------------------------------------------ scarico

/* L'indirizzo intero. Quelli che cominciano per "/" sono di Home Assistant e
   vogliono il permesso; gli altri vengono da fuori e non glielo si manda. */
static bool indirizzo(const char *src, char *out, size_t sz, bool *nostro)
{
    if (!src || !*src) return false;
    if (src[0] == '/') {
        char base[HA_URL_MAX];
        if (!ha_http_base(base, sizeof(base))) return false;
        snprintf(out, sz, "%s%s", base, src);
        *nostro = true;
        return true;
    }
    snprintf(out, sz, "%s", src);
    *nostro = false;
    return true;
}

/* Scarica a pezzi direttamente nel file: un'immagine intera in RAM sarebbe
   proprio quello che qui non si puo' permettere. */
static bool scarica(const char *src, web_image_t *out)
{
    char url[HA_URL_MAX * 2];
    bool nostro = false;
    if (!indirizzo(src, url, sizeof(url), &nostro)) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    char token[HA_TOKEN_MAX], bearer[HA_TOKEN_MAX + 16];
    if (nostro && ha_token_get_access(token, sizeof(token))) {
        snprintf(bearer, sizeof(bearer), "Bearer %s", token);
        esp_http_client_set_header(c, "Authorization", bearer);
        memset(token, 0, sizeof(token));
    }

    bool ok = false;
    FILE *f = NULL;
    char tmp[WEB_IMAGE_PATH_MAX];
    unsigned char testa[16];
    int presi_testa = 0;

    if (esp_http_client_open(c, 0) == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(c);
        int codice = esp_http_client_get_status_code(c);
        if (codice != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", src, codice);
        } else if (len > GIU_MAX) {
            ESP_LOGW(TAG, "%s: %lld byte, troppo grande", src, (long long)len);
        } else {
            /* Mezzo kilobyte per volta basta: a scrivere su SD si va
               comunque a blocchi, e lo stack qui e' prezioso. */
            char buf[512];
            int n;
            size_t scritti = 0;
            while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
                if (presi_testa < (int)sizeof(testa)) {
                    int q = (int)sizeof(testa) - presi_testa;
                    if (q > n) q = n;
                    memcpy(testa + presi_testa, buf, q);
                    presi_testa += q;
                }
                if (!f) {
                    /* Il formato si decide adesso, coi byte veri in mano, non
                       indovinandolo dal nome. */
                    out->tipo = tipo_da_contenuto(testa, presi_testa);
                    if (out->tipo == IMG_IGNOTA) out->tipo = tipo_da_nome(src);
                    if (out->tipo == IMG_IGNOTA) {
                        ESP_LOGW(TAG, "%s: formato non riconosciuto", src);
                        break;
                    }
                    if (!cartella_pronta()) break;      // senza SD non si tiene
                    percorso_di(src, out->tipo, out->percorso, sizeof(out->percorso));
                    snprintf(tmp, sizeof(tmp), "%s.tmp", out->percorso);
                    f = fopen(tmp, "wb");
                    if (!f) break;
                }
                if (fwrite(buf, 1, n, f) != (size_t)n) break;
                scritti += n;
                if (scritti > GIU_MAX) break;
            }
            if (f && n <= 0 && scritti > 0) ok = true;
        }
    }
    if (f) {
        fclose(f);
        if (ok) {
            /* Si rinomina solo a scaricamento finito: cosi' una caduta di rete
               non lascia in cache mezza immagine che poi si disegnerebbe
               rotta per sempre. */
            unlink(out->percorso);
            ok = rename(tmp, out->percorso) == 0;
        }
        if (!ok) unlink(tmp);
    }
    esp_http_client_cleanup(c);
    memset(bearer, 0, sizeof(bearer));
    if (ok) {
        snprintf(out->lvgl, sizeof(out->lvgl), "S:%s", out->percorso);
        ESP_LOGI(TAG, "presa %s -> %s", src, out->percorso);
        pota();
    }
    return ok;
}

bool web_image_scarica_file(const char *src, const char *destinazione)
{
    if (!src || !*src || !destinazione || !*destinazione) return false;

    char url[HA_URL_MAX * 2];
    bool nostro = false;
    if (!indirizzo(src, url, sizeof(url), &nostro)) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return false;

    char token[HA_TOKEN_MAX], bearer[HA_TOKEN_MAX + 16];
    if (nostro && ha_token_get_access(token, sizeof(token))) {
        snprintf(bearer, sizeof(bearer), "Bearer %s", token);
        esp_http_client_set_header(cl, "Authorization", bearer);
        memset(token, 0, sizeof(token));
    }

    char tmp[WEB_IMAGE_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", destinazione);
    bool ok = false;
    FILE *f = NULL;

    if (esp_http_client_open(cl, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        int codice = esp_http_client_get_status_code(cl);
        if (codice != 200) {
            ESP_LOGW(TAG, "foto %s: HTTP %d", src, codice);
        } else if ((f = fopen(tmp, "wb")) != NULL) {
            char buf[512];
            int n;
            size_t scritti = 0;
            while ((n = esp_http_client_read(cl, buf, sizeof(buf))) > 0) {
                if (fwrite(buf, 1, n, f) != (size_t)n) break;
                scritti += n;
                if (scritti > GIU_MAX) break;
            }
            if (n <= 0 && scritti > 0) ok = true;
        }
    }
    if (f) {
        fclose(f);
        if (ok) {
            unlink(destinazione);
            ok = rename(tmp, destinazione) == 0;
        }
        if (!ok) unlink(tmp);
    }
    esp_http_client_cleanup(cl);
    memset(bearer, 0, sizeof(bearer));
    if (ok) ESP_LOGI(TAG, "foto salvata in %s", destinazione);
    return ok;
}

// ------------------------------------------------------------------ coda

static void img_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        while (true) {
            Attesa a;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (s_coda.empty()) { xSemaphoreGive(s_mtx); break; }
            a = s_coda.front();
            s_coda.erase(s_coda.begin());
            xSemaphoreGive(s_mtx);

            web_image_t img = {};
            if (scarica(a.src.c_str(), &img) && s_cb) s_cb();
            /* Una pausa fra una e l'altra: le immagini sono il traffico piu'
               grosso che questo pannello faccia passare dal collegamento verso
               il C6, e tirarne giu' quattro di fila e' il modo migliore per
               far cadere l'SDIO. */
            vTaskDelay(pdMS_TO_TICKS(800));
        }
    }
}

static void accoda(const char *src, bool subito)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool gia = false;
    for (const Attesa &a : s_coda) if (a.src == src) { gia = true; break; }
    if (!gia) s_coda.push_back({src, subito});
    xSemaphoreGive(s_mtx);
    if (!s_task) xTaskCreate(img_task, "web_img", 8192, NULL, 3, &s_task);
    else         xTaskNotifyGive(s_task);
}

bool web_image_prendi(const char *src, web_image_t *out, bool subito)
{
    if (!src || !*src || !out) return false;
    memset(out, 0, sizeof(*out));

    if (!subito && cartella_pronta()) {
        /* Gia' da parte? Si prova nei due formati: quale sia lo si e' deciso
           quando e' stata scaricata. */
        for (web_image_tipo_t t : {IMG_JPEG, IMG_PNG}) {
            char p[WEB_IMAGE_PATH_MAX];
            percorso_di(src, t, p, sizeof(p));
            struct stat st;
            if (stat(p, &st) == 0 && st.st_size > 0) {
                snprintf(out->percorso, sizeof(out->percorso), "%s", p);
                snprintf(out->lvgl, sizeof(out->lvgl), "S:%s", p);
                out->tipo = t;
                return true;
            }
        }
    }
    accoda(src, subito);
    return false;
}

void web_image_on_arrivo(web_image_cb_t cb) { s_cb = cb; }

void web_image_svuota(void)
{
    if (!cartella_pronta()) return;
    DIR *d = opendir(CARTELLA);
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        char p[WEB_IMAGE_PATH_MAX];
        snprintf(p, sizeof(p), CARTELLA "/%s", e->d_name);
        if (unlink(p) == 0) n++;
    }
    closedir(d);
    ESP_LOGI(TAG, "cache svuotata: %d file", n);
}

// ------------------------------------------------------------------ disegno

static jpeg_decoder_handle_t s_jpg = NULL;

/* Il buffer dei pixel vive quanto l'oggetto LVGL che lo mostra: quando quello
   viene cancellato - cambiando vista, o ricostruendo una card - il buffer se
   ne va con lui. Senza questa riga ogni ridisegno lascerebbe indietro qualche
   megabyte di PSRAM, e il pannello se ne accorgerebbe solo dopo ore. */
static void libera_pixel(lv_event_t *e)
{
    lv_img_dsc_t *d = (lv_img_dsc_t *)lv_event_get_user_data(e);
    if (!d) return;
    free((void *)d->data);
    free(d);
}

/* JPEG -> immagine LVGL. L'acceleratore del P4 scrive a blocchi di 16 pixel,
   quindi il buffer di uscita e' piu' largo della figura: le colonne e le
   righe in piu' sono riempimento e vanno tolte, altrimenti si vedrebbe una
   fascia di spazzatura sul bordo destro e in basso. */
static lv_img_dsc_t *jpeg_in_memoria(const char *path)
{
    if (!s_jpg) {
        jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 1000 };
        if (jpeg_new_decoder_engine(&ecfg, &s_jpg) != ESP_OK) { s_jpg = NULL; return NULL; }
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long insize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (insize <= 0) { fclose(f); return NULL; }

    jpeg_decode_memory_alloc_cfg_t in_mem = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t in_alloc = 0;
    uint8_t *in = (uint8_t *)jpeg_alloc_decoder_mem(insize, &in_mem, &in_alloc);
    if (!in) { fclose(f); return NULL; }
    bool letto = fread(in, 1, insize, f) == (size_t)insize;
    fclose(f);
    if (!letto) { free(in); return NULL; }

    jpeg_decode_picture_info_t hdr;
    if (jpeg_decoder_get_info(in, insize, &hdr) != ESP_OK) { free(in); return NULL; }

    const uint32_t wa = (hdr.width + 15u) & ~15u;
    const uint32_t ha = (hdr.height + 15u) & ~15u;
    size_t serve = (size_t)wa * ha * 2;
    size_t libera = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (serve + 2 * 1024 * 1024 > libera) {
        ESP_LOGW(TAG, "%s: %ux%u troppo grande per la memoria libera",
                 path, (unsigned)hdr.width, (unsigned)hdr.height);
        free(in);
        return NULL;
    }

    jpeg_decode_memory_alloc_cfg_t out_mem = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t out_alloc = 0;
    uint8_t *grezzo = (uint8_t *)jpeg_alloc_decoder_mem(serve, &out_mem, &out_alloc);
    if (!grezzo) { free(in); return NULL; }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t prodotti = 0;
    esp_err_t e = jpeg_decoder_process(s_jpg, &dcfg, in, insize, grezzo, out_alloc, &prodotti);
    free(in);
    if (e != ESP_OK) { free(grezzo); return NULL; }

    /* Si ricopia riga per riga dentro un buffer della misura giusta. */
    size_t riga = (size_t)hdr.width * 2;
    uint8_t *pulito = (uint8_t *)heap_caps_malloc(riga * hdr.height,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pulito) { free(grezzo); return NULL; }
    for (uint32_t y = 0; y < hdr.height; y++)
        memcpy(pulito + y * riga, grezzo + (size_t)y * wa * 2, riga);
    free(grezzo);

    lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    if (!d) { free(pulito); return NULL; }
    d->header.cf       = LV_IMG_CF_TRUE_COLOR;
    d->header.always_zero = 0;
    d->header.w        = hdr.width;
    d->header.h        = hdr.height;
    d->data_size       = riga * hdr.height;
    d->data            = pulito;
    return d;
}

lv_obj_t *web_image_mostra(lv_obj_t *parent, const web_image_t *img, int w, int h)
{
    if (!parent || !img || !img->percorso[0]) return NULL;
    lv_obj_t *o = lv_img_create(parent);

    if (img->tipo == IMG_PNG) {
        /* Il PNG lo apre LVGL per conto suo, leggendo dal file: non passa
           dalla nostra memoria e non c'e' niente da liberare. */
        lv_img_set_src(o, img->lvgl);
    } else {
        lv_img_dsc_t *d = jpeg_in_memoria(img->percorso);
        if (!d) { lv_obj_del(o); return NULL; }
        lv_img_set_src(o, d);
        lv_obj_add_event_cb(o, libera_pixel, LV_EVENT_DELETE, d);
    }

    /* Ridimensionamento a proporzioni rispettate: una figura stirata si nota
       subito e fa sembrare rotto tutto il resto.

       La misura si legge dall'oggetto, non dal decoder: lv_img_set_src() ha
       appena dato all'immagine la dimensione della sorgente, e questo vale
       per il JPEG che abbiamo decodificato noi come per il PNG che apre LVGL.
       Chiederla al decoder funzionava per uno dei due e non per l'altro, e la
       figura usciva a grandezza naturale, tagliata dai bordi della card. */
    lv_coord_t iw = lv_obj_get_width(o);
    lv_coord_t ih = lv_obj_get_height(o);
    if (w > 0 && h > 0 && iw > 0 && ih > 0 && (iw > w || ih > h)) {
        int zx = (w * 256) / iw;
        int zy = (h * 256) / ih;
        int z = zx < zy ? zx : zy;
        if (z > 0 && z < 256) {
            lv_img_set_zoom(o, (uint16_t)z);
            /* Con lo zoom l'oggetto resta della misura di prima: glielo si
               dice, se no il riquadro intorno conta uno spazio che non c'e'
               piu'. */
            lv_obj_set_size(o, (iw * z) / 256, (ih * z) / 256);
        }
    }
    return o;
}

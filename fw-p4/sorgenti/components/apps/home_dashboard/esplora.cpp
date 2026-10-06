#include "esplora.h"
#include "backup.h"              // backup_e_segreto(): l'unico elenco dei segreti

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <string>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "esplora";

/* Le radici. Fuori da queste non si guarda: non per paranoia, ma perche' un
   indirizzo come /sdcard/../../dev/qualcosa non ha nessun motivo legittimo di
   arrivare qui, e l'unico modo di essere sicuri di non servirlo e' non
   provarci. */
static const char *const RADICI[] = { "/sdcard", "/spiffs", nullptr };

const char *esplora_radice(int i)
{
    if (i < 0) return nullptr;
    for (int k = 0; RADICI[k]; k++) if (k == i) return RADICI[k];
    return nullptr;
}

bool esplora_permesso(const char *percorso)
{
    if (!percorso || !*percorso) return false;
    size_t n = strlen(percorso);
    if (n >= 256) return false;

    /* ".." va rifiutato come segmento, non come sottostringa: un file che si
       chiama "foto..vecchie.jpg" e' legittimo. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)percorso[i];
        if (c < 0x20 || c == 0x7f) return false;          // caratteri di controllo
        if (percorso[i] == '.' && percorso[i + 1] == '.') {
            bool inizio = (i == 0) || percorso[i - 1] == '/';
            bool fine   = (percorso[i + 2] == 0) || percorso[i + 2] == '/';
            if (inizio && fine) return false;
        }
    }

    for (int k = 0; RADICI[k]; k++) {
        size_t r = strlen(RADICI[k]);
        if (strncmp(percorso, RADICI[k], r) != 0) continue;
        if (percorso[r] == 0 || percorso[r] == '/') return true;   // la radice o dentro
    }
    return false;
}

// ------------------------------------------------------------------ elenco

int esplora_elenca(const char *percorso, esplora_voce_t *v, int max, bool *troncato)
{
    if (troncato) *troncato = false;
    if (!v || max <= 0 || !esplora_permesso(percorso)) return -1;

    DIR *d = opendir(percorso);
    if (!d) {
        ESP_LOGW(TAG, "%s non si apre: %s", percorso, strerror(errno));
        return -1;
    }

    /* Le voci si raccolgono in PSRAM e non sullo stack: questo codice gira sul
       task del server web, che di stack ne ha poco, e duecento voci da settanta
       byte sono quattordici kilobyte. Lo stesso errore - struct con std::string
       sullo stack - ci e' gia' costato un "Stack protection fault" nel task che
       scarica le immagini. */
    std::vector<esplora_voce_t> *tutte = new (std::nothrow) std::vector<esplora_voce_t>();
    if (!tutte) { closedir(d); return -1; }

    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (tutte->size() >= 2000) { if (troncato) *troncato = true; break; }

        esplora_voce_t x = {};
        snprintf(x.nome, sizeof(x.nome), "%s", e->d_name);
        x.cartella = (e->d_type == DT_DIR);

        /* La dimensione la sa solo stat, e stat costa: su una cartella con
           molti file e' il pezzo lento. Sulle cartelle non si chiama. */
        if (!x.cartella) {
            char pieno[320];
            snprintf(pieno, sizeof(pieno), "%s/%s", percorso, e->d_name);
            struct stat st = {};
            if (stat(pieno, &st) == 0) {
                x.byte = (uint32_t)st.st_size;
                x.quando = st.st_mtime;
            }
        }
        tutte->push_back(x);
    }
    closedir(d);

    // cartelle prima, poi file, dentro i gruppi in ordine alfabetico
    std::sort(tutte->begin(), tutte->end(), [](const esplora_voce_t &a, const esplora_voce_t &b) {
        if (a.cartella != b.cartella) return a.cartella;
        return strcasecmp(a.nome, b.nome) < 0;
    });

    int n = (int)tutte->size();
    if (n > max) { n = max; if (troncato) *troncato = true; }
    for (int i = 0; i < n; i++) v[i] = (*tutte)[i];
    delete tutte;
    return n;
}

bool esplora_elimina(const char *percorso)
{
    if (!esplora_permesso(percorso)) return false;

    /* Una radice non si cancella nemmeno se qualcuno ci prova: /sdcard non e'
       un file, e il tentativo di toglierlo non deve nemmeno partire. */
    for (int k = 0; RADICI[k]; k++)
        if (!strcmp(percorso, RADICI[k])) return false;

    struct stat st = {};
    if (stat(percorso, &st) != 0) return false;

    int e = S_ISDIR(st.st_mode) ? rmdir(percorso) : unlink(percorso);
    if (e != 0) {
        ESP_LOGW(TAG, "%s non si cancella: %s", percorso, strerror(errno));
        return false;
    }
    /* A livello di avviso e non di informazione: una cancellazione e' una cosa
       che non si disfa, e nel registro deve saltare all'occhio. */
    ESP_LOGW(TAG, "cancellato %s", percorso);
    return true;
}

bool esplora_spazio(const char *percorso, uint64_t *totale, uint64_t *libero)
{
    if (!esplora_permesso(percorso)) return false;

    if (!strncmp(percorso, "/sdcard", 7)) {
        uint64_t t = 0, l = 0;
        if (esp_vfs_fat_info("/sdcard", &t, &l) != ESP_OK) return false;
        if (totale) *totale = t;
        if (libero) *libero = l;
        return true;
    }
    if (!strncmp(percorso, "/spiffs", 7)) {
        size_t t = 0, usati = 0;
        if (esp_spiffs_info(CONFIG_BSP_SPIFFS_PARTITION_LABEL, &t, &usati) != ESP_OK) return false;
        if (totale) *totale = t;
        if (libero) *libero = t > usati ? t - usati : 0;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ NVS

static const char *nome_tipo(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8:   return "u8";
    case NVS_TYPE_I8:   return "i8";
    case NVS_TYPE_U16:  return "u16";
    case NVS_TYPE_I16:  return "i16";
    case NVS_TYPE_U32:  return "u32";
    case NVS_TYPE_I32:  return "i32";
    case NVS_TYPE_U64:  return "u64";
    case NVS_TYPE_I64:  return "i64";
    case NVS_TYPE_STR:  return "testo";
    case NVS_TYPE_BLOB: return "blob";
    default:            return "?";
    }
}

/* Riempie valore/byte di una voce leggendola per davvero. Il testo lungo viene
   troncato con dei puntini: questa e' una finestra per capire cosa c'e', non un
   modo di estrarre dati. Chi vuole tutto usa il salvataggio della
   configurazione, che e' fatto per quello. */
static void leggi_valore(nvs_handle_t h, const char *chiave, nvs_type_t t, esplora_nvs_t *x)
{
    switch (t) {
    case NVS_TYPE_U8:  { uint8_t  u = 0; if (!nvs_get_u8 (h, chiave, &u)) snprintf(x->valore, sizeof(x->valore), "%u", u); break; }
    case NVS_TYPE_I8:  { int8_t   i = 0; if (!nvs_get_i8 (h, chiave, &i)) snprintf(x->valore, sizeof(x->valore), "%d", i); break; }
    case NVS_TYPE_U16: { uint16_t u = 0; if (!nvs_get_u16(h, chiave, &u)) snprintf(x->valore, sizeof(x->valore), "%u", u); break; }
    case NVS_TYPE_I16: { int16_t  i = 0; if (!nvs_get_i16(h, chiave, &i)) snprintf(x->valore, sizeof(x->valore), "%d", i); break; }
    case NVS_TYPE_U32: { uint32_t u = 0; if (!nvs_get_u32(h, chiave, &u)) snprintf(x->valore, sizeof(x->valore), "%lu", (unsigned long)u); break; }
    case NVS_TYPE_I32: { int32_t  i = 0; if (!nvs_get_i32(h, chiave, &i)) snprintf(x->valore, sizeof(x->valore), "%ld", (long)i); break; }
    case NVS_TYPE_U64: { uint64_t u = 0; if (!nvs_get_u64(h, chiave, &u)) snprintf(x->valore, sizeof(x->valore), "%llu", (unsigned long long)u); break; }
    case NVS_TYPE_I64: { int64_t  i = 0; if (!nvs_get_i64(h, chiave, &i)) snprintf(x->valore, sizeof(x->valore), "%lld", (long long)i); break; }
    case NVS_TYPE_STR: {
        size_t sz = 0;
        if (nvs_get_str(h, chiave, nullptr, &sz) != ESP_OK || sz == 0) break;
        x->byte = (uint32_t)sz;
        /* In PSRAM: un certificato in NVS e' qualche kilobyte, e di stack qui
           non se ne puo' prendere tanto. */
        char *buf = (char *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!buf) break;
        if (nvs_get_str(h, chiave, buf, &sz) == ESP_OK) {
            /* Le andate a capo diventano spazi: una riga d'elenco con dentro
               un ritorno a capo manda a gambe all'aria qualunque tabella. */
            for (char *p = buf; *p; p++) if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
            snprintf(x->valore, sizeof(x->valore), "%s", buf);
            if (sz > sizeof(x->valore)) {
                x->valore[sizeof(x->valore) - 4] = 0;
                strcat(x->valore, "...");
            }
        }
        heap_caps_free(buf);
        break;
    }
    case NVS_TYPE_BLOB: {
        size_t sz = 0;
        if (nvs_get_blob(h, chiave, nullptr, &sz) == ESP_OK) x->byte = (uint32_t)sz;
        snprintf(x->valore, sizeof(x->valore), "%u byte", (unsigned)x->byte);
        break;
    }
    default: break;
    }
}

int esplora_nvs(esplora_nvs_t *v, int max, bool *troncato)
{
    if (troncato) *troncato = false;
    if (!v || max <= 0) return -1;

    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &it);
    if (e != ESP_OK) return 0;        // NVS vuota: non e' un guasto

    int n = 0;
    while (e == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (n >= max) { if (troncato) *troncato = true; break; }

        esplora_nvs_t *x = &v[n];
        memset(x, 0, sizeof(*x));
        snprintf(x->spazio, sizeof(x->spazio), "%s", info.namespace_name);
        snprintf(x->chiave, sizeof(x->chiave), "%s", info.key);
        snprintf(x->tipo, sizeof(x->tipo), "%s", nome_tipo(info.type));
        x->segreto = backup_e_segreto(info.namespace_name, info.key);

        nvs_handle_t h;
        if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
            if (x->segreto) {
                /* Dei segreti si dice che ci sono e quanto sono grandi: serve a
                   sapere se il certificato e' stato generato o se il PIN e'
                   impostato, che e' la domanda vera. Il valore no. */
                size_t sz = 0;
                if (info.type == NVS_TYPE_STR) nvs_get_str(h, info.key, nullptr, &sz);
                else if (info.type == NVS_TYPE_BLOB) nvs_get_blob(h, info.key, nullptr, &sz);
                x->byte = (uint32_t)sz;
                snprintf(x->valore, sizeof(x->valore), "(non mostrato)");
            } else {
                leggi_valore(h, info.key, info.type, x);
            }
            nvs_close(h);
        }
        n++;
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

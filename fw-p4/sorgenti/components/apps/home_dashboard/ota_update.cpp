#include "ota_update.h"
#include "ha_http.h"          // ha_tls_ca(), ha_http_base()
#include "ha_config.h"        // HA_URL_MAX, HA_TOKEN_MAX
#include "ha_token.h"         // il permesso per chiedere il file a Home Assistant
#include "ha_ws.h"            // ha_ws_connected(): la prova che il pannello funziona
#include "avviso_ui.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <new>              // std::nothrow
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "psa/crypto.h"

static const char *TAG = "ota";

/* La fetta con cui si scarica. Grande quanto basta a non fare mille giri, e
   piccola abbastanza da non soffocare il collegamento SDIO verso il C6: le
   raffiche grosse su quel bus sono la cosa che lo fa cadere, ed e' il motivo
   per cui tutto il traffico pesante di questo pannello va a pezzi. */
#define FETTA 4096

/* Quanto deve reggere un firmware nuovo prima di essere dato per buono: due
   minuti in piedi E collegato a Home Assistant. Non basta "si e' avviato" -
   un firmware che parte e non riesce piu' a parlare con HA e' inutile quanto
   uno che va in crash, e da appeso al muro non si distinguono. */
#define PROVA_S 120

static SemaphoreHandle_t s_mtx = nullptr;
static ota_info_t        s_info;
static volatile bool     s_in_corso = false;

struct Lavoro { char url[400]; char sha[72]; uint32_t byte; };

static void segna(ota_stato_t st, int pct, const char *msg)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_info.stato = st;
    if (pct >= 0) s_info.pct = pct;
    if (msg) snprintf(s_info.messaggio, sizeof(s_info.messaggio), "%s", msg);
    xSemaphoreGive(s_mtx);
}

// ------------------------------------------------------------------ impronta

static bool esadecimale_uguale(const uint8_t *impronta, const char *atteso)
{
    char mio[65];
    for (int i = 0; i < 32; i++) snprintf(mio + i * 2, 3, "%02x", impronta[i]);
    mio[64] = 0;
    /* Senza badare alle maiuscole: chi calcola l'impronta puo' scriverla in un
       modo o nell'altro, e rifiutare un aggiornamento buono per una lettera
       maiuscola sarebbe un guasto assurdo da diagnosticare. */
    return strcasecmp(mio, atteso) == 0;
}

// ------------------------------------------------------------------ il lavoro

static void ota_task(void *arg)
{
    Lavoro *l = (Lavoro *)arg;
    const esp_partition_t *dest = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    bool aperta = false;
    char *buf = nullptr;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool hash_vivo = false;
    uint32_t fatti = 0;
    char msg[112];

    if (!dest) {
        /* Succede solo se qualcuno rimette una tabella delle partizioni senza
           slot OTA. Vale la pena dirlo per esteso: il sintomo da fuori sarebbe
           "l'aggiornamento non parte" e nessuno penserebbe alle partizioni. */
        segna(OTA_FALLITO, 0, "nessuna partizione OTA: tabella senza ota_0/ota_1");
        ESP_LOGE(TAG, "nessuna partizione dove scrivere");
        goto fine;
    }
    ESP_LOGI(TAG, "scrivo in %s (0x%lx, %lu byte)", dest->label,
             (unsigned long)dest->address, (unsigned long)dest->size);

    if (l->byte && l->byte > dest->size) {
        snprintf(msg, sizeof(msg), "immagine da %lu byte: non ci sta in %lu",
                 (unsigned long)l->byte, (unsigned long)dest->size);
        segna(OTA_FALLITO, 0, msg);
        goto fine;
    }

    buf = (char *)heap_caps_malloc(FETTA, MALLOC_CAP_SPIRAM);
    if (!buf) { segna(OTA_FALLITO, 0, "niente memoria"); goto fine; }

    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        segna(OTA_FALLITO, 0, "non riesco a calcolare l'impronta");
        goto fine;
    }
    hash_vivo = true;

    {
        /* L'indirizzo puo' arrivare intero oppure come solo percorso.

           Home Assistant manda la seconda forma - "/api/crowpanel/firmware/..." -
           perche' non sa a che indirizzo il pannello lo raggiunge: ci sono di
           mezzo nomi di rete, porte e reverse proxy, e l'unico che sa come ci
           parla e' il pannello. Lo stesso fanno le foto e il salvataggio.

           Senza risolverlo, esp_http_client_init non riesce nemmeno a
           interpretarlo e torna NULL: il sintomo era "non riesco a chiedere il
           file", che accusava la rete per un indirizzo mai costruito. */
        char url[HA_URL_MAX * 2];
        char base[HA_URL_MAX];
        bool nostro = false;
        bool so_base = ha_http_base(base, sizeof(base));
        if (l->url[0] == '/') {
            if (!so_base) {
                segna(OTA_FALLITO, 0, "non so a che indirizzo sta Home Assistant");
                goto fine;
            }
            snprintf(url, sizeof(url), "%s%s", base, l->url);
            nostro = true;
        } else {
            snprintf(url, sizeof(url), "%s", l->url);
            if (so_base) {
                size_t nb = strlen(base);
                nostro = strncasecmp(url, base, nb) == 0 &&
                         (url[nb] == 0 || url[nb] == '/' || url[nb] == '?');
            }
        }

        /* Il permesso va allegato solo se il file sta davvero su Home
           Assistant: quel permesso apre tutta la casa, e mandarlo a un server
           qualunque indicato nel comando vorrebbe dire consegnarglielo. Stessa
           regola dell'audio della voce. */
        char bearer[HA_TOKEN_MAX + 16] = "";
        if (nostro) {
            char token[HA_TOKEN_MAX];
            if (!ha_token_get_access(token, sizeof(token))) {
                segna(OTA_FALLITO, 0, "non ho il permesso per chiedere il file a Home Assistant");
                goto fine;
            }
            snprintf(bearer, sizeof(bearer), "Bearer %s", token);
        }
        ESP_LOGI(TAG, "prendo il firmware da %s%s", url, nostro ? " (con il permesso)" : "");

        esp_http_client_config_t c = {};
        c.url = url;
        c.timeout_ms = 20000;
        c.keep_alive_enable = true;
        if (!strncmp(url, "https://", 8)) {
            const char *ca = ha_tls_ca();
            if (ca) c.cert_pem = ca; else c.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_http_client_handle_t cl = esp_http_client_init(&c);
        if (!cl) { segna(OTA_FALLITO, 0, "indirizzo non valido"); goto fine; }
        if (bearer[0]) esp_http_client_set_header(cl, "Authorization", bearer);

        if (esp_http_client_open(cl, 0) != ESP_OK) {
            esp_http_client_cleanup(cl);
            segna(OTA_FALLITO, 0, "non risponde");
            goto fine;
        }
        int lung = esp_http_client_fetch_headers(cl);
        int stato = esp_http_client_get_status_code(cl);
        if (stato != 200) {
            snprintf(msg, sizeof(msg), "risposta %d invece di 200", stato);
            esp_http_client_cleanup(cl);
            segna(OTA_FALLITO, 0, msg);
            goto fine;
        }
        uint32_t attesi = l->byte ? l->byte : (lung > 0 ? (uint32_t)lung : 0);

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_info.byte_attesi = attesi;
        s_info.byte_fatti = 0;
        xSemaphoreGive(s_mtx);
        segna(OTA_SCARICA, 0, "scarico");

        /* OTA_SIZE_UNKNOWN cancella tutta la partizione: va bene, ed e' anche
           l'unica scelta onesta quando la dimensione non arriva. */
        if (esp_ota_begin(dest, attesi ? attesi : OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
            esp_http_client_cleanup(cl);
            segna(OTA_FALLITO, 0, "non riesco ad aprire la partizione");
            goto fine;
        }
        aperta = true;

        int n;
        while ((n = esp_http_client_read(cl, buf, FETTA)) > 0) {
            if (esp_ota_write(h, buf, n) != ESP_OK) {
                segna(OTA_FALLITO, 0, "scrittura in flash fallita");
                esp_http_client_cleanup(cl);
                goto fine;
            }
            if (psa_hash_update(&op, (const uint8_t *)buf, n) != PSA_SUCCESS) {
                segna(OTA_FALLITO, 0, "impronta fallita");
                esp_http_client_cleanup(cl);
                goto fine;
            }
            fatti += n;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_info.byte_fatti = fatti;
            s_info.pct = attesi ? (int)((uint64_t)fatti * 100 / attesi) : 0;
            xSemaphoreGive(s_mtx);
        }
        esp_http_client_cleanup(cl);

        if (n < 0) { segna(OTA_FALLITO, 0, "collegamento interrotto"); goto fine; }
        if (attesi && fatti != attesi) {
            snprintf(msg, sizeof(msg), "arrivati %lu byte su %lu",
                     (unsigned long)fatti, (unsigned long)attesi);
            segna(OTA_FALLITO, 0, msg);
            goto fine;
        }
    }

    // ---- l'impronta, PRIMA di dire al bootloader qualunque cosa
    segna(OTA_VERIFICA, 100, "verifico l'impronta");
    {
        uint8_t impronta[32];
        size_t quanti = 0;
        hash_vivo = false;
        if (psa_hash_finish(&op, impronta, sizeof(impronta), &quanti) != PSA_SUCCESS ||
            quanti != 32) {
            segna(OTA_FALLITO, 0, "impronta non calcolabile");
            goto fine;
        }
        if (!esadecimale_uguale(impronta, l->sha)) {
            /* Questa e' la ragione per cui esiste tutto il meccanismo. L'immagine
               si butta e il bootloader non la vede nemmeno: esp_ota_abort chiude
               senza toccare la tabella di avvio. */
            ESP_LOGE(TAG, "IMPRONTA DIVERSA: l'immagine arrivata non e' quella mandata");
            segna(OTA_FALLITO, 0, "impronta diversa: immagine scartata");
            goto fine;
        }
    }

    /* esp_ota_end fa il suo controllo: che sia davvero un'applicazione valida
       per questo chip. E' un controllo diverso dal nostro e si sommano - il
       nostro dice "e' arrivata intera", questo dice "e' avviabile". */
    if (esp_ota_end(h) != ESP_OK) {
        aperta = false;
        segna(OTA_FALLITO, 0, "immagine non valida per questo chip");
        goto fine;
    }
    aperta = false;

    if (esp_ota_set_boot_partition(dest) != ESP_OK) {
        segna(OTA_FALLITO, 0, "non riesco a impostare l'avvio");
        goto fine;
    }

    ESP_LOGW(TAG, "firmware installato in %s: al prossimo riavvio parte da li', in prova",
             dest->label);
    segna(OTA_PRONTO, 100, "installato: riavvio fra poco");
    avviso_ui_mostra("Firmware aggiornato: il pannello si riavvia.");

    if (buf) { heap_caps_free(buf); buf = nullptr; }
    delete l;
    l = nullptr;              // il fine: qui sotto non deve liberarlo una seconda volta
    s_in_corso = false;
    /* Il riavvio si prende con calma: il messaggio deve restare a schermo
       abbastanza per essere letto, e chi ha chiesto l'aggiornamento deve avere
       il tempo di ricevere la risposta. */
    vTaskDelay(pdMS_TO_TICKS(4000));
    esp_restart();

fine:
    if (hash_vivo) psa_hash_abort(&op);
    if (aperta) esp_ota_abort(h);
    if (buf) heap_caps_free(buf);
    {
        ota_info_t i;
        ota_update_stato(&i);
        if (i.stato == OTA_FALLITO) ESP_LOGE(TAG, "aggiornamento non riuscito: %s", i.messaggio);
    }
    delete l;
    s_in_corso = false;
    vTaskDelete(NULL);
}

bool ota_update_avvia(const char *url, const char *sha256, uint32_t byte)
{
    if (!s_mtx) ota_update_init();
    if (s_in_corso) {
        ESP_LOGW(TAG, "un aggiornamento e' gia' in corso");
        return false;
    }
    if (!url || !*url) return false;

    /* L'impronta si pretende, e si pretende ben formata: senza, l'unica
       verifica rimasta sarebbe quella di ESP-IDF, che dice "e' un firmware" ma
       non "e' quello che hai mandato". Un aggiornamento senza impronta non si
       fa. */
    if (!sha256 || strlen(sha256) != 64) {
        ESP_LOGE(TAG, "serve un'impronta SHA-256 di 64 caratteri");
        return false;
    }
    for (const char *p = sha256; *p; p++)
        if (!isxdigit((unsigned char)*p)) {
            ESP_LOGE(TAG, "l'impronta non e' esadecimale");
            return false;
        }

    Lavoro *l = new (std::nothrow) Lavoro();
    if (!l) return false;
    snprintf(l->url, sizeof(l->url), "%s", url);
    snprintf(l->sha, sizeof(l->sha), "%s", sha256);
    l->byte = byte;

    s_in_corso = true;
    segna(OTA_SCARICA, 0, "comincio");
    /* Stack largo: qui dentro c'e' una stretta di mano TLS, il motore
       dell'impronta e il driver della flash. Il task della voce e quello delle
       immagini ci sono gia' passati, con il pannello che moriva per "Stack
       protection fault". */
    if (xTaskCreate(ota_task, "ota", 12288, l, 4, NULL) != pdPASS) {
        delete l;
        s_in_corso = false;
        segna(OTA_FALLITO, 0, "niente memoria per il task");
        return false;
    }
    return true;
}

bool ota_update_in_corso(void) { return s_in_corso; }

const char *ota_parole(ota_stato_t st)
{
    switch (st) {
    case OTA_SCARICA:  return "scarica";
    case OTA_VERIFICA: return "verifica";
    case OTA_PRONTO:   return "pronto";
    case OTA_FALLITO:  return "fallito";
    default:           return "fermo";
    }
}

void ota_update_stato(ota_info_t *out)
{
    if (!out) return;
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_mtx);
}

void ota_update_versione(char *out, size_t sz)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) { snprintf(out, sz, "?"); return; }
    snprintf(out, sz, "%s %s (%s %s)", d->project_name, d->version, d->date, d->time);
}

// ------------------------------------------------------------------ la prova

/* L'immagine appena installata va confermata, altrimenti al riavvio successivo
   il bootloader rimette quella di prima. La conferma arriva solo dopo che il
   pannello ha dimostrato di funzionare: in piedi e collegato a Home Assistant,
   senza interruzioni, per PROVA_S secondi.

   Il conto riparte da zero se il collegamento cade: due minuti spezzettati non
   dimostrano niente. */
static void prova_task(void *arg)
{
    int di_fila = 0;
    while (di_fila < PROVA_S) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ha_ws_connected()) di_fila++;
        else                   di_fila = 0;
    }
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "firmware confermato: ha retto %d s collegato a Home Assistant", PROVA_S);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_info.in_prova = false;
        xSemaphoreGive(s_mtx);
    } else {
        ESP_LOGE(TAG, "non riesco a confermare il firmware: %s", esp_err_to_name(e));
    }
    vTaskDelete(NULL);
}

void ota_update_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        memset(&s_info, 0, sizeof(s_info));
    }

    char v[96];
    ota_update_versione(v, sizeof(v));
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "in esecuzione da %s: %s", run ? run->label : "?", v);

    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_info.in_prova = true;
        ESP_LOGW(TAG, "firmware IN PROVA: lo confermo dopo %d s collegato a Home "
                      "Assistant, altrimenti al prossimo riavvio torna il precedente",
                 PROVA_S);
        xTaskCreate(prova_task, "ota_prova", 3072, nullptr, 2, nullptr);
    }
}

#include "ha_plugin.h"
#include "ha_ws.h"
#include "ha_config.h"
#include "backup.h"
#include "json_escape.h"
#include "web_image.h"
#include "net_config.h"
#include "standby_show.h"
#include "idle_manager.h"
#include "tts_player.h"
#include "avviso_ui.h"
#include "hosted_recovery.h"

#include <string>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ha_plugin";
static const char *NS  = "hapanel";
static const char *K_OWNS = "ha_slides";

#define EVENT_NAME   "crowpanel_command"
#define STATE_EVERY_S 30

static bool s_paired = false;
static bool s_owns   = false;          // la configurazione dello slideshow la fa HA
static bool s_told   = false;          // "integrazione assente" gia' detto una volta
static TaskHandle_t s_task = NULL;

// ------------------------------------------------------------------ utilita'

/* Identificativo stabile con cui il pannello si fa riconoscere da HA.
   Il MAC del Wi-Fi qui non si puo' chiedere al solito modo: la radio sta sul
   C6, e il P4 da solo risponde tutto zeri. Lo si prende dall'interfaccia di
   rete, che il suo indirizzo ce l'ha davvero; se anche quella tace (rete non
   ancora avviata) si ripiega su un numero sorteggiato una volta e conservato,
   cosi' il pannello resta lo stesso dispositivo anche dopo un riavvio. */
void ha_plugin_id(char *out, size_t sz)
{
    uint8_t mac[6] = {0};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif || esp_netif_get_mac(nif, mac) != ESP_OK) memset(mac, 0, sizeof(mac));
    bool zero = true;
    for (int i = 0; i < 6; i++) if (mac[i]) zero = false;
    if (zero && esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        for (int i = 0; i < 6; i++) if (mac[i]) zero = false;

    if (zero) {
        static char saved[16];
        if (!saved[0]) {
            nvs_handle_t h;
            size_t n = sizeof(saved);
            if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
                if (nvs_get_str(h, "ha_id", saved, &n) != ESP_OK) saved[0] = 0;
                nvs_close(h);
            }
            if (!saved[0]) {
                uint32_t a = esp_random(), b = esp_random();
                snprintf(saved, sizeof(saved), "%08lx%04lx",
                         (unsigned long)a, (unsigned long)(b & 0xffff));
                if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, "ha_id", saved);
                    nvs_commit(h);
                    nvs_close(h);
                }
            }
        }
        strlcpy(out, saved, sz);
        return;
    }
    snprintf(out, sz, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool ha_plugin_paired(void)         { return s_paired; }
bool ha_plugin_owns_slideshow(void) { return s_owns; }

static void owns_save(bool v)
{
    s_owns = v;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, K_OWNS, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void owns_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, K_OWNS, &v);
        nvs_close(h);
    }
    s_owns = v != 0;
}

void ha_plugin_release_slideshow(void)
{
    ESP_LOGI(TAG, "lo slideshow torna sotto il pannello");
    owns_save(false);
}

static std::string ip_now(void)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif) esp_netif_get_ip_info(nif, &ip);
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));
    return b;
}

/* Il testo va dentro una stringa JSON: virgolette e barre vanno protette,
   altrimenti un nome con l'apostrofo tipografico o una barra rompe il
   messaggio e HA scarta tutto. */
static std::string esc(const char *s)
{
    std::string o;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
        else if ((unsigned char)*p < 0x20) continue;
        else o += *p;
    }
    return o;
}

/* Configurazione dello slideshow mandata da Home Assistant: la applico e la
   scrivo nella copia in flash. Cosi' se domani HA non risponde lo slideshow
   parte lo stesso, con l'ultima configurazione conosciuta. */
static void apply_slideshow(const cJSON *cfgj)
{
    if (!cJSON_IsObject(cfgj)) return;
    standby_cfg_t c;
    standby_config_load(&c);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfgj, "slideshow_dopo_min"))) c.slideshow_after_s  = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfgj, "spegni_dopo_min")))    c.screen_off_after_s = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfgj, "secondi_foto")))       c.photo_seconds      = v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(cfgj, "casuale")))              c.shuffle            = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(cfgj, "cartella")))           strlcpy(c.folder, v->valuestring, sizeof(c.folder));
    standby_overlay_from_json(cJSON_GetObjectItem(cfgj, "sovrimpressione"), &c);
    standby_config_save(&c);
    idle_manager_reload_config();
    ESP_LOGI(TAG, "slideshow configurato da Home Assistant (%d valori)", c.n_overlay);
}

// ------------------------------------------------------------------ annuncio

static void on_announce(bool ok, cJSON *result, const char *error, void *ctx)
{
    (void)ctx;
    if (!ok) {
        s_paired = false;
        if (!s_told) {
            ESP_LOGI(TAG, "integrazione non installata in Home Assistant (%s): "
                          "il pannello funziona lo stesso", error ? error : "?");
            s_told = true;
        }
        /* HA ha risposto, e ha risposto che quel comando non lo conosce:
           l'integrazione non c'e' piu'. Se prima comandava lei lo slideshow,
           il comando torna al pannello, altrimenti la pagina web resterebbe
           in sola lettura per sempre in attesa di un HA che non risponde. */
        if (s_owns) {
            ESP_LOGI(TAG, "lo slideshow torna sotto il pannello");
            owns_save(false);
        }
        return;
    }
    s_paired = true;
    s_told = false;

    /* HA dice se e' lui a tenere la configurazione dello slideshow, e in quel
       caso qual e'. */
    bool managed = cJSON_IsTrue(cJSON_GetObjectItem(result, "managed"));
    if (managed) apply_slideshow(cJSON_GetObjectItem(result, "config"));
    if (managed != s_owns) owns_save(managed);
    ESP_LOGI(TAG, "integrazione collegata (slideshow: %s)", managed ? "lo fa HA" : "lo fa il pannello");

    if (s_task) xTaskNotifyGive(s_task);          // manda subito lo stato
}

static void announce(void)
{
    char id[16];
    ha_plugin_id(id, sizeof(id));
    net_config_t n;
    net_config_load(&n);
    const esp_app_desc_t *app = esp_app_get_description();

    std::string body =
        "\"type\":\"crowpanel/announce\",\"panel\":{"
        "\"id\":\"" + std::string(id) + "\","
        "\"nome\":\"" + esc(n.hostname) + "\","
        "\"stanza\":\"" + esc(n.room) + "\","
        "\"modello\":\"CrowPanel Advance 10.1\","
        "\"versione\":\"" + esc(app ? app->version : "?") + "\","
        "\"indirizzo\":\"" + ip_now() + "\","
        "\"max_valori\":" + std::to_string(STANDBY_MAX_OVERLAY) + "}";
    if (ha_ws_request(body.c_str(), on_announce, NULL) < 0)
        ESP_LOGW(TAG, "annuncio non partito");
}

/* Perche' il pannello si e' riavviato l'ultima volta. Serve soprattutto per
   una ragione: dopo un crash il pannello adesso riparte da solo, e senza
   questo nessuno si accorgerebbe che e' successo qualcosa. */
const char *ha_plugin_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "accensione";
    case ESP_RST_EXT:      return "pulsante di reset";
    case ESP_RST_SW:       return "riavvio richiesto";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:  return "blocco (watchdog interrupt)";
    case ESP_RST_TASK_WDT: return "blocco (watchdog task)";
    case ESP_RST_WDT:      return "blocco (watchdog)";
    case ESP_RST_BROWNOUT: return "alimentazione insufficiente";
    default:               return "sconosciuto";
    }
}

// -------------------------------------------------------------------- stato

/* Letta dal coprocessore STC8, che il pannello interroga gia' per conto suo. */
extern "C" void panel_alimentazione(uint32_t *mv, uint8_t *percento, uint8_t *stato);

static void push_state(void)
{
    if (!s_paired) return;
    char id[16];
    ha_plugin_id(id, sizeof(id));
    char dash[HA_DASH_MAX];
    int view = 0;
    ha_config_load_dash(dash, sizeof(dash), &view);

    uint32_t ali_mv = 0;
    uint8_t  ali_pct = 0, ali_stato = 0;
    panel_alimentazione(&ali_mv, &ali_pct, &ali_stato);

    char body[512];
    snprintf(body, sizeof(body),
             /* "pannello" e non "id": nel protocollo di HA "id" e' gia' il
                numero progressivo del messaggio, e due chiavi uguali nello
                stesso oggetto si coprono a vicenda. */
             "\"type\":\"crowpanel/state\",\"pannello\":\"%s\",\"stato\":{"
             "\"schermo\":%s,\"luminosita\":%d,\"dashboard\":\"%s\",\"vista\":%d,"
             "\"acceso_da\":%lld,\"ram_interna\":%u,\"ram_psram\":%u,\"recuperi\":%u,"
             "\"riavvio\":\"%s\","
             /* Alimentazione. "stato" e' il numero grezzo che manda il
                coprocessore: cosa significhi non e' documentato da nessuna
                parte, e inventarsi che 2 vuol dire "in carica" sarebbe un
                sensore che mente. Lo si pubblica com'e', e appena lo si vede
                cambiare - staccando la corrente, attaccando una batteria - si
                sa cosa vuol dire e gli si da' un nome. */
             "\"alimentazione_mv\":%u,\"alimentazione_pct\":%u,\"alimentazione_stato\":%u}",
             id,
             idle_manager_screen_on() ? "true" : "false",
             idle_manager_brightness(),
             esc(dash).c_str(), view,
             (long long)(esp_timer_get_time() / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             hosted_recovery_count(), ha_plugin_reset_reason(),
             (unsigned)ali_mv, (unsigned)ali_pct, (unsigned)ali_stato);
    ha_ws_request(body, NULL, NULL);
}

// ------------------------------------------------------------------ comandi

/* Cambiare dashboard vuol dire ricollegarsi: il giro passa da un task a parte
   perche' fermare il WebSocket da dentro il suo stesso task lo blocca. */
extern "C" void home_dashboard_reconnect(void);

static void reconnect_task(void *arg)
{
    (void)arg;
    home_dashboard_reconnect();
    vTaskDelete(NULL);
}

/* La via di ritorno del salvataggio verso Home Assistant.

   Il pannello e HA si parlano in un verso solo: HA manda eventi sul bus, il
   pannello manda comandi WebSocket. Per far tornare indietro un file serviva
   quindi un comando nuovo, "crowpanel/backup", e non una risposta - perche'
   l'evento che l'ha chiesto non ne ha una.

   Il lavoro si fa in un task suo: esportare vuol dire percorrere tutta la
   memoria NVS, e farlo dentro la gestione di un evento terrebbe fermo il
   filo del WebSocket per il tempo che ci vuole. */
struct LavoroBackup {
    bool chiave;          // true = manda la chiave di recupero, non il file
    bool cifrato;
    std::string richiesta;
};

static void manda_backup_task(void *arg)
{
    LavoroBackup *l = (LavoroBackup *)arg;
    char id[16];
    ha_plugin_id(id, sizeof(id));

    std::string tipo, contenuto;
    if (l->chiave) {
        char k[BACKUP_CHIAVE_MAX];
        if (backup_chiave_testo(k, sizeof(k))) { tipo = "chiave"; contenuto = k; }
        backup_chiave_segna_presa();
    } else {
        char *j = backup_esporta(l->cifrato);
        if (j) { tipo = l->cifrato ? "cifrato" : "chiaro"; contenuto = j; free(j); }
    }

    if (tipo.empty()) {
        ESP_LOGE(TAG, "salvataggio per Home Assistant non riuscito");
    } else {
        /* Il contenuto va scritto come stringa JSON: dentro c'e' altro JSON,
           pieno di virgolette che vanno protette. */
        std::string esc = json_escape(contenuto.c_str());
        std::string body = std::string("\"type\":\"crowpanel/backup\",\"pannello\":\"") + id +
                           "\",\"richiesta\":\"" + l->richiesta +
                           "\",\"tipo\":\"" + tipo +
                           /* json_escape mette gia' le virgolette: aggiungerne
                              altre faceva un "dati":""...""  che Home Assistant
                              scartava senza dire niente. */
                           "\",\"dati\":" + esc;
        ha_ws_request(body.c_str(), NULL, NULL);
        ESP_LOGI(TAG, "mandato a Home Assistant: %s (%u byte)",
                 tipo.c_str(), (unsigned)contenuto.size());
    }
    delete l;
    vTaskDelete(NULL);
}

static void avvia_backup(bool chiave, bool cifrato, const char *richiesta)
{
    LavoroBackup *l = new LavoroBackup();
    l->chiave = chiave;
    l->cifrato = cifrato;
    l->richiesta = richiesta ? richiesta : "";
    /* Lo stack e' largo: dentro ci girano la lettura della NVS, cJSON e la
       cifratura, e il file finito puo' arrivare a diversi kilobyte. */
    if (xTaskCreate(manda_backup_task, "ha_backup", 12288, l, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "niente memoria per preparare il salvataggio");
        delete l;
    }
}

/* ---- le foto dello slideshow ----

   Home Assistant le converte prima di mandarle - le rimpicciolisce a misura
   di schermo e le salva in JPEG - e poi dice al pannello di andarsele a
   prendere. Non le manda dentro l'evento: una foto da duecento kilobyte
   scritta in base64 sul bus degli eventi e' esattamente la raffica che il
   collegamento SDIO verso il C6 regge peggio. Cosi' invece passa da HTTP, a
   pezzi, come tutto il resto del traffico grosso.

   La cartella e' quella che lo slideshow guarda gia'. */
#define CARTELLA_FOTO "/sdcard/foto"

struct LavoroFoto { std::string url, nome; };

/* Un nome di file che non possa uscire dalla cartella.

   Il nome arriva da fuori, e un nome come "../../sdcard/standby.json"
   scriverebbe dove non deve. Si tengono lettere, cifre, punto, trattino e
   underscore; tutto il resto diventa underscore. */
static std::string nome_pulito(const char *n)
{
    std::string out;
    for (const char *c = n; *c && out.size() < 48; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_';
        out += ok ? *c : '_';
    }
    while (!out.empty() && out[0] == '.') out.erase(0, 1);   // niente file nascosti
    return out;
}

static void foto_task(void *arg)
{
    LavoroFoto *l = (LavoroFoto *)arg;
    mkdir(CARTELLA_FOTO, 0777);
    std::string dest = std::string(CARTELLA_FOTO) + "/" + l->nome;
    bool ok = web_image_scarica_file(l->url.c_str(), dest.c_str());
    ESP_LOGI(TAG, "foto %s: %s", l->nome.c_str(), ok ? "salvata" : "NON salvata");
    delete l;
    vTaskDelete(NULL);
}

static void on_command(const cJSON *data, void *ctx)
{
    (void)ctx;
    if (!cJSON_IsObject(data)) return;
    const cJSON *who = cJSON_GetObjectItem(data, "id");
    char id[16];
    ha_plugin_id(id, sizeof(id));
    /* Sul bus di HA l'evento lo vedono tutti i pannelli: ognuno prende solo
       quello che porta il suo identificativo. */
    if (!cJSON_IsString(who) || strcmp(who->valuestring, id) != 0) return;

    const cJSON *c = cJSON_GetObjectItem(data, "comando");
    if (!cJSON_IsString(c)) return;
    const char *cmd = c->valuestring;
    ESP_LOGI(TAG, "comando da Home Assistant: %s", cmd);

    if (!strcmp(cmd, "schermo")) {
        idle_manager_set_screen(cJSON_IsTrue(cJSON_GetObjectItem(data, "acceso")));
    } else if (!strcmp(cmd, "luminosita")) {
        const cJSON *v = cJSON_GetObjectItem(data, "valore");
        if (cJSON_IsNumber(v)) idle_manager_set_brightness(v->valueint);
    } else if (!strcmp(cmd, "dashboard")) {
        const cJSON *p = cJSON_GetObjectItem(data, "percorso");
        const cJSON *v = cJSON_GetObjectItem(data, "vista");
        if (cJSON_IsString(p)) {
            ha_config_save_dash(p->valuestring, cJSON_IsNumber(v) ? v->valueint : 0);
            xTaskCreate(reconnect_task, "ha_recfg", 6144, NULL, 4, NULL);
        }
    } else if (!strcmp(cmd, "avviso")) {
        /* Scritto sempre, letto solo se lo si chiede: un messaggio che passa
           solo dalla voce lo perde chi non e' nella stanza, e chi non ha
           l'altoparlante collegato non lo riceve proprio. */
        const cJSON *m = cJSON_GetObjectItem(data, "messaggio");
        if (cJSON_IsString(m)) {
            avviso_ui_mostra(m->valuestring);
            if (cJSON_IsTrue(cJSON_GetObjectItem(data, "voce")))
                tts_player_say(m->valuestring);
        }
    } else if (!strcmp(cmd, "slideshow")) {
        apply_slideshow(cJSON_GetObjectItem(data, "config"));
        if (!s_owns) owns_save(true);
    } else if (!strcmp(cmd, "esporta")) {
        const cJSON *r = cJSON_GetObjectItem(data, "richiesta");
        avvia_backup(false, !cJSON_IsFalse(cJSON_GetObjectItem(data, "cifrato")),
                     cJSON_IsString(r) ? r->valuestring : "");
    } else if (!strcmp(cmd, "chiave")) {
        const cJSON *r = cJSON_GetObjectItem(data, "richiesta");
        avvia_backup(true, true, cJSON_IsString(r) ? r->valuestring : "");
    } else if (!strcmp(cmd, "importa")) {
        const cJSON *f = cJSON_GetObjectItem(data, "file");
        const cJSON *k = cJSON_GetObjectItem(data, "chiave");
        if (cJSON_IsString(f)) {
            char msg[160];
            bool ok = backup_importa(f->valuestring,
                                     cJSON_IsString(k) ? k->valuestring : NULL,
                                     msg, sizeof(msg));
            ESP_LOGW(TAG, "ripristino da Home Assistant: %s", msg);
            avviso_ui_mostra(ok ? "Configurazione ripristinata. Riavvio..."
                                : "Ripristino non riuscito.");
            if (ok) {
                /* Un pannello che continua a girare con meta' della vecchia
                   configurazione in RAM e meta' della nuova in memoria non e'
                   ne' l'una ne' l'altra cosa. */
                vTaskDelay(pdMS_TO_TICKS(2500));
                esp_restart();
            }
        }
    } else if (!strcmp(cmd, "foto")) {
        const cJSON *u = cJSON_GetObjectItem(data, "url");
        const cJSON *n = cJSON_GetObjectItem(data, "nome");
        if (cJSON_IsString(u) && cJSON_IsString(n)) {
            LavoroFoto *l = new LavoroFoto();
            l->url = u->valuestring;
            l->nome = nome_pulito(n->valuestring);
            if (l->nome.empty()) l->nome = "foto.jpg";
            if (xTaskCreate(foto_task, "ha_foto", 8192, l, 3, NULL) != pdPASS) delete l;
        }
    } else if (!strcmp(cmd, "elimina_foto")) {
        const cJSON *n = cJSON_GetObjectItem(data, "nome");
        if (cJSON_IsTrue(cJSON_GetObjectItem(data, "tutte"))) {
            DIR *d = opendir(CARTELLA_FOTO);
            int q = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    std::string p = std::string(CARTELLA_FOTO) + "/" + e->d_name;
                    if (unlink(p.c_str()) == 0) q++;
                }
                closedir(d);
            }
            ESP_LOGW(TAG, "foto cancellate: %d (lo slideshow le rilegge al prossimo giro)", q);
        } else if (cJSON_IsString(n)) {
            std::string nm = nome_pulito(n->valuestring);
            std::string p = std::string(CARTELLA_FOTO) + "/" + nm;
            ESP_LOGI(TAG, "foto %s: %s", nm.c_str(),
                     unlink(p.c_str()) == 0 ? "cancellata" : "non c'era");
        }
    } else if (!strcmp(cmd, "stato")) {
        /* niente da fare: lo stato lo mandiamo qui sotto in ogni caso */
    } else {
        ESP_LOGW(TAG, "comando sconosciuto: %s", cmd);
        return;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

// -------------------------------------------------------------------- avvio

static void state_task(void *arg)
{
    (void)arg;
    while (true) {
        /* Si sveglia da sola ogni mezzo minuto e ogni volta che qualcosa
           cambia, cosi' in HA lo stato non resta indietro dopo un comando. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATE_EVERY_S * 1000));
        vTaskDelay(pdMS_TO_TICKS(300));      // lascia finire il comando appena dato
        if (!s_paired) {
            /* Riprova a presentarsi: l'integrazione puo' essere stata
               installata dopo che il pannello si era gia' collegato, e
               aspettare la prossima riconnessione vorrebbe dire ore. */
            if (ha_ws_connected()) announce();
        } else {
            push_state();
        }
    }
}

static void on_ready(void)
{
    announce();
}

void ha_plugin_start(void)
{
    owns_load();
    ha_ws_subscribe_event(EVENT_NAME, on_command, NULL);
    ha_ws_on_ready(on_ready);
    if (!s_task) xTaskCreate(state_task, "ha_plugin", 4096, NULL, 3, &s_task);
    ESP_LOGI(TAG, "in ascolto di Home Assistant (slideshow: %s)",
             s_owns ? "lo fa HA" : "lo fa il pannello");
}

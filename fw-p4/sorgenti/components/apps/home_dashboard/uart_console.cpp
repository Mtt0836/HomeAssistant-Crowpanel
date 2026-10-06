#include "uart_console.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <cstdarg>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include <math.h>
#include "energy_model.h"
#include "backup.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "ha_ws.h"
#include "ha_config.h"
#include "lovelace_ui.h"
#include "hosted_recovery.h"
#include "web_auth.h"
#include "web_cert.h"
#include "ha_token.h"
#include "setting/setup_wizard.h"
#include "factory_reset.h"
#include "ha_registry.h"
#include "ha_plugin.h"
#include "setting/pin_lock.h"
#include "idle_manager.h"
#include "standby_show.h"
#include "batteria.h"
#include "ota_update.h"
#include "esplora.h"
#include "ram_monitor.h"
#include "debug_config.h"
#include "setting/settings_extra.h"
#include "esp_system.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_misc.h"
#include "esp_http_client.h"

static const char *TAG = "console";

/* Uscita dei comandi: va sempre sulla seriale e, quando un comando e' lanciato
   dalla sezione Debug delle Impostazioni, viene anche copiata in un buffer da
   mostrare a schermo. Tutti i printf dei comandi passano di qui. */
static SemaphoreHandle_t s_exec_mtx = nullptr;
static char  *s_cap     = nullptr;
static size_t s_cap_sz  = 0;
static size_t s_cap_len = 0;

static int con_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int con_out(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    fputs(tmp, stdout);
    if (s_cap && s_cap_len + 1 < s_cap_sz) {
        size_t k = strlcpy(s_cap + s_cap_len, tmp, s_cap_sz - s_cap_len);
        s_cap_len += (k < s_cap_sz - s_cap_len) ? k : (s_cap_sz - s_cap_len - 1);
    }
    return n;
}
#define printf con_out

static ESP_Brookesia_Phone *s_phone   = nullptr;
static int                  s_ha_id   = -1;

/* ---- apertura dell'app -------------------------------------------------
   startApp() e' privato nel manager di Brookesia; la via pubblica e' l'evento
   applicativo, lo stesso che il launcher invia quando tocchi un'icona.      */
static void cmd_open(void)
{
    if (s_phone == nullptr || s_ha_id < 0) {
        printf("OPEN_ERR app non registrata\n");
        return;
    }
    ESP_Brookesia_CoreAppEventData_t ev = {};
    ev.id   = s_ha_id;
    ev.type = ESP_BROOKESIA_CORE_APP_EVENT_TYPE_START;
    ev.data = nullptr;

    bsp_display_lock(0);
    bool ok = s_phone->sendAppEvent(&ev);
    bsp_display_unlock();
    printf(ok ? "OPEN_OK id=%d\n" : "OPEN_ERR sendAppEvent fallita\n", s_ha_id);
}

/* ---- screenshot --------------------------------------------------------
   Lo schermo e' 1024x600 RGB565: 1,2 MB, circa due minuti a 115200 baud.
   Di default sottocampiono 1:4 (256x150, ~9 s). "shot 1" manda il originale. */
static void cmd_shot(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    bsp_display_lock(0);
    /* Standby, configurazione guidata e tastierino del PIN vivono su
       lv_layer_top(), che lo snapshot dello schermo non include: quando c'e'
       una di quelle finestre fotografo quel livello. Non basta guardare se
       lv_layer_top() ha figli: Brookesia ci tiene sempre la barra di stato e
       quella dei gesti, e il resto verrebbe fuori nero. */
    bool overlay = standby_show_active() || setup_wizard_active() || pin_lock_dialog_active();
    lv_obj_t *target = overlay ? lv_layer_top() : lv_scr_act();
    lv_img_dsc_t *snap = (lv_img_dsc_t *)lv_snapshot_take(target, LV_IMG_CF_TRUE_COLOR);
    bsp_display_unlock();

    if (snap == nullptr) {
        printf("SHOT_ERR lv_snapshot_take ha restituito null\n");
        return;
    }

    const int w = snap->header.w, h = snap->header.h;
    const int ow = w / scale, oh = h / scale;
    const uint16_t *src = (const uint16_t *)snap->data;

    /* Durante la trasmissione il log resta muto: una riga di log in mezzo al
       base64 sfaserebbe l'immagine. */
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("<<<SHOT %d %d>>>\n", ow, oh);

    uint8_t raw[48];
    unsigned char b64[80];
    size_t nraw = 0, olen = 0;
    for (int y = 0; y < oh; y++) {
        /* La trasmissione dura decine di secondi: senza cedere la CPU il task
           watchdog scatta e i suoi messaggi finiscono in mezzo all'immagine. */
        if ((y & 3) == 0) vTaskDelay(1);
        const uint16_t *row = src + (size_t)(y * scale) * w;
        for (int x = 0; x < ow; x++) {
            uint16_t p = row[x * scale];
            raw[nraw++] = (uint8_t)(p & 0xFF);
            raw[nraw++] = (uint8_t)(p >> 8);
            if (nraw == sizeof(raw)) {
                mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, nraw);
                b64[olen] = 0;
                printf("%s\n", (const char *)b64);
                nraw = 0;
            }
        }
    }
    if (nraw > 0) {
        mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, nraw);
        b64[olen] = 0;
        printf("%s\n", (const char *)b64);
    }
    printf("<<<END>>>\n");
    debug_config_set(debug_config_get());      // ripristina il livello di log scelto

    lv_snapshot_free(snap);
}

static void cmd_info(void)
{
    char dash[HA_DASH_MAX]; int view = 0;
    ha_config_load_dash(dash, sizeof(dash), &view);
    printf("INFO uptime=%llds heap_int=%u heap_psram=%u ha_ws=%s recuperi_sdio=%u dash=%s/%d\n",
           (long long)(esp_timer_get_time() / 1000000),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           ha_ws_connected() ? "connesso" : "disconnesso",
           hosted_recovery_count(), dash, view);

    char fp[160];
    web_cert_fingerprint(fp, sizeof(fp));
    printf("WEB accesso=%s ospite=%s certificato=%s\n",
           web_auth_configured() ? "protetto" : "da_configurare",
           web_auth_guest_enabled() ? "attivo" : "no",
           fp[0] ? fp : "assente");
    char url[HA_URL_MAX] = {0};
    ha_config_load_url(url, sizeof(url));
    printf("HA indirizzo=%s\n", url[0] ? url : "non impostato");
    printf("HA abbinamento=%s integrazione=%s slideshow=%s\n",
           ha_token_have_refresh() ? "si" : "no (token incollato)",
           ha_plugin_paired() ? "installata" : "assente",
           ha_plugin_owns_slideshow() ? "lo fa HA" : "lo fa il pannello");
    printf("ULTIMO RIAVVIO %s\n", ha_plugin_reset_reason());
}

/* Imposta solo l'indirizzo di Home Assistant: il token resta dov'e', non viene
   letto ne' riscritto. Poi riavvia, cosi' la configurazione si ricarica pulita. */
static void cmd_url(const char *url)
{
    while (*url == ' ') url++;
    if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
        printf("URL_ERR deve iniziare con ws:// o wss://\n");
        return;
    }
    if (!ha_config_save_url(url)) {
        printf("URL_ERR scrittura NVS fallita\n");
        return;
    }
    printf("URL_OK %s - riavvio fra 1s\n", url);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* Sceglie la dashboard di HA da mostrare: "dash <url_path> [vista]".
   Come "url", salva e riavvia cosi' la configurazione si ricarica pulita. */
static void cmd_dash(const char *args)
{
    while (*args == ' ') args++;
    char path[HA_DASH_MAX] = {0};
    int view = 0;
    if (sscanf(args, "%63s %d", path, &view) < 1 || !path[0]) {
        printf("DASH_ERR uso: dash <url_path> [vista]\n");
        return;
    }
    if (!ha_config_save_dash(path, view)) {
        printf("DASH_ERR scrittura NVS fallita\n");
        return;
    }
    printf("DASH_OK %s vista %d - riavvio fra 1s\n", path, view);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* Legge la versione del firmware ESP-Hosted che gira sul C6 e la confronta con
   quella dell'host. La documentazione di ESP-Hosted richiede che slave e host
   siano lo stesso codice: un disallineamento puo' causare timeout SDIO
   sporadici sotto esercizio prolungato. */
static void cmd_fw(void)
{
    esp_hosted_coprocessor_fwver_t v = {};
    esp_err_t err = esp_hosted_get_coprocessor_fwversion(&v);
    if (err != ESP_OK) {
        printf("FW_ERR impossibile leggere la versione del C6: %s\n", esp_err_to_name(err));
        return;
    }
    printf("FW_C6   %u.%u.%u (rev=%d pre=%d build=%d)\n",
           (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1,
           (int)v.revision, (int)v.prerelease, (int)v.build);
    printf("FW_HOST componente esp_hosted lato P4: vedi idf_component.yml\n");
}

/* Elenca una cartella (SD o SPIFFS): serve a capire perche' lo slideshow non
   trova le foto (cartella diversa, estensione diversa, SD non montata). */
static void cmd_ls(const char *path)
{
    while (*path == ' ') path++;
    DIR *d = opendir(path);
    if (!d) {
        printf("LS_ERR %s non apribile (%s)\n", path, strerror(errno));
        return;
    }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != nullptr) {
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st = {};
        bool dir = e->d_type == DT_DIR;
        if (!dir && stat(full, &st) != 0) st.st_size = 0;
        printf("LS %-40s %s %lu\n", e->d_name, dir ? "<cartella>" : "file", (unsigned long)st.st_size);
        n++;
    }
    closedir(d);
    printf("LS_OK %s: %d elementi\n", path, n);
}

/* Chi occupa la RAM interna a runtime: tutti gli stack dei task stanno li',
   e LVGL tiene il conto dei propri blocchi (main/lvgl_mem.c). */
extern "C" void lvgl_mem_stats(size_t *internal_bytes, size_t *psram_bytes);

static void cmd_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(n * sizeof(TaskStatus_t) + 64, MALLOC_CAP_SPIRAM);
    if (!st) return;
    n = uxTaskGetSystemState(st, n, nullptr);
    printf("TASK                 prio core stack_libero_min\n");
    for (UBaseType_t i = 0; i < n; i++) {
        int core = (int)st[i].xCoreID;
        printf("%-20s %4u %4s %8u\n", st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority,
               core > 1 ? "-" : (core ? "1" : "0"), (unsigned)st[i].usStackHighWaterMark);
    }
    heap_caps_free(st);
    size_t li = 0, lp = 0;
    lvgl_mem_stats(&li, &lp);
    printf("TASK totale %u task\n", (unsigned)n);
    printf("RAM interna: libera %u, minima %u, blocco max %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("LVGL: %u byte in RAM interna, %u in PSRAM\n", (unsigned)li, (unsigned)lp);
}

/* Stampa in JSON la vista Lovelace in uso (solo lettura, per sviluppo). */
static void cmd_llcfg(void)
{
    bsp_display_lock(0);
    char *j = ll_view_json();
    bsp_display_unlock();
    if (!j) { printf("LLCFG_ERR nessuna vista caricata\n"); return; }
    printf("<<<LLCFG>>>\n%s\n<<<END>>>\n", j);
    free(j);
}

/* Preferenze del pannello Energia di HA: quali sensori fanno da rete,
   fotovoltaico, batteria. Contengono solo nomi di statistiche. */
static void enprefs_cb(bool ok, cJSON *result, const char *error, void *ctx)
{
    if (!ok) { printf("ENPREFS_ERR %s\n", error ? error : "?"); return; }
    char *j = cJSON_PrintUnformatted(result);
    printf("<<<ENPREFS>>>\n%s\n<<<END>>>\n", j ? j : "null");
    free(j);
}

/* Lo stato del pannello Energia: cosa ha trovato nelle preferenze, che numeri
   ne ha ricavato e per quale finestra. Serve a capire perche' le card mostrano
   quello che mostrano - sopratutto a chi ha una configurazione Energia diversa
   dalla nostra, che e' poi quasi chiunque. */
static void cmd_energia(const char *args)
{
    if (args && *args) {
        EnergyPeriodo p = EN_OGGI;
        if      (!strncmp(args, "sett", 4)) p = EN_SETTIMANA;
        else if (!strncmp(args, "mese", 4)) p = EN_MESE;
        else if (!strncmp(args, "anno", 4)) p = EN_ANNO;
        energy_model_set_periodo(p);
    }
    energy_model_forza();

    const EnergyModel *m = energy_model_get();
    printf("ENERGIA periodo=%s prefs=%s dati=%s%s%s\n",
           energy_periodo_nome(m->periodo),
           m->prefs_lette ? "lette" : "no",
           m->dati_pronti ? "pronti" : "no",
           m->errore.empty() ? "" : " errore=", m->errore.c_str());
    printf("ENERGIA configurato: rete=%d solare=%d batteria=%d gas=%d acqua=%d dispositivi=%u\n",
           m->c_e_rete, m->c_e_solare, m->c_e_batteria, m->c_e_gas, m->c_e_acqua,
           (unsigned)m->dispositivi.size());
    printf("ENERGIA rete +%.3f -%.3f  sole %.3f  batteria +%.3f -%.3f  casa %.3f\n",
           m->rete_presa, m->rete_immessa, m->solare,
           m->batteria_scarica, m->batteria_carica, m->casa);
    printf("ENERGIA autosufficienza=%.0f sole_in_casa=%.0f bilancio_rete=%.0f (negativo = non calcolabile)\n",
           m->autosufficienza, m->solare_usato, m->neutralita);
    for (const EnergyVoce &d : m->dispositivi)
        printf("ENERGIA dispositivo %-44s %.3f\n", d.id.c_str(), d.totale);
    printf("ENERGIA (se dice \"no\" richiama fra qualche secondo: la richiesta si accoda)\n");
}

/* Prova la catena del salvataggio: esporta, cifra, riapre e confronta, senza
   scrivere niente e senza stampare un solo pezzo di configurazione. */
static void cmd_backup(void)
{
    char r[240];
    backup_autoprova(r, sizeof(r));
    printf("BACKUP %s\n", r);
    char k[BACKUP_CHIAVE_MAX];
    /* Della chiave si mostra solo l'inizio: basta a vedere che c'e' ed e'
       sempre la stessa, non basta a nessuno per usarla. */
    if (backup_chiave_testo(k, sizeof(k)))
        printf("BACKUP chiave presente (comincia per %.4s), gia' scaricata: %s\n",
               k, backup_chiave_gia_presa() ? "si" : "no");
}

/* "mostra <percorso>": stampa un file sulla seriale.

   Serve a portarsi via dal pannello un file senza passare dalla pagina web,
   che vuole la password, e senza estrarre la scheda SD, che a pannello acceso
   non si fa. Tipicamente: il registro della batteria da analizzare sul
   computer, o la configurazione dello slideshow prima di un intervento che
   rifa' lo SPIFFS.

   I percorsi ammessi sono gli stessi dell'esploratore - niente ".." e solo
   /sdcard e /spiffs - perche' il controllo deve stare in un posto solo.

   A fette e con un tetto: un file enorme riempirebbe il terminale per minuti
   senza che nessuno possa fermarlo. */
static void cmd_mostra(const char *percorso)
{
    while (*percorso == ' ') percorso++;
    if (!esplora_permesso(percorso)) {
        printf("MOSTRA_ERR percorso non ammesso: '%s'\n", percorso);
        return;
    }
    FILE *f = fopen(percorso, "rb");
    if (!f) {
        printf("MOSTRA_ERR %s non si apre (%s)\n", percorso, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long dim = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Le marche di inizio e fine servono a chi cattura dal PC per ritagliare
       il file dal resto del log, che nel frattempo continua a uscire. */
    printf("MOSTRA_INIZIO %s %ld\n", percorso, dim);
    char *buf = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (buf) {
        size_t n;
        while ((n = fread(buf, 1, 1024, f)) > 0) {
            fwrite(buf, 1, n, stdout);
            /* Una pausa ogni fetta: la seriale ha il suo buffer e il log degli
               altri task continua a scriverci dentro. Senza, un file grosso
               perde pezzi proprio mentre lo si sta salvando. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        heap_caps_free(buf);
    }
    fclose(f);
    printf("\nMOSTRA_FINE %s\n", percorso);
}

/* "scrivi <percorso> <base64>"    crea o sovrascrive
   "aggiungi <percorso> <base64>"  accoda

   Il gemello di "mostra": rimettere nel pannello un file che si ha sul
   computer, senza passare dalla pagina web - che vuole la password - e senza
   estrarre la scheda SD. Serve soprattutto dopo un intervento che rifa' lo
   SPIFFS, per riportare la configurazione dov'era.

   In base64 perche' la riga della console e' testo: un JSON pieno di virgolette
   passerebbe male e un file binario non passerebbe affatto. E il base64 si
   spezza dove si vuole, quindi un file piu' lungo di una riga si manda in piu'
   pezzi con "aggiungi" - purche' ogni pezzo sia multiplo di quattro
   caratteri, altrimenti non si decodifica da solo. */
static void cmd_scrivi(const char *arg, bool accoda)
{
    while (*arg == ' ') arg++;
    const char *sp = strchr(arg, ' ');
    char percorso[200];
    if (!sp || (size_t)(sp - arg) >= sizeof(percorso)) {
        printf("SCRIVI_ERR serve: %s <percorso> <base64>\n", accoda ? "aggiungi" : "scrivi");
        return;
    }
    size_t lp = sp - arg;
    memcpy(percorso, arg, lp);
    percorso[lp] = 0;
    const char *testo = sp + 1;
    while (*testo == ' ') testo++;

    if (!esplora_permesso(percorso)) {
        printf("SCRIVI_ERR percorso non ammesso: '%s'\n", percorso);
        return;
    }

    size_t serve = 0, fatti = 0, n = strlen(testo);
    mbedtls_base64_decode(NULL, 0, &serve, (const unsigned char *)testo, n);
    if (!serve) { printf("SCRIVI_ERR base64 non valido\n"); return; }
    unsigned char *dati = (unsigned char *)heap_caps_malloc(serve, MALLOC_CAP_SPIRAM);
    if (!dati) { printf("SCRIVI_ERR niente memoria\n"); return; }
    if (mbedtls_base64_decode(dati, serve, &fatti, (const unsigned char *)testo, n) != 0) {
        heap_caps_free(dati);
        printf("SCRIVI_ERR base64 non valido\n");
        return;
    }

    FILE *f = fopen(percorso, accoda ? "ab" : "wb");
    if (!f) {
        heap_caps_free(dati);
        printf("SCRIVI_ERR %s non si apre (%s)\n", percorso, strerror(errno));
        return;
    }
    size_t scritti = fwrite(dati, 1, fatti, f);
    long dim = ftell(f);
    fclose(f);
    heap_caps_free(dati);
    if (scritti != fatti)
        printf("SCRIVI_ERR scritti %u byte su %u\n", (unsigned)scritti, (unsigned)fatti);
    else
        printf("SCRIVI_OK %s: %u byte %s, il file ora e' %ld\n", percorso,
               (unsigned)fatti, accoda ? "accodati" : "scritti", dim);
}

/* "ota"                        a che punto e' l'aggiornamento
   "ota <url> <sha256> [byte]"  comincia un aggiornamento

   Esiste per provare l'OTA senza passare da Home Assistant: si serve il .bin
   dal computer e si guarda tutta la catena - scaricamento, impronta, scrittura,
   riavvio, conferma - prima di metterci in mezzo anche il plugin. La stessa
   strada che abbiamo seguito per l'OTA del C6. */
static void cmd_ota(const char *arg)
{
    if (!arg[0]) {
        ota_info_t i;
        ota_update_stato(&i);
        char v[96];
        ota_update_versione(v, sizeof(v));
        printf("OTA in esecuzione: %s%s\n", v, i.in_prova ? "  [IN PROVA]" : "");
        printf("OTA stato=%s %d%% (%lu/%lu byte) %s\n", ota_parole(i.stato), i.pct,
               (unsigned long)i.byte_fatti, (unsigned long)i.byte_attesi, i.messaggio);
        return;
    }
    char url[400] = "", sha[80] = "";
    unsigned byte = 0;
    int n = sscanf(arg, "%399s %79s %u", url, sha, &byte);
    if (n < 2) {
        printf("OTA_ERR serve: ota <url> <sha256 di 64 caratteri> [byte]\n");
        return;
    }
    if (ota_update_avvia(url, sha, byte)) printf("OTA_OK avviato\n");
    else                                  printf("OTA_ERR non parte (vedi il log)\n");
}

/* "batteria"                      stato, curva, taratura, registro
   "batteria azzera"               dimentica la curva imparata
   "batteria ma <spento> <acceso>" i consumi misurati a pinza, in mA */
static void cmd_batteria(const char *arg)
{
    if (!strcmp(arg, "azzera")) {
        batteria_azzera_curva();
        printf("BATTERIA_OK curva dimenticata\n");
        return;
    }
    if (!strncmp(arg, "ma", 2)) {
        unsigned spento = 0, acceso = 0;
        if (sscanf(arg + 2, "%u %u", &spento, &acceso) != 2) {
            printf("BATTERIA_ERR serve: batteria ma <mA schermo spento> <mA schermo acceso>\n");
            return;
        }
        if (spento > 5000 || acceso > 5000) {
            printf("BATTERIA_ERR valori fuori scala (massimo 5000 mA)\n");
            return;
        }
        batteria_imposta_correnti((uint16_t)spento, (uint16_t)acceso);
        printf("BATTERIA_OK consumi: %u mA spento, %u mA acceso\n", spento, acceso);
        return;
    }
    if (arg[0]) {
        printf("BATTERIA_ERR non capisco '%s' (batteria | batteria azzera | batteria ma A B)\n", arg);
        return;
    }
    /* In PSRAM: la curva a 21 punti piu' il resto non sta comoda in 512 byte
       di stack, e questa console gira su un task piccolo. */
    char *buf = (char *)heap_caps_malloc(1536, MALLOC_CAP_SPIRAM);
    if (!buf) { printf("BATTERIA_ERR niente memoria\n"); return; }
    batteria_diagnostica(buf, 1536);
    fputs(buf, stdout);
    heap_caps_free(buf);
}

static void cmd_enprefs(void)
{
    if (ha_ws_request("\"type\":\"energy/get_prefs\"", enprefs_cb, nullptr) < 0)
        printf("ENPREFS_ERR non connesso a HA\n");
}

/* Misura la tenuta del collegamento SDIO sotto un download grande: scarica
   il file senza salvarlo e riporta quanti byte passano prima di un guasto.
   Non tocca il C6 (a parte il traffico). */
static void cmd_dltest(const char *url)
{
    while (*url == ' ') url++;
    const unsigned rec0 = hosted_recovery_count();
    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (cl == NULL || esp_http_client_open(cl, 0) != ESP_OK) {
        printf("DLTEST_ERR apertura fallita\n");
        if (cl) esp_http_client_cleanup(cl);
        return;
    }
    int64_t total = esp_http_client_fetch_headers(cl);
    static char buf[4096];
    int64_t got = 0;
    int r;
    const int64_t t0 = esp_timer_get_time();
    while ((r = esp_http_client_read(cl, buf, sizeof(buf))) > 0) got += r;
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    printf("DLTEST %s %lld/%lld byte in %lld ms (%lld KB/s), recuperi durante: %u\n",
           got == total ? "OK" : "INTERROTTO", (long long)got, (long long)total, (long long)ms,
           ms > 0 ? (long long)(got / ms) : 0LL, hosted_recovery_count() - rec0);
}

/* Gli header di risposta arrivano solo come eventi: da "Content-Range:
   bytes a-b/TOTALE" ricavo la lunghezza dell'immagine. */
static esp_err_t range_header_cb(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value &&
        strcasecmp(ev->header_key, "Content-Range") == 0) {
        const char *slash = strchr(ev->header_value, '/');
        if (slash && ev->user_data) *(int64_t *)ev->user_data = atoll(slash + 1);
    }
    return ESP_OK;
}

/* Trasferisce un'immagine firmware al C6 attraverso il collegamento SDIO gia'
   attivo. Lo slave ha due slot (ota_0/ota_1) con otadata: la scrittura va in
   quello inattivo e la commutazione avviene solo a fine riuscita, quindi
   un'interruzione lascia intatto il firmware funzionante. */
/* C'e' davvero un microfono su questa scheda?

   Il BSP dichiara BSP_CAPS_AUDIO_MIC e un microfono PDM su GPIO 24 e 26, ma
   quel BSP e' quello della scheda di sviluppo di Espressif adattato da
   Elecrow: una capacita' dichiarata puo' essere ereditata dall'originale
   senza che il componente sia montato qui. Dichiarato e presente non sono la
   stessa cosa, e l'unico modo di saperlo e' ascoltare.

   La prova: si registra un secondo e si guarda cosa e' arrivato. Silenzio
   digitale perfetto - tutti zero, o un valore fisso - vuol dire che nessuno
   sta parlando al convertitore: il piedino e' scollegato. Rumore di fondo,
   anche pochissimo, vuol dire che il microfono c'e'. Se parli mentre gira, la
   differenza fra i due casi diventa lampante. */
static void cmd_mic(void)
{
    esp_codec_dev_handle_t h = bsp_audio_codec_microphone_init();
    if (!h) { printf("MIC_ERR il BSP non apre il microfono\n"); return; }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = 1;
    fs.channel_mask    = 1;
    fs.sample_rate     = 16000;
    int e = esp_codec_dev_open(h, &fs);
    if (e != 0) { printf("MIC_ERR apertura fallita (%d)\n", e); return; }
    esp_codec_dev_set_in_gain(h, 30.0f);

    /* A fette: il buffer deve stare in RAM interna perche' ci scrive il DMA,
       e qui dentro non ce n'e' da sprecare. 2048 byte per giro, 16 giri: un
       secondo tondo a 16 kHz. */
    const int FETTA = 2048;
    int16_t *buf = (int16_t *)heap_caps_malloc(FETTA, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) { esp_codec_dev_close(h); printf("MIC_ERR niente memoria\n"); return; }

    int32_t minimo = 32767, massimo = -32768;
    int64_t somma_quadrati = 0;
    int campioni = 0, diversi = 0;
    int16_t primo = 0;

    printf("MIC registro un secondo: parla, fischia, batti le mani\n");
    for (int giro = 0; giro < 16; giro++) {
        if (esp_codec_dev_read(h, buf, FETTA) != 0) break;
        int n = FETTA / 2;
        for (int i = 0; i < n; i++) {
            int16_t v = buf[i];
            if (!campioni && !i) primo = v;
            if (v != primo) diversi++;
            if (v < minimo)  minimo = v;
            if (v > massimo) massimo = v;
            somma_quadrati += (int32_t)v * v;
        }
        campioni += n;
    }
    esp_codec_dev_close(h);

    if (!campioni) { free(buf); printf("MIC_ERR non e' arrivato niente\n"); return; }
    int rms = (int)sqrt((double)(somma_quadrati / campioni));
    free(buf);

    printf("MIC campioni=%d min=%d max=%d rms=%d diversi_dal_primo=%d\n",
           campioni, (int)minimo, (int)massimo, rms, diversi);
    if (massimo == minimo)
        printf("MIC ESITO valore fisso (%d): nessun microfono collegato a quei piedini\n",
               (int)minimo);
    else if (rms < 20 && (massimo - minimo) < 64)
        printf("MIC ESITO quasi piatto: probabilmente non c'e' microfono, "
               "ma riprova parlandogli vicino prima di darlo per morto\n");
    else
        printf("MIC ESITO il microfono c'e' e sente\n");
}

static void cmd_slaveota(const char *args)
{
    /* "slaveota <url> [prova]": con "prova" l'SDIO si spegne nell'istante
       dell'attivazione, cosi' il C6 riparte sull'immagine nuova senza mai
       ricevere il P4 e il suo ripristino automatico si puo' verificare. */
    while (*args == ' ') args++;
    char url[160] = {0};
    char opt[16] = {0};
    sscanf(args, "%159s %15s", url, opt);
    const bool prova = strcmp(opt, "prova") == 0;
    if (strncmp(url, "http://", 7) != 0) {
        printf("OTA_ERR serve un URL che inizia con http://\n");
        return;
    }

    /* Fase 1: l'immagine si scarica per intero nella PSRAM del P4.
       Il collegamento SDIO verso il C6 cade quando riceve una raffica di dati
       mentre il P4 gli scrive (vedi crowpanel-v12-vincoli, punto 6): scaricare e
       scrivere insieme lo esponeva proprio a quella combinazione. Separando le
       fasi, un guasto durante il download costa solo un nuovo tentativo e il
       C6 non viene toccato. */
    uint8_t *img = nullptr;
    int64_t total = 0;
    esp_err_t err = ESP_FAIL;
    const int64_t PIECE = 16 * 1024;
    int64_t got = 0;
    int fails = 0;
    /* Download a pezzi con "Range": anche il solo download da 1,2 MB fa cadere
       il collegamento SDIO dopo qualche centinaio di KB, quindi ogni caduta
       deve costare solo il pezzo in corso. Il primo pezzo rivela la lunghezza
       totale (Content-Range); il server e' c6_serve/rangeserver.py. */
    while (total == 0 || got < total) {
        if (fails >= 60) {
            printf("OTA_ERR troppi errori di download: il C6 non e' stato toccato\n");
            if (img) heap_caps_free(img);
            return;
        }
        if (fails > 0) vTaskDelay(pdMS_TO_TICKS(6000));   // lascia finire il recupero SDIO

        int64_t to = got + PIECE - 1;
        if (total > 0 && to > total - 1) to = total - 1;
        char range[48];
        snprintf(range, sizeof(range), "bytes=%lld-%lld", (long long)got, (long long)to);

        int64_t cr_total = 0;
        esp_http_client_config_t cfg = {};
        cfg.url           = url;
        cfg.timeout_ms    = 15000;
        cfg.event_handler = range_header_cb;
        cfg.user_data     = &cr_total;
        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        if (cl == NULL) {
            printf("OTA_ERR init client HTTP fallita\n");
            if (img) heap_caps_free(img);
            return;
        }
        esp_http_client_set_header(cl, "Range", range);
        err = esp_http_client_open(cl, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(cl);
            fails++;
            printf("OTA_WARN pezzo a %lld: apertura fallita (%s), ripeto\n", (long long)got, esp_err_to_name(err));
            continue;
        }
        int64_t clen = esp_http_client_fetch_headers(cl);
        int status = esp_http_client_get_status_code(cl);
        if (status <= 0 || clen <= 0) {                 // connessione caduta: ripeto
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            fails++;
            printf("OTA_WARN pezzo a %lld: risposta assente, ripeto\n", (long long)got);
            continue;
        }
        if (status != 206) {
            printf("OTA_ERR il server non gestisce Range (status=%d): usa rangeserver.py\n", status);
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            if (img) heap_caps_free(img);
            return;
        }
        if (total == 0) {
            total = cr_total;
            if (total <= 0 || total > 1920 * 1024) {
                printf("OTA_ERR lunghezza immagine non valida (%lld)\n", (long long)total);
                esp_http_client_close(cl);
                esp_http_client_cleanup(cl);
                return;
            }
            img = (uint8_t *)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (img == nullptr) {
                printf("OTA_ERR PSRAM insufficiente per %lld byte\n", (long long)total);
                esp_http_client_close(cl);
                esp_http_client_cleanup(cl);
                return;
            }
            printf("OTA_INFO immagine di %lld byte, download a pezzi da %lld KB\n",
                   (long long)total, (long long)(PIECE / 1024));
        }
        int64_t want = LV_MIN(clen, total - got);
        int64_t n = 0;
        int r;
        while (n < want && (r = esp_http_client_read(cl, (char *)img + got + n, (int)LV_MIN(4096, want - n))) > 0) {
            n += r;
        }
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
        if (n != want) {
            fails++;
            printf("OTA_WARN pezzo a %lld interrotto (%lld/%lld): ripeto\n", (long long)got, (long long)n, (long long)want);
            continue;
        }
        got += n;
        printf("OTA_DL %lld%%\n", (long long)(got * 100 / total));
    }
    printf("OTA_INFO immagine di %lld byte scaricata (%d pezzi ripetuti), scrittura sul C6\n",
           (long long)total, fails);

    /* Fase 2: scrittura sul C6, senza download in corso. */
    const unsigned rec_before = hosted_recovery_count();
    if ((err = esp_hosted_slave_ota_begin()) != ESP_OK) {
        printf("OTA_ERR begin: %s\n", esp_err_to_name(err));
        heap_caps_free(img);
        return;
    }
    const int CHUNK = 1400;
    int64_t done = 0;
    int last_pct = -1;
    bool failed = false;
    while (done < total) {
        uint32_t n = (uint32_t)LV_MIN(CHUNK, total - done);
        if ((err = esp_hosted_slave_ota_write(img + done, n)) != ESP_OK ||
            hosted_recovery_count() != rec_before) {
            printf("OTA_ERR scrittura a %lld byte: %s\n", (long long)done, esp_err_to_name(err));
            failed = true;
            break;
        }
        done += n;
        int pct = (int)((done * 100) / total);
        if (pct != last_pct && (pct % 10) == 0) {
            printf("OTA_PROG %d%%\n", pct);
            last_pct = pct;
        }
    }
    heap_caps_free(img);

    if (failed) {
        /* Niente ota_end: se il trasporto e' caduto il recupero sta ricreando le
           strutture RPC, e chiamarle ora le usava dopo la liberazione (panic
           "Interrupt wdt timeout" del 21/09). Il C6 viene comunque resettato:
           lo slot scritto a meta' non e' attivo e resta innocuo. */
        printf("OTA_ERR scrittura interrotta: lo slot attivo del C6 non e' cambiato, riprova\n");
        return;
    }

    if ((err = esp_hosted_slave_ota_end()) != ESP_OK) {
        printf("OTA_ERR end: %s\n", esp_err_to_name(err));
        return;
    }
    printf("OTA_OK immagine scritta per intero, attivo lo slot nuovo\n");

    err = esp_hosted_slave_ota_activate();
    if (prova) hosted_recovery_suspend();
    printf("OTA_ACT %s - il C6 si riavvia sul firmware nuovo%s\n", esp_err_to_name(err),
           prova ? " (PROVA: SDIO spenta, riavviare il P4 per riaccenderla)" : "");
}

/* Un dito finto, per provare dal PC le card che si comandano.

   Fino alle card della luce e del termostato tutto quello che si vedeva sullo
   schermo si poteva controllare con una fotografia. Quelle invece rispondono
   al trascinamento, e per sapere se il cerchio della luminosita' manda a Home
   Assistant il numero giusto bisogna trascinarlo davvero.

   E' un secondo dispositivo di puntamento registrato accanto al touch vero:
   LVGL ne accetta piu' di uno, e cosi' il tocco finto passa per la stessa
   strada di quello vero (pressione, trascinamento, rilascio) invece di
   saltare direttamente all'evento. Si registra solo la prima volta che serve,
   quindi chi non lo usa non se ne accorge. */
static lv_indev_drv_t s_dito_drv;
static lv_indev_t    *s_dito = NULL;
static volatile int   s_dito_x = 0, s_dito_y = 0;
static volatile bool  s_dito_giu = false;

static void dito_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = (lv_coord_t)s_dito_x;
    data->point.y = (lv_coord_t)s_dito_y;
    data->state = s_dito_giu ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void dito_pronto(void)
{
    if (s_dito) return;
    bsp_display_lock(0);
    lv_indev_drv_init(&s_dito_drv);
    s_dito_drv.type = LV_INDEV_TYPE_POINTER;
    s_dito_drv.read_cb = dito_read;
    s_dito = lv_indev_drv_register(&s_dito_drv);
    bsp_display_unlock();
}

/* "tap x y" oppure "drag x1 y1 x2 y2": il trascinamento passa per una decina
   di posizioni intermedie, perche' un salto solo LVGL lo prenderebbe per un
   tocco secco e il cursore non seguirebbe il dito. */
static void cmd_tap(const char *args, bool trascina)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int n = sscanf(args, "%d %d %d %d", &x1, &y1, &x2, &y2);
    if (n < (trascina ? 4 : 2)) {
        printf("TAP_ERR serve '%s'\n", trascina ? "drag x1 y1 x2 y2" : "tap x y");
        return;
    }
    dito_pronto();
    /* Le coordinate si prendono dalla fotografia, che e' dello schermo attivo:
       se l'app non parte dall'angolo del display (sopra c'e' la barra di
       stato) qui ci va aggiunto lo scostamento, se no il tocco cade piu' in
       alto di dove si e' puntato. */
    lv_area_t a;
    bsp_display_lock(0);
    lv_obj_get_coords(lv_scr_act(), &a);
    bsp_display_unlock();
    x1 += a.x1; y1 += a.y1;
    x2 += a.x1; y2 += a.y1;
    s_dito_x = x1; s_dito_y = y1;
    vTaskDelay(pdMS_TO_TICKS(60));
    s_dito_giu = true;
    vTaskDelay(pdMS_TO_TICKS(120));
    if (trascina) {
        const int passi = 12;
        for (int i = 1; i <= passi; i++) {
            s_dito_x = x1 + (x2 - x1) * i / passi;
            s_dito_y = y1 + (y2 - y1) * i / passi;
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    s_dito_giu = false;
    vTaskDelay(pdMS_TO_TICKS(120));
    if (trascina) printf("DRAG %d,%d -> %d,%d\n", x1, y1, x2, y2);
    else          printf("TAP %d,%d\n", x1, y1);
}

/* Interroga una tantum la memoria libera del C6. Serve a verificare l'ipotesi
   di una risorsa che si consuma a ogni pacchetto: se l'heap scende in modo
   costante fino al timeout SDIO, la causa e' una perdita lato coprocessore. */
static void cmd_cpmem(void)
{
    esp_hosted_config_mem_monitor_t cfg = {};
    cfg.config = ESP_HOSTED_MEMMONITOR_NO_CHANGE;
    esp_hosted_curr_mem_info_t m = {};
    esp_err_t err = esp_hosted_set_mem_monitor(&cfg, &m);
    if (err != ESP_OK) {
        printf("CPMEM_ERR %s\n", esp_err_to_name(err));
        return;
    }
    printf("CPMEM t=%llds heap=%u int_dma_free=%u int_dma_big=%u int_8bit_free=%u int_8bit_big=%u\n",
           (long long)(esp_timer_get_time() / 1000000),
           (unsigned)m.curr_total_heap_size,
           (unsigned)m.curr_internal.cap_dma.free_size,
           (unsigned)m.curr_internal.cap_dma.largest_free_block,
           (unsigned)m.curr_internal.cap_8bit.free_size,
           (unsigned)m.curr_internal.cap_8bit.largest_free_block);
}

static void handle(char *line)
{
    while (*line == ' ') line++;
    if (*line == '\0') return;

    /* Un comando e' attivita' dell'operatore, come un tocco: rinvia lo standby
       (e se e' gia' attivo lo chiude, come farebbe il dito sullo schermo). */
    bsp_display_lock(0);
    lv_disp_trig_activity(NULL);
    bsp_display_unlock();

    if (!strcmp(line, "help")) {
        printf("COMANDI: open | openapp <id> | shot [1-8] | info | fw | cpmem | url <ws://..> | dash <path> [vista] | refresh | ramlog [ora] | ls <path> | tasks | lvgl ram/psram | console on/off | standby | wizard [step N|done|reset] | llcfg | enprefs | energia [oggi|settimana|mese|anno] | batteria [azzera|ma A B] | ota [url sha256] | mostra <file> | scrivi/aggiungi <file> <b64> | backup | sdiooff | entities | scroll [px] | tap x y | drag x1 y1 x2 y2 | mic | factory | reboot | slaveota <http://..> [prova] | help\n");
    } else if (!strcmp(line, "mic")) {
        cmd_mic();
    } else if (!strncmp(line, "setscr ", 7)) {
        bsp_display_lock(0);
        bool ok = settings_extra_show(atoi(line + 7));
        bsp_display_unlock();
        printf("SETSCR %s\n", ok ? "ok" : "Impostazioni non aperte");
    } else if (!strncmp(line, "openapp ", 8)) {
        ESP_Brookesia_CoreAppEventData_t ev = {};
        ev.id = atoi(line + 8);
        ev.type = ESP_BROOKESIA_CORE_APP_EVENT_TYPE_START;
        bsp_display_lock(0);
        bool ok = s_phone && s_phone->sendAppEvent(&ev);
        bsp_display_unlock();
        printf("OPENAPP %d %s\n", ev.id, ok ? "ok" : "fallita");
    } else if (!strcmp(line, "open")) {
        cmd_open();
    } else if (!strncmp(line, "shot", 4)) {
        cmd_shot(line[4] ? atoi(line + 4) : 4);
    } else if (!strncmp(line, "dash ", 5)) {
        cmd_dash(line + 5);
    } else if (!strncmp(line, "url ", 4)) {
        cmd_url(line + 4);
    } else if (!strncmp(line, "slaveota ", 9)) {
        cmd_slaveota(line + 9);
    } else if (!strncmp(line, "ramlog", 6)) {
        if (!strcmp(line + 6, " ora")) ram_monitor_sample_now();
        char *rl = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!rl) return;
        ram_monitor_read(rl, 4096);
        printf("<<<RAMLOG>>>\n%s<<<END>>>\n", rl);
        heap_caps_free(rl);
    } else if (!strcmp(line, "lvgl ram") || !strcmp(line, "lvgl psram")) {
        debug_config_t c = *debug_config_get();
        c.lvgl_psram = !strcmp(line, "lvgl psram");
        debug_config_set(&c);
        printf("LVGL in %s dal prossimo riavvio\n", c.lvgl_psram ? "PSRAM" : "RAM interna");
    } else if (!strncmp(line, "ls ", 3)) {
        cmd_ls(line + 3);
    } else if (!strcmp(line, "tasks")) {
        cmd_tasks();
    } else if (!strcmp(line, "console on") || !strcmp(line, "console off")) {
        debug_config_t c = *debug_config_get();
        c.uart_console = !strcmp(line, "console on");
        debug_config_set(&c);
        printf("CONSOLE %s\n", c.uart_console ? "abilitata" : "disabilitata (resta solo 'console on')");
    } else if (!strcmp(line, "standby")) {
        idle_manager_force_standby();
        printf("STANDBY_OK\n");
    } else if (!strcmp(line, "llcfg")) {
        cmd_llcfg();
    } else if (!strncmp(line, "energia", 7)) {
        cmd_energia(line[7] == ' ' ? line + 8 : "");
    } else if (!strcmp(line, "backup")) {
        cmd_backup();
    } else if (!strncmp(line, "batteria", 8)) {
        cmd_batteria(line[8] == ' ' ? line + 9 : "");
    } else if (!strncmp(line, "scrivi ", 7)) {
        cmd_scrivi(line + 7, false);
    } else if (!strncmp(line, "aggiungi ", 9)) {
        cmd_scrivi(line + 9, true);
    } else if (!strncmp(line, "mostra ", 7)) {
        cmd_mostra(line + 7);
    } else if (!strncmp(line, "ota", 3)) {
        cmd_ota(line[3] == ' ' ? line + 4 : "");
    } else if (!strcmp(line, "enprefs")) {
        cmd_enprefs();
    } else if (!strncmp(line, "dltest ", 7)) {
        cmd_dltest(line + 7);
    } else if (!strcmp(line, "sdiooff")) {
        hosted_recovery_suspend();
        printf("SDIOOFF_OK - riavviare il P4 per riaccenderla\n");
    } else if (!strcmp(line, "reboot")) {
        printf("REBOOT_OK\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(line, "refresh")) {
        ll_charts_refresh();
        printf("REFRESH_OK\n");
    } else if (!strcmp(line, "cpmem")) {
        cmd_cpmem();
    } else if (!strcmp(line, "fw")) {
        cmd_fw();
    } else if (!strncmp(line, "wizard", 6)) {
        const char *a = line[6] ? line + 7 : "";
        bsp_display_lock(0);
        if (!strncmp(a, "step ", 5)) {
            setup_wizard_goto(atoi(a + 5));
            printf("WIZARD passo %d\n", atoi(a + 5));
        } else if (!strcmp(a, "next")) {
            setup_wizard_next();
            printf("WIZARD avanti\n");
        } else if (!strcmp(a, "done")) {
            setup_wizard_close(true);
            printf("WIZARD chiuso e segnato come fatto\n");
        } else if (!strcmp(a, "reset")) {
            setup_wizard_reset();
            printf("WIZARD si ripresentera' al prossimo avvio\n");
        } else {
            setup_wizard_start_with(s_phone, 2);
            printf("WIZARD avviato\n");
        }
        bsp_display_unlock();
    } else if (!strcmp(line, "entities")) {
        /* Lo stesso elenco che la pagina web mostra per scegliere i valori in
           sovrimpressione: qui se ne vede solo la taglia, per controllare che
           Home Assistant risponda e che la memoria regga. */
        char *j = ha_registry_json();
        if (!j) {
            printf("ENTITIES nessuna risposta da Home Assistant\n");
        } else {
            int gruppi = 0, voci = 0;
            for (const char *p = j; (p = strstr(p, "\"voci\"")) != NULL; p++) gruppi++;
            for (const char *p = j; (p = strstr(p, "\"id\":")) != NULL; p++) voci++;
            printf("ENTITIES %d gruppi, %d voci, %u byte\n", gruppi, voci, (unsigned)strlen(j));
            printf("%.400s...\n", j);
            free(j);
        }
    } else if (!strncmp(line, "scroll", 6)) {
        /* Per fotografare dal PC le schermate piu' lunghe dello schermo:
           senza questo si vede solo quello che sta nella prima videata. */
        int dy = line[6] ? atoi(line + 7) : 300;
        bsp_display_lock(0);
        lv_obj_t *scr = lv_scr_act();
        lv_obj_t *box = nullptr;
        /* Non basta il primo figlio scorrevole: le schermate hanno anche la
           barra del titolo, che e' scorrevole ma non ha niente da scorrere.
           Cerco quello che ha davvero del contenuto sotto il bordo. */
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(scr); i++) {
            lv_obj_t *ch = lv_obj_get_child(scr, i);
            if (lv_obj_has_flag(ch, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(ch) > 0) {
                box = ch;
                break;
            }
        }
        /* "bounded": lv_obj_scroll_by da solo va oltre la fine e lascia lo
           schermo vuoto invece di fermarsi all'ultima riga. */
        if (box) lv_obj_scroll_by_bounded(box, 0, -dy, LV_ANIM_OFF);
        bsp_display_unlock();
        printf(box ? "SCROLL %d\n" : "SCROLL nessun contenitore scorrevole\n", dy);
    } else if (!strncmp(line, "tap ", 4)) {
        cmd_tap(line + 4, false);
    } else if (!strncmp(line, "drag ", 5)) {
        cmd_tap(line + 5, true);
    } else if (!strcmp(line, "factory")) {
        /* Parola per esteso: e' un comando che cancella tutto, non voglio che
           parta per un tasto sfiorato o per una riga rimasta nel terminale. */
        printf("FACTORY: cancella tutto. Per farlo davvero: 'factory cancella'\n");
    } else if (!strcmp(line, "factory cancella")) {
        printf("FACTORY avviato: il pannello si riavvia fra poco\n");
        factory_reset_start();
    } else if (!strcmp(line, "info")) {
        cmd_info();
    } else {
        printf("CMD_ERR sconosciuto: '%s' (prova 'help')\n", line);
    }
}

static void console_task(void *arg)
{
    /* Installiamo il driver solo per la ricezione: il log continua a uscire
       dal percorso diretto, quindi non viene dirottato ne' perso. */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    }
    /* Il FIFO di ricezione contiene rumore raccolto durante il boot: senza
       questo flush il primo comando arriva con un byte spurio davanti. */
    uart_flush_input(UART_NUM_0);
    ESP_LOGI(TAG, "console UART pronta - scrivi 'help'");

    /* Larga: ottanta byte bastavano finche' i comandi erano "info" e "reboot",
       ma "ota <url> <impronta> <byte>" ne vuole da solo centodieci - solo
       l'impronta SHA-256 sono 64 caratteri. */
    char line[320];
    int n = 0;
    bool troppo_lunga = false;
    while (true) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(250)) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            if (troppo_lunga) {
                /* Una riga troppo lunga NON si esegue tagliata. Prima i
                   caratteri in piu' venivano buttati in silenzio e il comando
                   partiva mutilato: un "ota" con mezza impronta veniva
                   rifiutato lamentandosi dell'impronta, e il vero motivo - la
                   riga tagliata - non compariva da nessuna parte. */
                printf("CMD_ERR riga troppo lunga (massimo %d caratteri): non eseguita\n",
                       (int)sizeof(line) - 1);
            } else if (n > 0) {
                if (!debug_config_get()->uart_console && strcmp(line, "console on") != 0) {
                    printf("CONSOLE disabilitata dalle Impostazioni (per riattivarla: console on)\n");
                } else {
                    xSemaphoreTake(s_exec_mtx, portMAX_DELAY);
                    handle(line);
                    xSemaphoreGive(s_exec_mtx);
                }
            }
            n = 0;
            troppo_lunga = false;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        } else {
            troppo_lunga = true;
        }
    }
}

void uart_console_exec(const char *cmd, char *out, size_t out_sz)
{
    /* Stessa misura della console seriale: questa strada la usano la pagina
       web e Home Assistant, e un comando che li' funziona non deve arrivare
       tagliato solo perche' e' passato da un'altra porta. */
    char line[320];
    strlcpy(line, cmd, sizeof(line));
    if (!strncmp(line, "shot", 4)) {        // l'immagine in base64 non ha senso a schermo
        strlcpy(out, "shot: disponibile solo dalla seriale\n", out_sz);
        return;
    }
    xSemaphoreTake(s_exec_mtx, portMAX_DELAY);
    s_cap = out; s_cap_sz = out_sz; s_cap_len = 0;
    if (out_sz) out[0] = 0;
    handle(line);
    s_cap = nullptr;
    xSemaphoreGive(s_exec_mtx);
}

void uart_console_start(ESP_Brookesia_Phone *phone, int ha_app_id)
{
    if (!s_exec_mtx) s_exec_mtx = xSemaphoreCreateMutex();
    s_phone = phone;
    s_ha_id = ha_app_id;
    xTaskCreate(console_task, "uart_console", 12288, nullptr, 3, nullptr);
}

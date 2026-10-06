#include "tts_player.h"
#include "ha_config.h"
#include "ha_token.h"
#include "avviso_ui.h"
#include <string.h>
#include <strings.h>      // strncasecmp: l'indirizzo si confronta senza badare alle maiuscole
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "ha_http.h"
#include "cJSON.h"
#include "bsp_board_extra.h"      // bsp_extra_player_init(), audio_player_play()
#include "audio_player.h"

static const char *TAG = "tts";
#define TTS_OUT_PATH  "/sdcard/tts_out"   // estensione aggiunta a runtime

static bool s_inited = false;

// Ricava la base HTTP dall'URL WebSocket salvato (ws->http, wss->https, toglie /api/websocket)
static void http_base_from_ws(const char *ws, char *out, size_t sz)
{
    char tmp[HA_URL_MAX]; strncpy(tmp,ws,sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
    char *p = strstr(tmp,"/api/websocket"); if (p) *p=0;
    if (!strncmp(tmp,"wss://",6))      snprintf(out,sz,"https://%s",tmp+6);
    else if (!strncmp(tmp,"ws://",5))  snprintf(out,sz,"http://%s", tmp+5);
    else snprintf(out,sz,"%s",tmp);
}

static void tts_task(void *arg)
{
    char *message = (char*)arg;
    char url[HA_URL_MAX], token[HA_TOKEN_MAX];
    /* Il permesso si chiede a ha_token, non a ha_config: quest'ultimo pretende
       il long-lived incollato a mano, e un pannello abbinato dal telefono non
       ce l'ha mai avuto. Restava muto senza dire perche'. */
    if (!ha_config_load_url(url, sizeof(url)) || !url[0] ||
        !ha_token_get_access(token, sizeof(token))) {
        ESP_LOGW(TAG, "niente indirizzo o niente permesso: non posso parlare");
        free(message); vTaskDelete(NULL); return;
    }

    char base[HA_URL_MAX]; http_base_from_ws(url,base,sizeof(base));
    char bearer[HA_TOKEN_MAX+16]; snprintf(bearer,sizeof(bearer),"Bearer %s",token);

    // 1) chiedi a HA l'URL dell'audio generato
    char get_url[HA_URL_MAX+32]; snprintf(get_url,sizeof(get_url),"%s/api/tts_get_url",base);
    char motore[HA_TTS_MAX];
    ha_config_load_tts(motore, sizeof(motore));
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req,"engine_id",motore);
    cJSON_AddStringToObject(req,"message",message);
    char *req_str = cJSON_PrintUnformatted(req); cJSON_Delete(req);

    esp_http_client_config_t cfg = {}; cfg.url=get_url; cfg.method=HTTP_METHOD_POST;
    /* Home Assistant dietro HTTPS: il certificato indicato dall'utente, o le
       autorita' pubbliche. Stesso criterio del WebSocket. */
    if (!strncmp(get_url, "https://", 8)) {
        const char *ca = ha_tls_ca();
        if (ca) cfg.cert_pem = ca; else cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c,"Authorization",bearer);
    esp_http_client_set_header(c,"Content-Type","application/json");
    esp_http_client_set_post_field(c,req_str,strlen(req_str));

    char resp[512]=""; 
    if (esp_http_client_open(c,strlen(req_str))==ESP_OK) {
        esp_http_client_write(c,req_str,strlen(req_str));
        esp_http_client_fetch_headers(c);
        int r=esp_http_client_read_response(c,resp,sizeof(resp)-1); if(r>0) resp[r]=0;
    }
    esp_http_client_cleanup(c); free(req_str);

    cJSON *root=cJSON_Parse(resp);
    cJSON *au = root?cJSON_GetObjectItem(root,"url"):NULL;
    if (!cJSON_IsString(au)) {
        /* Il caso piu' comune e' il motore sbagliato: chi non ha Piper si
           ritrova qui. Che sia scritto quale ha provato, o si cerca a vuoto. */
        ESP_LOGE(TAG,"il motore \"%s\" non ha prodotto audio. Risposta di HA: %s", motore, resp);
        if (root) cJSON_Delete(root);
        free(message);
        vTaskDelete(NULL);
        return;
    }

    // 2) scarica l'audio su SD
    const char *audio_url = au->valuestring;
    const char *ext = strstr(audio_url,".mp3") ? ".mp3" : ".wav";
    char outpath[64]; snprintf(outpath,sizeof(outpath),"%s%s",TTS_OUT_PATH,ext);

    /* L'indirizzo dell'audio lo decide Home Assistant, e puo' arrivare
       relativo ("/api/tts_proxy/...") o assoluto. Se e' relativo va attaccato
       alla base, altrimenti esp_http_client non saprebbe nemmeno a chi
       chiedere.

       Il permesso di accesso invece si manda SOLO se l'indirizzo resta dentro
       Home Assistant. Un indirizzo che punta altrove lo si scarica lo stesso -
       non c'e' motivo di rifiutarlo - ma allegarci anche il nostro permesso
       vorrebbe dire consegnare a un estraneo le chiavi di casa, e quel permesso
       apre tutto Home Assistant. */
    char scarica[HA_URL_MAX * 2];
    bool nostro;
    if (audio_url[0] == '/') {
        snprintf(scarica, sizeof(scarica), "%s%s", base, audio_url);
        nostro = true;
    } else {
        snprintf(scarica, sizeof(scarica), "%s", audio_url);
        size_t nb = strlen(base);
        nostro = strncasecmp(scarica, base, nb) == 0 &&
                 (scarica[nb] == 0 || scarica[nb] == '/' || scarica[nb] == '?');
    }
    if (!nostro)
        ESP_LOGW(TAG, "l'audio non sta su Home Assistant: lo scarico senza mandare il permesso");

    esp_http_client_config_t dc = {}; dc.url=scarica;
    if (!strncmp(scarica, "https://", 8)) {
        const char *ca = ha_tls_ca();
        if (ca) dc.cert_pem = ca; else dc.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t dh = esp_http_client_init(&dc);
    if (nostro) esp_http_client_set_header(dh,"Authorization",bearer);
    FILE *of=fopen(outpath,"wb");
    if (of && esp_http_client_open(dh,0)==ESP_OK) {
        esp_http_client_fetch_headers(dh);
        char buf[1024]; int n;
        while ((n=esp_http_client_read(dh,buf,sizeof(buf)))>0) fwrite(buf,1,n,of);
    }
    if (of) fclose(of);
    esp_http_client_cleanup(dh);
    cJSON_Delete(root); free(message);

    // 3) riproduci (stesso motore audio del music player del factory)
    FILE *pf = fopen(outpath, "rb");
    if (!pf) {
        ESP_LOGE(TAG, "non riesco a riaprire %s", outpath);
        vTaskDelete(NULL);
        return;
    }
    /* Il player si prende in carico il file - e lo chiude a fine riproduzione -
       SOLO se accetta la richiesta. La sua intestazione lo dice chiaro: "If not
       ESP_OK returned then should be fclose()d by the caller".

       Senza questo controllo ogni riproduzione rifiutata (coda piena, player
       non avviato, formato non riconosciuto) lasciava un descrittore aperto per
       sempre. Sono pochi, e quando finiscono non si ferma solo la voce: smette
       di funzionare anche la lettura della scheda SD, quindi lo slideshow. Un
       guasto che sarebbe saltato fuori lontanissimo dalla sua causa. */
    esp_err_t pe = audio_player_play(pf);
    if (pe != ESP_OK) {
        fclose(pf);
        ESP_LOGE(TAG, "riproduzione rifiutata (%s): %s", esp_err_to_name(pe), outpath);
    } else {
        ESP_LOGI(TAG, "tts riprodotto: %s", outpath);
    }
    vTaskDelete(NULL);
}

void tts_player_init(void)
{
    if (s_inited) return;
    bsp_extra_player_init();   // registra callback audio (idempotente se gia' fatto dal music player)
    s_inited = true;
}

void tts_player_say(const char *msg)
{
    if (!msg || !*msg) return;
    char *copy = strdup(msg);
    if (!copy) return;
    /* La copia la libera il task appena creato, in tutte le sue vie d'uscita.
       Ma se il task non nasce nessuno la liberera' mai: qui ci pensa chi ha
       chiamato. */
    /* Stack largo: da quando il pannello sa parlare in HTTPS, ogni chiamata
       a Home Assistant puo' portarsi dietro una stretta di mano TLS, che
       di stack ne vuole alcuni kilobyte in piu' di una in chiaro. */
    if (xTaskCreate(tts_task, "tts", 10240, copy, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "niente memoria per il task della voce");
        free(copy);
    }
}

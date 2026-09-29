#include "tts_player.h"
#include "ha_config.h"
#include "ha_token.h"
#include "avviso_ui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
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
    esp_http_client_config_t dc = {}; dc.url=audio_url;
    esp_http_client_handle_t dh = esp_http_client_init(&dc);
    esp_http_client_set_header(dh,"Authorization",bearer);   // url puo' essere relativo/assoluto
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
    FILE *pf=fopen(outpath,"rb");
    if (pf) { audio_player_play(pf); }   // il player chiude il file a fine riproduzione
    ESP_LOGI(TAG,"tts riprodotto: %s",outpath);
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
    xTaskCreate(tts_task,"tts",6144,copy,4,NULL);
}

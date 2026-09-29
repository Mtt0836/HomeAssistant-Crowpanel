#pragma once
#include <stdbool.h>
#include "cJSON.h"

// Client WebSocket verso Home Assistant (come fa il frontend di HA).
// Auth con long-lived token, lettura della dashboard Lovelace, stati delle sole
// entita' mostrate via subscribe_entities (push), call_service.
// Watchdog di riconnessione: l'auto-reconnect integrato non e' affidabile,
// quindi un task dedicato forza stop/start quando la connessione cade.
//
// Tutte le callback girano nel task del client WebSocket.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ha_status_cb_t)(bool connected);
typedef void (*ha_tts_cb_t)(const char *message);   // evento HA "panel_tts"
// Configurazione della dashboard (risultato di lovelace/config), o NULL + errore.
typedef void (*ha_lovelace_cb_t)(cJSON *config, const char *error);
// Stato di un'entita': state puo' essere NULL se cambiano solo gli attributi;
// attrs contiene solo gli attributi nuovi/cambiati (o NULL).
typedef void (*ha_entity_cb_t)(const char *entity_id, const char *state, cJSON *attrs);

typedef struct {
    ha_status_cb_t   on_status;
    ha_lovelace_cb_t on_lovelace;
    ha_entity_cb_t   on_entity;
    ha_tts_cb_t      on_tts;
} ha_ws_callbacks_t;

// dash_path: url_path della dashboard ("lovelace" = quella predefinita).
void ha_ws_start(const char *url, const char *token, const char *dash_path,
                 const ha_ws_callbacks_t *cbs);
void ha_ws_restart(const char *url, const char *token, const char *dash_path);

// Chiede di nuovo la configurazione della dashboard (risposta via on_lovelace).
void ha_ws_request_lovelace(void);

// Sostituisce l'insieme di entita' seguite. Lo stato completo di ognuna arriva
// subito via on_entity, poi solo le variazioni. ids: array NULL-terminated.
void ha_ws_follow_entities(const char *const *ids);

// Richiesta generica (es. recorder/statistics_during_period). body: campi del
// messaggio senza graffe e senza "id". La risposta arriva a cb nel task del
// WebSocket; se la connessione cade prima, cb riceve success=false.
// Ritorna l'id del messaggio, -1 se non connesso o troppe richieste in corso.
typedef void (*ha_result_cb_t)(bool success, cJSON *result, const char *error, void *ctx);
int ha_ws_request(const char *body, ha_result_cb_t cb, void *ctx);

// Rinuncia ad aspettare la risposta a una richiesta: la callback non verra'
// piu' chiamata, nemmeno se la risposta arriva tardi o se la connessione cade.
// Da usare prima di buttare via quello che era stato passato come ctx.
// Ritorna false se la callback e' gia' partita: in quel caso il ctx e' ancora
// in uso in questo istante e va lasciato in vita ancora un momento.
bool ha_ws_cancel(int id);

// Si mette in ascolto di un evento del bus di HA (es. "crowpanel_command").
// cb riceve il campo "data" dell'evento e gira nel task del WebSocket.
// L'iscrizione viene rifatta da sola a ogni riconnessione, quindi va chiesta
// una volta sola all'avvio. Ritorna false se non c'e' piu' posto.
typedef void (*ha_event_cb_t)(const cJSON *data, void *ctx);
bool ha_ws_subscribe_event(const char *event_type, ha_event_cb_t cb, void *ctx);

// Comando che resta aperto e continua a mandare aggiornamenti, per esempio
// "weather/subscribe_forecast": le previsioni del tempo non stanno negli
// attributi dell'entita', HA le manda solo a chi si iscrive. body sono i
// campi del messaggio senza graffe e senza "id"; cb riceve tutto l'evento.
// Come sopra, l'iscrizione viene rifatta da sola a ogni riconnessione.
// Ritorna un numero da passare a ha_ws_unsubscribe, o -1 se non c'e' posto.
int  ha_ws_subscribe(const char *body, ha_event_cb_t cb, void *ctx);
void ha_ws_unsubscribe(int handle);

// Collegamento pronto: autenticazione accettata, si puo' parlare con HA.
// Viene richiamata a ogni riconnessione, non solo la prima volta: e' il punto
// giusto per rimandare a HA quello che deve sapere di noi.
typedef void (*ha_ready_cb_t)(void);
bool ha_ws_on_ready(ha_ready_cb_t cb);

// Comanda un'entita': chiama <domain>.<service> (es. toggle, turn_on, press).
void ha_ws_call_service(const char *domain, const char *service, const char *entity_id);

// Come sopra, ma passando anche dei dati al servizio: extra sono i campi JSON
// senza graffe, per esempio "\"brightness_pct\":40" per light.turn_on o
// "\"temperature\":20.5" per climate.set_temperature.
void ha_ws_call_service_data(const char *domain, const char *service,
                             const char *entity_id, const char *extra);
bool ha_ws_connected(void);

#ifdef __cplusplus
}
#endif

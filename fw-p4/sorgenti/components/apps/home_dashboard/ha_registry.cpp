#include "ha_registry.h"
#include "ha_ws.h"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "ha_reg";

#define CACHE_VALID_US  (5LL * 60 * 1000000)
#define ANSWER_WAIT_MS  12000

struct Dev { std::string name, area; };
struct Ent { std::string id, name, dev, area; };

/* Le risposte arrivano nel task del WebSocket: le svuoto li' dentro nelle
   strutture qui sopra e sveglio chi aspetta. Copiare l'albero cJSON intero
   sarebbe uno spreco (il registro di un impianto grande e' parecchie decine
   di migliaia di voci) e comunque verrebbe buttato subito dopo. */
struct Wait {
    SemaphoreHandle_t sem;
    bool ok = false;
    std::map<std::string, Dev> *devs = nullptr;
    std::map<std::string, std::string> *areas = nullptr;
    std::vector<Ent> *ents = nullptr;
};

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

static void on_areas(bool ok, cJSON *result, const char *error, void *ctx)
{
    Wait *w = (Wait *)ctx;
    w->ok = ok;
    if (ok) {
        const cJSON *a;
        cJSON_ArrayForEach(a, result) {
            const char *id = jstr(a, "area_id");
            const char *nm = jstr(a, "name");
            if (id && nm) (*w->areas)[id] = nm;
        }
    } else {
        ESP_LOGW(TAG, "stanze: %s", error ? error : "?");
    }
    xSemaphoreGive(w->sem);
}

static void on_devices(bool ok, cJSON *result, const char *error, void *ctx)
{
    Wait *w = (Wait *)ctx;
    w->ok = ok;
    if (ok) {
        const cJSON *d;
        cJSON_ArrayForEach(d, result) {
            const char *id = jstr(d, "id");
            if (!id) continue;
            /* name_by_user e' il nome che l'utente ha dato in HA: se c'e',
               e' quello che si aspetta di ritrovare qui. */
            const char *nm = jstr(d, "name_by_user");
            if (!nm) nm = jstr(d, "name");
            const char *ar = jstr(d, "area_id");
            (*w->devs)[id] = Dev{ nm ? nm : "", ar ? ar : "" };
        }
    } else {
        ESP_LOGW(TAG, "dispositivi: %s", error ? error : "?");
    }
    xSemaphoreGive(w->sem);
}

static void on_entities(bool ok, cJSON *result, const char *error, void *ctx)
{
    Wait *w = (Wait *)ctx;
    w->ok = ok;
    if (ok) {
        const cJSON *e;
        cJSON_ArrayForEach(e, result) {
            const char *id = jstr(e, "entity_id");
            if (!id) continue;
            /* Le entita' nascoste in HA sono nascoste anche qui; quelle di
               servizio (entity_category "config"/"diagnostic") restano fuori:
               in sovrimpressione non ci si mette "aggiornamento firmware". */
            if (cJSON_IsString(cJSON_GetObjectItem(e, "hidden_by"))) continue;
            if (cJSON_IsString(cJSON_GetObjectItem(e, "entity_category"))) continue;
            const char *nm = jstr(e, "name");
            if (!nm) nm = jstr(e, "original_name");
            const char *dv = jstr(e, "device_id");
            const char *ar = jstr(e, "area_id");
            w->ents->push_back(Ent{ id, nm ? nm : "", dv ? dv : "", ar ? ar : "" });
        }
    } else {
        ESP_LOGW(TAG, "entita': %s", error ? error : "?");
    }
    xSemaphoreGive(w->sem);
}

/* Ripiego per chi non e' amministratore: gli stati li legge chiunque, ma non
   dicono a quale dispositivo appartiene l'entita'. Viene fuori una lista sola. */
static void on_states(bool ok, cJSON *result, const char *error, void *ctx)
{
    Wait *w = (Wait *)ctx;
    w->ok = ok;
    if (ok) {
        const cJSON *s;
        cJSON_ArrayForEach(s, result) {
            const char *id = jstr(s, "entity_id");
            if (!id) continue;
            const cJSON *at = cJSON_GetObjectItem(s, "attributes");
            const char *nm = at ? jstr(at, "friendly_name") : nullptr;
            w->ents->push_back(Ent{ id, nm ? nm : "", "", "" });
        }
    } else {
        ESP_LOGW(TAG, "stati: %s", error ? error : "?");
    }
    xSemaphoreGive(w->sem);
}

static bool ask(const char *body, ha_result_cb_t cb, Wait *w)
{
    /* Avanzi di una richiesta precedente andata a vuoto: se restassero, la
       prossima attesa finirebbe subito credendo di aver avuto risposta. */
    xSemaphoreTake(w->sem, 0);
    w->ok = false;

    int id = ha_ws_request(body, cb, w);
    if (id < 0) return false;
    if (xSemaphoreTake(w->sem, pdMS_TO_TICKS(ANSWER_WAIT_MS)) == pdTRUE) return w->ok;

    ESP_LOGW(TAG, "nessuna risposta a %.40s", body);
    /* Qui sta il punto delicato. Finche' la richiesta resta in sospeso dentro
       ha_ws, prima o poi la callback verra' chiamata: alla risposta tardiva,
       oppure quando la connessione cade e chi aspettava viene avvisato. Se
       nel frattempo abbiamo smesso di aspettare, quella callback scriverebbe
       in una struttura che non esiste piu'. E' esattamente cosi' che il
       pannello andava in crash al riavvio di Home Assistant: HA occupato non
       rispondeva in tempo, poi cadeva la connessione e la risposta "persa"
       arrivava a un indirizzo morto.
       Quindi si rinuncia davvero alla richiesta. Se la callback e' gia'
       partita non si puo' piu' togliere: le si lascia il tempo di finire. */
    if (!ha_ws_cancel(id)) xSemaphoreTake(w->sem, pdMS_TO_TICKS(2000));
    return false;
}

/* Nome da mostrare quando HA non ne ha uno: "sensor.potenza_totale" ->
   "Potenza totale". Meglio di far leggere l'identificativo grezzo. */
static std::string pretty(const std::string &id)
{
    size_t dot = id.find('.');
    std::string s = dot == std::string::npos ? id : id.substr(dot + 1);
    for (char &c : s) if (c == '_') c = ' ';
    if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

/* Copie in PSRAM: l'elenco di un impianto grande e' parecchie decine di kB e
   la RAM interna libera e' poca (il resto del pannello la usa tutta). cJSON
   gia' lavora in PSRAM, qui ci si allinea. */
static char *psram_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memcpy(p, s, n);
    return p;
}

static char     *s_cache = nullptr;
static int64_t   s_cache_at = 0;

void ha_registry_forget(void)
{
    free(s_cache);
    s_cache = nullptr;
    s_cache_at = 0;
}

/* La struttura che le callback riempiono, e il suo semaforo, non vengono mai
   distrutti: cosi' una risposta in ritardo non trova mai memoria morta. Il
   mutex serve perche' ci sia una sola richiesta per volta a usarli (la pagina
   web puo' essere aperta da piu' browser insieme). */
static Wait             s_wait;
static SemaphoreHandle_t s_busy = nullptr;

char *ha_registry_json(void)
{
    if (s_cache && esp_timer_get_time() - s_cache_at < CACHE_VALID_US) return psram_dup(s_cache);
    if (!ha_ws_connected()) return nullptr;

    if (!s_busy) {
        s_busy = xSemaphoreCreateMutex();
        s_wait.sem = xSemaphoreCreateBinary();
    }
    if (!s_busy || !s_wait.sem) return nullptr;
    if (xSemaphoreTake(s_busy, pdMS_TO_TICKS(30000)) != pdTRUE) return nullptr;

    /* Anche queste sono statiche, e per lo stesso motivo del semaforo: sono
       le uniche cose che le callback toccano, e non devono poter sparire
       sotto i piedi di una risposta arrivata in ritardo. Il mutex qui sopra
       garantisce che ci sia un solo utilizzatore per volta. */
    static std::map<std::string, Dev> devs;
    static std::map<std::string, std::string> areas;
    static std::vector<Ent> ents;
    devs.clear();
    areas.clear();
    ents.clear();

    Wait &w = s_wait;
    w.devs = &devs;
    w.areas = &areas;
    w.ents = &ents;

    bool admin = ask("\"type\":\"config/entity_registry/list\"", on_entities, &w);
    if (admin) {
        ask("\"type\":\"config/device_registry/list\"", on_devices, &w);
        ask("\"type\":\"config/area_registry/list\"", on_areas, &w);
    } else {
        ESP_LOGI(TAG, "registro non leggibile: ripiego sugli stati");
        ents.clear();
        if (!ask("\"type\":\"get_states\"", on_states, &w)) {
            xSemaphoreGive(s_busy);
            return nullptr;
        }
    }
    xSemaphoreGive(s_busy);
    if (ents.empty()) return nullptr;

    /* Raggruppo per dispositivo tenendo l'ordine alfabetico, con le entita'
       senza dispositivo in fondo. */
    std::map<std::string, std::vector<const Ent *>> by_dev;
    for (const Ent &e : ents) by_dev[e.dev].push_back(&e);

    struct Group { std::string name, area; std::vector<const Ent *> items; };
    std::vector<Group> groups;
    std::vector<const Ent *> loose;
    for (auto &kv : by_dev) {
        if (kv.first.empty()) { loose = kv.second; continue; }
        auto it = devs.find(kv.first);
        std::string nm = it != devs.end() && !it->second.name.empty() ? it->second.name : kv.first;
        std::string ar;
        if (it != devs.end()) {
            auto a = areas.find(it->second.area);
            if (a != areas.end()) ar = a->second;
        }
        groups.push_back(Group{ nm, ar, kv.second });
    }
    std::sort(groups.begin(), groups.end(),
              [](const Group &a, const Group &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    if (!loose.empty())
        groups.push_back(Group{ admin ? "Senza dispositivo" : "Tutte le entita'", "", loose });

    cJSON *root = cJSON_CreateObject();
    cJSON *garr = cJSON_AddArrayToObject(root, "gruppi");
    for (Group &g : groups) {
        std::sort(g.items.begin(), g.items.end(), [](const Ent *a, const Ent *b) {
            return strcasecmp(a->name.empty() ? a->id.c_str() : a->name.c_str(),
                              b->name.empty() ? b->id.c_str() : b->name.c_str()) < 0;
        });
        cJSON *go = cJSON_CreateObject();
        cJSON_AddStringToObject(go, "nome", g.name.c_str());
        if (!g.area.empty()) cJSON_AddStringToObject(go, "stanza", g.area.c_str());
        cJSON *varr = cJSON_AddArrayToObject(go, "voci");
        for (const Ent *e : g.items) {
            cJSON *eo = cJSON_CreateObject();
            cJSON_AddStringToObject(eo, "id", e->id.c_str());
            cJSON_AddStringToObject(eo, "nome", e->name.empty() ? pretty(e->id).c_str() : e->name.c_str());
            cJSON_AddItemToArray(varr, eo);
        }
        cJSON_AddItemToArray(garr, go);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return nullptr;

    ESP_LOGI(TAG, "%u entita' in %u gruppi (%u byte)",
             (unsigned)ents.size(), (unsigned)groups.size(), (unsigned)strlen(out));
    free(s_cache);
    s_cache = psram_dup(out);
    s_cache_at = esp_timer_get_time();
    return out;
}

#include "lovelace_ui.h"
#include "energy_model.h"
#include "ha_http.h"
#include "web_image.h"
#include "ha_config.h"      // HA_URL_MAX: calendario e registro compongono un indirizzo
#include "json_escape.h"
#include "mdi_icon.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>        // std::move: i dati dei grafici si consegnano, non si ricopiano
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "ha_ws.h"

static const char *TAG = "lovelace";

/* Giorni in italiano per le previsioni: strftime seguirebbe la lingua
   della libreria C, che sul pannello e' inglese. */
static const char *GIORNI_BREVI[7] = { "dom", "lun", "mar", "mer", "gio", "ven", "sab" };

// ---- palette (tema scuro di HA) ----
#define C_BG        lv_color_hex(0x111318)
#define C_CARD      lv_color_hex(0x1f2226)
#define C_TEXT      lv_color_hex(0xe1e1e1)
#define C_TEXT2     lv_color_hex(0x9b9b9b)
#define C_ICON_OFF  lv_color_hex(0x33373d)
#define C_ON_LIGHT  lv_color_hex(0xff9800)
#define C_ON        lv_color_hex(0x03a9f4)
#define C_SENSOR    lv_color_hex(0x44739e)
#define C_UNAVAIL   lv_color_hex(0x6f6f6f)

// Griglia delle sezioni di HA: righe da 56 px, spazi da 8 px, 12 colonne.
#define ROW_H   56
#define GAP     8
#define SECTION_MIN_W 320

struct Entity {
    std::string state = "unknown";
    std::string name;            // friendly_name
    std::string unit;
    std::string device_class;
    std::string icon;            // "mdi:..." scelta in Home Assistant
    std::string temperatura;     // card meteo: sta negli attributi, non nello stato

    /* Le card che comandano davvero qualcosa hanno bisogno di sapere anche a
       che punto sta l'entita': quanto e' accesa la luce, a che temperatura e'
       regolato il termostato, cosa sta suonando. Tutta roba che in Home
       Assistant sta negli attributi e non nello stato.

       Nota: HA manda solo gli attributi cambiati, e quelli che spariscono
       (brightness quando la luce si spegne) non li segnala. Per questo le
       card guardano prima lo stato: a luce spenta la luminosita' non si
       mostra, qualunque numero sia rimasto qui. */
    double brightness = -1;      // light: 0..255
    double umidita_ora = NAN;    // humidifier: quella misurata
    double umidita_set = NAN;    // humidifier: quella voluta
    double umidita_min = 0, umidita_max = 100;
    std::string modo;            // humidifier: mode / climate: hvac_mode
    double temp_now   = NAN;     // climate: temperatura misurata
    double temp_set   = NAN;     // climate: temperatura voluta
    double temp_min   = 7, temp_max = 35, temp_step = 0.5;
    std::string azione;          // climate: hvac_action (heating, cooling, idle...)
    std::string titolo;          // media_player: media_title
    std::string artista;         // media_player: media_artist / media_series_title
    double volume = -1;          // media_player: 0..1
    int funzioni = 0;            // media_player: supported_features
};

enum BindKind { B_TILE, B_VALUE, B_BIGVALUE, B_NAME, B_SWITCH, B_BUTTON, B_GAUGE, B_WEATHER,
                B_LIGHT, B_CLIMATE, B_MEDIA,
                B_ALERT,      // card "alert": il riquadro si colora quando e' acceso
                B_UMID,       // card "humidifier"
                B_ALLARME };  // card "alarm-panel"

struct Bind {
    std::string eid;
    BindKind kind;
    lv_obj_t *obj;              // contenitore o label/switch
    lv_obj_t *icon;             // tile/button: cerchio dell'icona (o NULL)
    lv_obj_t *l_name;           // tile/button: label del nome (o NULL)
    lv_obj_t *l_state;          // tile: label dello stato (o NULL)
    std::string name_override;
    int extra = -1;             // B_GAUGE: indice in s_gauges
};

struct Action {
    std::string eid;
    std::string domain;
    std::string service;
};

static cJSON *s_view = NULL;
static std::vector<std::string> s_ids;
static std::vector<const char *> s_ids_c;
static std::map<std::string, Entity> s_ent;
static std::vector<Bind>   s_binds;
static std::vector<Action> s_actions;

// Card gauge: parametri e widget, indicizzati da Bind::extra.
struct Gauge {
    double min = 0, max = 100;
    bool needle = false;
    std::vector<std::pair<double, lv_color_t>> bands;   // soglie crescenti
    std::string unit;                                   // override della card
    lv_obj_t *meter = NULL;
    lv_meter_scale_t *scale = NULL;
    lv_meter_indicator_t *value_arc = NULL;              // senza needle
    lv_meter_indicator_t *needle_ind = NULL;             // con needle
    lv_obj_t *l_value = NULL;
};
static std::vector<Gauge> s_gauges;
static void gauge_refresh(Gauge &g, const std::string &eid);

/* Le tre card con cui si comanda qualcosa, indicizzate da Bind::extra come i
   gauge. Ognuna tiene solo i widget che devono cambiare quando HA dice che
   qualcosa e' cambiato. */
struct LuceCard {
    lv_obj_t *arco = NULL;          // cerchio della luminosita'
    lv_obj_t *bulbo = NULL;         // pulsante al centro, accende e spegne
    lv_obj_t *simbolo = NULL;
    lv_obj_t *l_stato = NULL;       // "45%" oppure "Spento"
};
static std::vector<LuceCard> s_luci;
static void luce_refresh(LuceCard &c, const std::string &eid);

struct TermoCard {
    lv_obj_t *arco = NULL;          // cerchio della temperatura voluta
    lv_obj_t *l_voluta = NULL;      // numerone al centro
    lv_obj_t *l_misurata = NULL;
    lv_obj_t *l_azione = NULL;      // "Riscalda", "Raffresca", "Fermo"
    lv_obj_t *simbolo = NULL;
};
static std::vector<TermoCard> s_termo;
static void termo_refresh(TermoCard &c, const std::string &eid);

struct MediaCard {
    lv_obj_t *l_titolo = NULL;
    lv_obj_t *l_artista = NULL;
    lv_obj_t *l_stato = NULL;
    lv_obj_t *simbolo = NULL;       // icona del tasto play/pausa
    lv_obj_t *volume = NULL;
    lv_obj_t *prec = NULL, *succ = NULL, *barra_vol = NULL;
};
static std::vector<MediaCard> s_media;
static void media_refresh(MediaCard &c, const std::string &eid);

// ------------------------------------------------------------------ utilita'

/* I font Montserrat integrati coprono solo ASCII piu' il simbolo di grado:
   le lettere accentate diventerebbero spazi vuoti. Le traslittero come si fa
   sulle tastiere senza accenti. */
static std::string sanitize(const char *s)
{
    std::string o;
    if (!s) return o;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { o += (char)*p++; continue; }
        if (p[0] == 0xC3 && p[1]) {
            const char *r = NULL;
            switch (p[1]) {
            case 0xA0: r = "a'"; break; case 0xA1: r = "a"; break;
            case 0xA8: r = "e'"; break; case 0xA9: r = "e'"; break;
            case 0xAC: r = "i'"; break; case 0xAD: r = "i"; break;
            case 0xB2: r = "o'"; break; case 0xB3: r = "o"; break;
            case 0xB9: r = "u'"; break; case 0xBA: r = "u"; break;
            case 0x80: r = "A'"; break; case 0x88: r = "E'"; break;
            case 0x89: r = "E'"; break; case 0x8C: r = "I'"; break;
            case 0x92: r = "O'"; break; case 0x99: r = "U'"; break;
            case 0xA4: r = "a"; break;  case 0xB6: r = "o"; break;
            case 0xBC: r = "u"; break;  case 0xB1: r = "n"; break;
            default: r = "?"; break;
            }
            o += r; p += 2; continue;
        }
        if (p[0] == 0xC2 && p[1]) {
            switch (p[1]) {
            case 0xB0: o += "\xC2\xB0"; break;     // gradi: presente nel font
            case 0xB2: o += "2"; break;
            case 0xB3: o += "3"; break;
            case 0xB5: o += "u"; break;
            default: break;
            }
            p += 2; continue;
        }
        // altri caratteri multibyte: salto l'intera sequenza
        int n = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : 2;
        while (n-- && *p) p++;
    }
    return o;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static std::string domain_of(const std::string &eid)
{
    size_t d = eid.find('.');
    return d == std::string::npos ? eid : eid.substr(0, d);
}

static Entity &ent(const std::string &eid) { return s_ent[eid]; }

static std::string display_name(const std::string &eid, const std::string &override_name)
{
    if (!override_name.empty()) return override_name;
    auto it = s_ent.find(eid);
    if (it != s_ent.end() && !it->second.name.empty()) return it->second.name;
    return eid;
}

static bool is_on(const std::string &eid)
{
    const std::string &s = ent(eid).state;
    return s == "on" || s == "open" || s == "opening" || s == "home" ||
           s == "playing" || s == "unlocked" || s == "heat" || s == "cool";
}

static bool unavailable(const std::string &eid)
{
    const std::string &s = ent(eid).state;
    return s == "unavailable" || s == "unknown";
}

// Stato numerico? Se si', ne restituisce il valore in *v.
static bool state_number(const std::string &s, double *v)
{
    if (s.empty()) return false;
    char *end = NULL;
    *v = strtod(s.c_str(), &end);
    return end && *end == 0;
}

/* Formato dei numeri come HA in italiano: virgola decimale, al massimo due
   decimali (HA usa la precisione dell'entita', che non arriva negli attributi). */
static std::string format_number(double v, const std::string &raw, const std::string &unit)
{
    char b[48];
    if (v == (long long)v && raw.find('.') == std::string::npos) snprintf(b, sizeof(b), "%lld", (long long)v);
    else {
        snprintf(b, sizeof(b), "%.2f", v);
        size_t n = strlen(b);
        while (n > 0 && b[n - 1] == '0') b[--n] = 0;
        if (n > 0 && b[n - 1] == '.') b[--n] = 0;
    }
    for (char *c = b; *c; c++) if (*c == '.') *c = ',';
    std::string out = b;
    if (!unit.empty()) out += (unit == "%" ? "" : " ") + unit;
    return out;
}

/* Da una data ISO di Home Assistant ("2026-09-25T06:32:46+00:00") all'ora
   locale del pannello. Ritorna false se non e' una data.

   L'istante arriva con il suo fuso, che spesso e' UTC: va portato a UTC e poi
   convertito, se no le previsioni serali finiscono nel giorno sbagliato.
   timegm non esiste nella libreria C del pannello e mktime userebbe il fuso
   locale falsando il conto, quindi i giorni dall'epoca si calcolano a mano
   (e' l'algoritmo days_from_civil di Howard Hinnant). */
static bool iso_to_local(const char *iso, struct tm *out, time_t *when)
{
    if (!iso || strlen(iso) < 16) return false;
    struct tm t = {};
    int off_h = 0, off_m = 0;
    char segno = '+';
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) < 5) return false;
    const char *z = strpbrk(iso + 10, "+-");
    if (z) sscanf(z, "%c%2d:%2d", &segno, &off_h, &off_m);

    int y = t.tm_year;
    int m = t.tm_mon;
    if (m <= 2) y--;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long giorni = (long long)era * 146097 + (long long)doe - 719468;

    time_t utc = (time_t)(giorni * 86400LL + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
    int off = off_h * 3600 + off_m * 60;
    utc -= (segno == '-') ? -off : off;
    if (when) *when = utc;
    localtime_r(&utc, out);
    return true;
}

/* Data e ora come le scriverebbe una persona: per oggi e domani basta l'ora,
   per i giorni vicini il giorno della settimana, per il resto la data. */
static std::string iso_locale(const char *iso)
{
    struct tm d;
    time_t quando;
    if (!iso_to_local(iso, &d, &quando)) return "";

    time_t adesso = time(NULL);
    struct tm oggi;
    localtime_r(&adesso, &oggi);

    char b[32];
    long giorni = (long)((quando - adesso) / 86400);
    if (d.tm_yday == oggi.tm_yday && d.tm_year == oggi.tm_year)
        snprintf(b, sizeof(b), "oggi %02d:%02d", d.tm_hour, d.tm_min);
    else if (giorni >= -1 && giorni < 6)
        snprintf(b, sizeof(b), "%s %02d:%02d", GIORNI_BREVI[d.tm_wday], d.tm_hour, d.tm_min);
    else
        snprintf(b, sizeof(b), "%02d/%02d %02d:%02d", d.tm_mday, d.tm_mon + 1, d.tm_hour, d.tm_min);
    return b;
}

static std::string format_state(const std::string &eid)
{
    Entity &e = ent(eid);
    const std::string &s = e.state;
    std::string dom = domain_of(eid);
    if (s == "unavailable") return "Non disponibile";
    if (s == "unknown")     return "Sconosciuto";

    double v;
    if (state_number(s, &v)) return format_number(v, s, e.unit);
    /* I sensori di tipo "timestamp" hanno per stato una data ISO intera:
       scritta cosi' com'e' occupa mezza card e non si legge. */
    if (e.device_class == "timestamp") {
        std::string t = iso_locale(s.c_str());
        if (!t.empty()) return t;
    }
    if (dom == "binary_sensor") {
        if (s == "on")  return "Attivo";
        if (s == "off") return "Inattivo";
    }
    if (s == "on")        return "Acceso";
    if (s == "off")       return "Spento";
    if (s == "open")      return "Aperto";
    if (s == "closed")    return "Chiuso";
    if (s == "opening")   return "In apertura";
    if (s == "closing")   return "In chiusura";
    if (s == "home")      return "A casa";
    if (s == "not_home")  return "Fuori casa";
    if (s == "locked")    return "Bloccato";
    if (s == "unlocked")  return "Sbloccato";
    if (s == "playing")   return "In riproduzione";
    if (s == "paused")    return "In pausa";
    if (s == "idle")      return "Inattivo";
    std::string out = sanitize(s.c_str());
    if (!e.unit.empty()) out += " " + e.unit;
    return out;
}

/* Servizio da chiamare al tocco, "" se l'entita' non si comanda.
   Le serrature restano escluse di proposito: aprirle con un tocco
   accidentale sul pannello non e' accettabile. */
static std::string tap_service(const std::string &eid)
{
    std::string d = domain_of(eid);
    if (d == "light" || d == "switch" || d == "fan" || d == "input_boolean" ||
        d == "automation" || d == "siren" || d == "humidifier" || d == "cover" ||
        d == "media_player" || d == "climate")
        return "toggle";
    if (d == "button" || d == "input_button") return "press";
    if (d == "script" || d == "scene")        return "turn_on";
    return "";
}

static lv_color_t active_color(const std::string &eid)
{
    if (unavailable(eid)) return C_UNAVAIL;
    std::string d = domain_of(eid);
    if (d == "sensor") return C_SENSOR;
    if (!is_on(eid))   return C_ICON_OFF;
    return d == "light" ? C_ON_LIGHT : C_ON;
}

/* Le quindici condizioni che HA puo' riportare, con l'icona e il nome in
   italiano. Sono quelle elencate nella sua documentazione: se ne arriva una
   che non conosciamo si mostra il testo grezzo, che e' meglio di niente. */
struct WxCond { const char *ha; const char *icona; const char *nome; };
static const WxCond WX_COND[] = {
    { "clear-night",      "weather-night",               "Sereno" },
    { "cloudy",           "weather-cloudy",              "Nuvoloso" },
    { "exceptional",      "alert-circle-outline",        "Eccezionale" },
    { "fog",              "weather-fog",                 "Nebbia" },
    { "hail",             "weather-hail",                "Grandine" },
    { "lightning",        "weather-lightning",           "Temporale" },
    { "lightning-rainy",  "weather-lightning-rainy",     "Temporale e pioggia" },
    { "partlycloudy",     "weather-partly-cloudy",       "Poco nuvoloso" },
    { "pouring",          "weather-pouring",             "Rovesci" },
    { "rainy",            "weather-rainy",               "Pioggia" },
    { "snowy",            "weather-snowy",               "Neve" },
    { "snowy-rainy",      "weather-snowy-rainy",         "Neve e pioggia" },
    { "sunny",            "weather-sunny",               "Sereno" },
    { "windy",            "weather-windy",               "Vento" },
    { "windy-variant",    "weather-windy-variant",       "Vento e nuvole" },
};

static const WxCond *wx_cond(const char *c)
{
    if (!c) return NULL;
    for (const WxCond &w : WX_COND) if (!strcmp(w.ha, c)) return &w;
    return NULL;
}

static std::string wx_icona(const char *cond)
{
    char buf[8];
    const WxCond *w = wx_cond(cond);
    if (w && mdi_icon_text(w->icona, buf, sizeof(buf))) return buf;
    return mdi_icon_text("weather-partly-cloudy", buf, sizeof(buf)) ? std::string(buf) : std::string();
}

/* Il carattere dell'icona da scrivere nel cerchio.

   Prima quella scelta in Home Assistant, che e' la sola a sapere davvero
   cosa rappresenta quell'entita'; se manca, una scelta per tipo. Fino a ieri
   il pannello l'icona di HA la ignorava del tutto e pescava fra otto simboli
   di LVGL: una lampadina e una tapparella avevano lo stesso disegno. */
static std::string icon_symbol(const std::string &eid)
{
    char buf[8];
    const Entity &e = ent(eid);
    if (!e.icon.empty() && mdi_icon_text(e.icon.c_str(), buf, sizeof(buf))) return buf;

    std::string d = domain_of(eid);
    const std::string &dc = e.device_class;
    const char *nome = "cog";
    if (d == "light")                                    nome = "lightbulb";
    else if (d == "switch" || d == "input_boolean")      nome = "toggle-switch";
    else if (d == "media_player")                        nome = "speaker";
    else if (d == "cover")                               nome = "window-shutter";
    else if (d == "fan")                                 nome = "fan";
    else if (d == "climate")                             nome = "thermostat";
    else if (d == "lock")                                nome = "lock";
    else if (d == "script" || d == "scene" || d == "automation") nome = "play";
    else if (d == "button" || d == "input_button")       nome = "gesture-tap-button";
    else if (d == "person" || d == "device_tracker")     nome = "account";
    else if (d == "weather")                             nome = "weather-partly-cloudy";
    else if (dc == "battery")                            nome = "battery";
    else if (dc == "power" || dc == "energy")            nome = "flash";
    else if (dc == "voltage" || dc == "current")         nome = "lightning-bolt";
    else if (dc == "temperature")                        nome = "thermometer";
    else if (dc == "humidity")                           nome = "water-percent";
    else if (dc == "motion" || dc == "occupancy")        nome = "motion-sensor";
    else if (dc == "door" || dc == "garage_door")        nome = "door";
    else if (dc == "window" || dc == "opening")          nome = "window-closed";
    else if (d == "binary_sensor")                       nome = "eye";
    return mdi_icon_text(nome, buf, sizeof(buf)) ? std::string(buf) : std::string();
}

// ------------------------------------------------------------------ widget base

static lv_obj_t *mk_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = mk_box(parent, w, h);
    lv_obj_set_style_bg_color(o, C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const std::string &txt, const lv_font_t *font,
                          lv_color_t col, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt.c_str());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    return l;
}

static lv_obj_t *mk_icon(lv_obj_t *parent, int size)
{
    lv_obj_t *c = mk_box(parent, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_t *s = lv_label_create(c);
    lv_obj_set_style_text_color(s, lv_color_white(), 0);
    lv_obj_set_style_text_font(s, mdi_icon_font(size >= 48 ? 32 : 20), 0);
    lv_obj_center(s);
    return c;
}

/* Le card nuove stanno in fondo, vicino allo smistamento, ma refresh() deve
   poterle richiamare: qui si dice solo che esistono. */
struct Umid;
struct Allarme;
static void alert_refresh(Bind &b);
static void umid_refresh(Umid *u, const std::string &eid);
static void allarme_refresh(Allarme *a);
extern std::vector<Umid *> s_umid;
extern std::vector<Allarme *> s_allarmi;

static void refresh(Bind &b)
{
    const std::string name = sanitize(display_name(b.eid, b.name_override).c_str());
    switch (b.kind) {
    case B_TILE:
    case B_BUTTON:
        if (b.icon) {
            lv_obj_set_style_bg_color(b.icon, active_color(b.eid), 0);
            lv_label_set_text(lv_obj_get_child(b.icon, 0), icon_symbol(b.eid).c_str());
        }
        if (b.l_name)  lv_label_set_text(b.l_name, name.c_str());
        if (b.l_state) lv_label_set_text(b.l_state, format_state(b.eid).c_str());
        break;
    case B_VALUE:
    case B_BIGVALUE:
        lv_label_set_text(b.obj, format_state(b.eid).c_str());
        break;
    case B_NAME:
        lv_label_set_text(b.obj, name.c_str());
        break;
    case B_SWITCH:
        if (is_on(b.eid)) lv_obj_add_state(b.obj, LV_STATE_CHECKED);
        else              lv_obj_clear_state(b.obj, LV_STATE_CHECKED);
        if (unavailable(b.eid)) lv_obj_add_state(b.obj, LV_STATE_DISABLED);
        else                    lv_obj_clear_state(b.obj, LV_STATE_DISABLED);
        break;
    case B_GAUGE:
        if (b.extra >= 0 && b.extra < (int)s_gauges.size()) gauge_refresh(s_gauges[b.extra], b.eid);
        break;
    case B_ALERT:
        alert_refresh(b);
        break;
    case B_UMID:
        if (b.extra >= 0 && b.extra < (int)s_umid.size()) umid_refresh(s_umid[b.extra], b.eid);
        break;
    case B_ALLARME:
        if (b.extra >= 0 && b.extra < (int)s_allarmi.size()) allarme_refresh(s_allarmi[b.extra]);
        break;
    case B_LIGHT:
        if (b.extra >= 0 && b.extra < (int)s_luci.size()) luce_refresh(s_luci[b.extra], b.eid);
        if (b.l_name) lv_label_set_text(b.l_name, name.c_str());
        break;
    case B_CLIMATE:
        if (b.extra >= 0 && b.extra < (int)s_termo.size()) termo_refresh(s_termo[b.extra], b.eid);
        if (b.l_name) lv_label_set_text(b.l_name, name.c_str());
        break;
    case B_MEDIA:
        if (b.extra >= 0 && b.extra < (int)s_media.size()) media_refresh(s_media[b.extra], b.eid);
        if (b.l_name) lv_label_set_text(b.l_name, name.c_str());
        break;
    case B_WEATHER: {
        /* Lo stato di un'entita' meteo e' la condizione ("rainy"), mentre la
           temperatura di adesso sta fra i suoi attributi. */
        const Entity &e = ent(b.eid);
        if (b.icon) lv_label_set_text(b.icon, wx_icona(e.state.c_str()).c_str());
        if (b.l_name) {
            const WxCond *w = wx_cond(e.state.c_str());
            lv_label_set_text(b.l_name, w ? w->nome : sanitize(e.state.c_str()).c_str());
        }
        if (b.l_state)
            lv_label_set_text(b.l_state,
                              e.temperatura.empty() ? "--"
                                                    : (e.temperatura + "°").c_str());
        break;
    }
    }
}

static void bind(const std::string &eid, BindKind k, lv_obj_t *obj, lv_obj_t *icon,
                 lv_obj_t *l_name, lv_obj_t *l_state, const std::string &name_override,
                 int extra = -1)
{
    Bind b{eid, k, obj, icon, l_name, l_state, name_override, extra};
    refresh(b);
    s_binds.push_back(b);
}

static void action_cb(lv_event_t *e)
{
    size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_actions.size()) return;
    const Action &a = s_actions[i];
    ESP_LOGI(TAG, "tocco: %s.%s %s", a.domain.c_str(), a.service.c_str(), a.eid.c_str());
    ha_ws_call_service(a.domain.c_str(), a.service.c_str(), a.eid.c_str());
}

/* Collega il tocco su obj all'azione della card. tap_action "none" disattiva;
   "toggle"/"call-service" o il default comandano l'entita' se comandabile
   ("more-info" non esiste sul pannello, quindi lo tratto come il default). */
static void attach_action(lv_obj_t *obj, const std::string &eid, const cJSON *card,
                          lv_event_code_t code)
{
    const cJSON *ta = card ? cJSON_GetObjectItem(card, "tap_action") : NULL;
    const char *act = ta ? jstr(ta, "action") : NULL;
    if (act && !strcmp(act, "none")) return;
    std::string svc = tap_service(eid);
    if (svc.empty()) return;
    s_actions.push_back({eid, domain_of(eid), svc});
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2a2e33), LV_STATE_PRESSED);
    lv_obj_add_event_cb(obj, action_cb, code, (void *)(intptr_t)(s_actions.size() - 1));
}

// ------------------------------------------------------------------ dimensioni

static const char *card_type(const cJSON *card)
{
    const char *t = jstr(card, "type");
    return t ? t : "";
}

// Colonne (su 12) occupate dalla card in una sezione, come il default di HA.
static int card_columns(const cJSON *card)
{
    const cJSON *go = cJSON_GetObjectItem(card, "grid_options");
    const cJSON *c  = go ? cJSON_GetObjectItem(go, "columns") : NULL;
    if (cJSON_IsNumber(c)) return LV_CLAMP(1, c->valueint, 12);
    if (cJSON_IsString(c) && !strcmp(c->valuestring, "full")) return 12;
    const cJSON *lo = cJSON_GetObjectItem(card, "layout_options");   // formato precedente, su 4
    const cJSON *gc = lo ? cJSON_GetObjectItem(lo, "grid_columns") : NULL;
    if (cJSON_IsNumber(gc)) return LV_CLAMP(1, gc->valueint * 3, 12);
    if (cJSON_IsString(gc) && !strcmp(gc->valuestring, "full")) return 12;
    std::string t = card_type(card);
    if (t == "tile" || t == "entity" || t == "sensor" || t == "gauge") return 6;
    if (t == "button") return 3;
    if (t == "light" || t == "thermostat") return 6;
    return 12;
}

// Altezza in px se la card ha righe fisse, 0 = si adatta al contenuto.
/* Le stesse regole, esposte all'editor web. */
int ll_card_columns(const cJSON *card) { return card_columns(card); }

int ll_card_rows(const cJSON *card)
{
    const cJSON *go = cJSON_GetObjectItem(card, "grid_options");
    const cJSON *r  = go ? cJSON_GetObjectItem(go, "rows") : NULL;
    if (cJSON_IsNumber(r)) return r->valueint;
    if (cJSON_IsString(r)) return 0;                  // "auto"
    std::string t = card_type(card);
    if (t == "tile") return 1;
    if (t == "button" || t == "entity" || t == "sensor") return 2;
    return 0;
}

void ll_grid_metrics(int *row_h, int *gap, int *screen_w, int *screen_h)
{
    if (row_h)    *row_h = ROW_H;
    if (gap)      *gap = GAP;
    if (screen_w) *screen_w = LV_HOR_RES;
    if (screen_h) *screen_h = LV_VER_RES;
}

static int card_rows_height(const cJSON *card)
{
    const cJSON *go = cJSON_GetObjectItem(card, "grid_options");
    const cJSON *r  = go ? cJSON_GetObjectItem(go, "rows") : NULL;
    int rows = 0;
    if (cJSON_IsNumber(r)) rows = r->valueint;
    else if (cJSON_IsString(r)) rows = 0;          // "auto"
    else {
        std::string t = card_type(card);
        if (t == "tile") rows = 1;
        else if (t == "button" || t == "entity" || t == "sensor") rows = 2;
    }
    return rows > 0 ? rows * ROW_H + (rows - 1) * GAP : 0;
}

// ------------------------------------------------------------------ card

static void render_card(lv_obj_t *parent, const cJSON *card, int w, int h);





// ------------------------------------------------------------------ Energia
//
// Le card del pannello Energia di Home Assistant. Non leggono entita': leggono
// il modello in energy_model.cpp, che a sua volta legge le preferenze Energia
// dell'utente. Per questo funzionano con qualunque configurazione - chi ha solo
// il contatore, chi ha i pannelli senza batteria, chi ha anche gas e acqua -
// senza che ci sia scritto da nessuna parte il nome di un sensore.

#define C_SOLE      lv_color_hex(0xff9800)
#define C_RETE      lv_color_hex(0x488fc2)
#define C_IMMESSO   lv_color_hex(0x8353d1)
#define C_BATT      lv_color_hex(0x4db6ac)
#define C_CASA      lv_color_hex(0x9e9e9e)
#define C_GASC      lv_color_hex(0x8b6d5c)
#define C_ACQUA     lv_color_hex(0x4fc3f7)
/* Il grigio della parte spenta delle lancette. Lo stesso valore c'e' piu'
   avanti come C_GAUGE_BG, ma quella riga sta sotto a questo blocco e il
   compilatore legge dall'alto. */
#define C_ARCO_BG   lv_color_hex(0x3a3d42)

static bool is_energy_card(const std::string &t) { return t.rfind("energy-", 0) == 0; }

/* Il registro delle card dell'energia presenti nella vista.

   Il contenitore si crea una volta sola, quando la vista viene costruita; il
   contenuto si rifa' dentro lo stesso contenitore ogni volta che arrivano
   numeri nuovi. Ricostruire l'intera vista sarebbe stato molto piu' semplice,
   ma ogni cinque minuti l'utente si vedrebbe saltare via lo scorrimento sotto
   le dita mentre sta leggendo. */
struct EnCard {
    lv_obj_t   *box;
    const cJSON *card;      // vive dentro s_view, come le specifiche dei grafici
    std::string tipo;
    int w, h;
};
static std::vector<EnCard> s_encards;

/* Quante cifre ha senso mostrare: sotto i 10 kWh il decimo conta, sopra i 100
   e' rumore. La pagina Energia di HA si comporta cosi' e l'occhio ci e'
   abituato. */
static std::string kwh(double v, const char *unita = "kWh")
{
    char b[48];
    double a = fabs(v);
    if (a < 10)       snprintf(b, sizeof(b), "%.2f %s", v, unita);
    else if (a < 100) snprintf(b, sizeof(b), "%.1f %s", v, unita);
    else              snprintf(b, sizeof(b), "%.0f %s", v, unita);
    return b;
}

/* Titolo della card e, se non c'e' niente da mostrare, il motivo. Ritorna
   false quando la card non deve disegnare altro. */
static bool energia_testa(lv_obj_t *c, const cJSON *card, int w,
                          const char *titolo_def, const EnergyModel *m, bool serve)
{
    const char *tit = jstr(card, "title");
    mk_label(c, sanitize(tit ? tit : titolo_def), &lv_font_montserrat_18, C_TEXT, w - 24);

    const char *guaio = NULL;
    if (!m->errore.empty())   guaio = m->errore.c_str();
    else if (!m->prefs_lette) guaio = "Leggo la configurazione Energia...";
    else if (!serve)          guaio = "Non configurato in Home Assistant";
    else if (!m->dati_pronti) guaio = "Raccolgo i dati...";
    if (guaio) {
        mk_label(c, guaio, &lv_font_montserrat_14, C_TEXT2, w - 24);
        return false;
    }
    return true;
}

/* Una riga "icona - nome - valore", il mattone di quasi tutte queste card. */
static void riga_energia(lv_obj_t *parent, int w, const char *icona, lv_color_t col,
                         const char *nome, const std::string &val)
{
    lv_obj_t *r = mk_box(parent, w, 34);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *i = mk_icon(r, 22);
    lv_obj_set_style_bg_color(i, C_ICON_OFF, 0);
    char gl[8];
    lv_obj_t *g = lv_obj_get_child(i, 0);
    lv_label_set_text(g, mdi_icon_text(icona, gl, sizeof(gl)) ? gl : "");
    lv_obj_set_style_text_color(g, col, 0);

    lv_obj_t *n = mk_label(r, nome, &lv_font_montserrat_14, C_TEXT2, w / 2);
    lv_obj_set_flex_grow(n, 1);
    mk_label(r, val, &lv_font_montserrat_16, C_TEXT, w / 2);
}

// ---- distribuzione: da dove arriva la corrente che consumi ----
static void fill_distribution(lv_obj_t *c, const cJSON *card, int w, int h)
{
    const EnergyModel *m = energy_model_get();
    if (!energia_testa(c, card, w, "Distribuzione", m,
                       m->c_e_rete || m->c_e_solare || m->c_e_batteria)) return;
    int iw = w - 24;
    if (m->c_e_solare)
        riga_energia(c, iw, "weather-sunny", C_SOLE, "Fotovoltaico", kwh(m->solare));
    if (m->c_e_rete) {
        riga_energia(c, iw, "transmission-tower", C_RETE, "Dalla rete", kwh(m->rete_presa));
        riga_energia(c, iw, "transmission-tower", C_IMMESSO, "In rete", kwh(m->rete_immessa));
    }
    if (m->c_e_batteria) {
        riga_energia(c, iw, "battery", C_BATT, "Dalla batteria", kwh(m->batteria_scarica));
        riga_energia(c, iw, "battery-charging", C_BATT, "In batteria", kwh(m->batteria_carica));
    }
    riga_energia(c, iw, "home", C_CASA, "Consumo di casa", kwh(m->casa));
    if (m->autosufficienza >= 0) {
        char t[64];
        snprintf(t, sizeof(t), "Autosufficienza %.0f%%", m->autosufficienza);
        mk_label(c, t, &lv_font_montserrat_14, C_TEXT2, iw);
    }
}

// ---- le lancette: stessa cosa con un numero diverso ----
static void fill_gauge(lv_obj_t *c, const cJSON *card, int w, int h, const char *titolo,
                       double valore, bool disponibile, lv_color_t col, const char *sotto)
{
    const EnergyModel *m = energy_model_get();
    if (!energia_testa(c, card, w, titolo, m, disponibile)) return;
    if (valore < 0) {
        mk_label(c, "Non calcolabile in questo periodo", &lv_font_montserrat_14, C_TEXT2, w - 24);
        return;
    }

    lv_obj_t *arco = lv_arc_create(c);
    int d = LV_MIN(w - 40, (h > 0 ? h : 200) - 90);
    if (d < 90) d = 90;
    lv_obj_set_size(arco, d, d);
    lv_arc_set_rotation(arco, 135);
    lv_arc_set_bg_angles(arco, 0, 270);
    lv_arc_set_range(arco, 0, 100);
    lv_arc_set_value(arco, (int)(valore + 0.5));
    lv_obj_remove_style(arco, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arco, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arco, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arco, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arco, C_ARCO_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arco, col, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arco, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arco, 0, 0);

    char t[32];
    snprintf(t, sizeof(t), "%.0f%%", valore);
    lv_obj_t *l = mk_label(arco, t, &lv_font_montserrat_24, C_TEXT, d - 24);
    lv_obj_center(l);
    if (sotto) mk_label(c, sotto, &lv_font_montserrat_14, C_TEXT2, w - 24);
}

// ---- la tabella dei numeri ----
static void fill_table(lv_obj_t *c, const cJSON *card, int w, int h)
{
    const EnergyModel *m = energy_model_get();
    if (!energia_testa(c, card, w, "Sorgenti", m,
                       m->c_e_rete || m->c_e_solare || m->c_e_batteria ||
                       m->c_e_gas || m->c_e_acqua)) return;
    int iw = w - 24;
    if (m->c_e_solare) riga_energia(c, iw, "solar-power", C_SOLE, "Prodotto", kwh(m->solare));
    if (m->c_e_rete) {
        riga_energia(c, iw, "flash", C_RETE, "Preso dalla rete", kwh(m->rete_presa));
        riga_energia(c, iw, "flash-off", C_IMMESSO, "Immesso in rete", kwh(m->rete_immessa));
    }
    if (m->c_e_batteria) {
        riga_energia(c, iw, "battery-charging", C_BATT, "Caricata", kwh(m->batteria_carica));
        riga_energia(c, iw, "battery", C_BATT, "Scaricata", kwh(m->batteria_scarica));
    }
    if (m->c_e_gas)   riga_energia(c, iw, "fire", C_GASC, "Gas", kwh(m->gas, m->unita_gas.c_str()));
    if (m->c_e_acqua) riga_energia(c, iw, "water", C_ACQUA, "Acqua", kwh(m->acqua, m->unita_acqua.c_str()));
    riga_energia(c, iw, "home", C_CASA, "Totale casa", kwh(m->casa));
    if (m->c_e_costo) {
        char t[72];
        snprintf(t, sizeof(t), "Spesa %.2f   Ricavo %.2f", m->costo, m->compenso);
        mk_label(c, t, &lv_font_montserrat_14, C_TEXT2, iw);
    }
}

// ---- i dispositivi che consumano di piu' ----
static void fill_devices(lv_obj_t *c, const cJSON *card, int w, int h)
{
    const EnergyModel *m = energy_model_get();
    if (!energia_testa(c, card, w, "Dispositivi", m, m->c_e_dispositivi)) return;

    std::vector<const EnergyVoce *> v;
    for (const EnergyVoce &d : m->dispositivi) if (d.totale > 0) v.push_back(&d);
    std::sort(v.begin(), v.end(),
              [](const EnergyVoce *a, const EnergyVoce *b) { return a->totale > b->totale; });
    if (v.empty()) {
        mk_label(c, "Nessun consumo nel periodo", &lv_font_montserrat_14, C_TEXT2, w - 24);
        return;
    }
    /* Su un pannello si leggono i primi, non tutti: oltre una certa lunghezza
       diventa una lista della spesa che nessuno guarda. */
    size_t quanti = v.size() < 8 ? v.size() : 8;
    for (size_t i = 0; i < quanti; i++)
        riga_energia(c, w - 24, "power-plug", C_ON, sanitize(v[i]->nome.c_str()).c_str(),
                     kwh(v[i]->totale));
}

// ---- i grafici a barre ----
static void fill_barre(lv_obj_t *c, const cJSON *card, int w, int h, const char *titolo,
                       bool disponibile, const std::vector<float> *s1, lv_color_t c1,
                       const std::vector<float> *s2, lv_color_t c2)
{
    const EnergyModel *m = energy_model_get();
    if (!energia_testa(c, card, w, titolo, m, disponibile)) return;
    if (m->npunti <= 0) return;

    lv_obj_t *ch = lv_chart_create(c);
    int alt = (h > 0 ? h : 3 * ROW_H + 2 * GAP) - 80;
    if (alt < 90) alt = 90;
    lv_obj_set_size(ch, w - 30, alt);
    lv_chart_set_type(ch, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(ch, m->npunti);
    lv_chart_set_div_line_count(ch, 4, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_line_color(ch, C_ARCO_BG, LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, LV_PART_INDICATOR);

    float mx = 0;
    if (s1) for (float x : *s1) if (x > mx) mx = x;
    if (s2) for (float x : *s2) if (x > mx) mx = x;
    if (mx <= 0) mx = 1;
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(mx * 1.1f + 1));

    for (int pass = 0; pass < 2; pass++) {
        const std::vector<float> *sv = pass ? s2 : s1;
        if (!sv) continue;
        lv_chart_series_t *ser = lv_chart_add_series(ch, pass ? c2 : c1, LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < m->npunti; i++)
            lv_chart_set_next_value(ch, ser, (lv_coord_t)(i < (int)sv->size() ? (*sv)[i] : 0));
    }

    char sotto[80];
    snprintf(sotto, sizeof(sotto), "%s - %d fasce", energy_periodo_nome(m->periodo), m->npunti);
    mk_label(c, sotto, &lv_font_montserrat_14, C_TEXT2, w - 24);
}

// ---- i pulsanti del periodo: l'unica card che comanda, e comanda tutte ----
static void periodo_cb(lv_event_t *e)
{
    EnergyPeriodo p = (EnergyPeriodo)(intptr_t)lv_event_get_user_data(e);
    energy_model_set_periodo(p);
}

static void fill_date(lv_obj_t *c, const cJSON *card, int w, int h)
{
    const EnergyModel *m = energy_model_get();
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i <= (int)EN_ANNO; i++) {
        EnergyPeriodo p = (EnergyPeriodo)i;
        bool scelto = (p == m->periodo);
        lv_obj_t *b = lv_btn_create(c);
        lv_obj_set_height(b, 40);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, scelto ? C_ON : lv_color_hex(0x2b2d31), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, periodo_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, energy_periodo_nome(p));
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, scelto ? lv_color_hex(0xffffff) : C_TEXT2, 0);
        lv_obj_center(l);
    }
}

/* Riempie una card gia' creata. Chiamata alla costruzione e a ogni numero
   nuovo. */
static void fill_energy_card(lv_obj_t *c, const cJSON *card, const std::string &t, int w, int h)
{
    const EnergyModel *m = energy_model_get();

    if (t == "energy-date-selection")          { fill_date(c, card, w, h); return; }
    if (t == "energy-distribution")            { fill_distribution(c, card, w, h); return; }
    if (t == "energy-sources-table")           { fill_table(c, card, w, h); return; }
    if (t == "energy-devices-graph" ||
        t == "energy-devices-detail-graph")    { fill_devices(c, card, w, h); return; }

    if (t == "energy-self-sufficiency-gauge") {
        fill_gauge(c, card, w, h, "Autosufficienza", m->autosufficienza,
                   m->c_e_rete || m->c_e_solare, C_SOLE,
                   "Consumi non comprati dalla rete");
        return;
    }
    if (t == "energy-solar-consumed-gauge") {
        fill_gauge(c, card, w, h, "Sole usato in casa", m->solare_usato,
                   m->c_e_solare, C_SOLE, "Prodotto rimasto in casa");
        return;
    }
    if (t == "energy-grid-neutrality-gauge" || t == "energy-grid-balance") {
        fill_gauge(c, card, w, h, "Bilancio con la rete", m->neutralita,
                   m->c_e_rete, C_IMMESSO, "Immesso sul totale scambiato");
        return;
    }
    if (t == "energy-carbon-consumed-gauge") {
        /* Vorrebbe l'integrazione CO2 Signal, che il pannello non interroga.
           Meglio dirlo che disegnare una lancetta inventata. */
        energia_testa(c, card, w, "Energia a basse emissioni", m, false);
        return;
    }

    if (t == "energy-usage-graph") {
        fill_barre(c, card, w, h, "Consumo", m->c_e_rete || m->c_e_solare,
                   &m->g_rete_presa, C_RETE, m->c_e_solare ? &m->g_solare : NULL, C_SOLE);
        return;
    }
    if (t == "energy-solar-graph") {
        fill_barre(c, card, w, h, "Fotovoltaico", m->c_e_solare, &m->g_solare, C_SOLE, NULL, C_SOLE);
        return;
    }
    if (t == "energy-gas-graph") {
        fill_barre(c, card, w, h, "Gas", m->c_e_gas, &m->g_gas, C_GASC, NULL, C_GASC);
        return;
    }
    if (t == "energy-water-graph") {
        fill_barre(c, card, w, h, "Acqua", m->c_e_acqua, &m->g_acqua, C_ACQUA, NULL, C_ACQUA);
        return;
    }

    /* energy-sankey, energy-compare e i tipi che HA aggiungera': il diagramma
       a flussi non sta in una card di un pannello da 10 pollici e il confronto
       fra due periodi vuole due raccolte. Si mostra la tabella, che dice le
       stesse cose in numeri invece che in disegno. */
    fill_table(c, card, w, h);
}

/* Scorre la vista in cerca di card dell'energia, comprese quelle annidate
   dentro stack, griglie e sezioni. Finche' non ce n'e' nemmeno una, il modello
   non chiede niente a Home Assistant: un pannello che non mostra l'energia non
   ha motivo di scaricarne i dati. */
static bool cerca_energia(const cJSON *n)
{
    if (!n) return false;
    if (cJSON_IsObject(n)) {
        const cJSON *t = cJSON_GetObjectItem(n, "type");
        if (cJSON_IsString(t) && is_energy_card(t->valuestring)) return true;
    }
    const cJSON *f;
    cJSON_ArrayForEach(f, n)
        if ((cJSON_IsObject(f) || cJSON_IsArray(f)) && cerca_energia(f)) return true;
    return false;
}

/* Crea il contenitore e lo iscrive al registro. */
static void render_energy_card(lv_obj_t *parent, const cJSON *card,
                               const std::string &t, int w, int h)
{
    int alt = h > 0 ? h : (t == "energy-date-selection" ? ROW_H : 3 * ROW_H + 2 * GAP);
    lv_obj_t *c = mk_card(parent, w, alt);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    s_encards.push_back({c, card, t, w, alt});
    fill_energy_card(c, card, t, w, alt);
}

/* Arrivano numeri nuovi: ogni card si rifa' dentro il suo contenitore, che
   resta dov'era. Chiamata dal filo del modello, che ha gia' il lock. */
static void energia_aggiorna(void)
{
    bsp_display_lock(0);
    for (EnCard &e : s_encards) {
        if (!e.box) continue;
        lv_obj_clean(e.box);
        fill_energy_card(e.box, e.card, e.tipo, e.w, e.h);
    }
    bsp_display_unlock();
}

static void render_placeholder(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    std::string t = card_type(card);
    const char *fase = "non ancora supportata";

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x2b2d31), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x44474d), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const char *title = jstr(card, "title");
    const char *ent1  = jstr(card, "entity");
    const char *nm    = jstr(card, "name");
    lv_obj_t *lh = mk_label(c, sanitize(title ? title : t.c_str()), &lv_font_montserrat_16, C_TEXT, w - 24);
    if (!title && ent1) bind(ent1, B_NAME, lh, NULL, NULL, NULL, nm ? sanitize(nm) : "");
    mk_label(c, t + " - " + fase, &lv_font_montserrat_14, C_TEXT2, w - 24);
}

static void render_heading(lv_obj_t *parent, const cJSON *card, int w)
{
    const char *txt = jstr(card, "heading");
    if (!txt) txt = jstr(card, "title");
    const char *style = jstr(card, "heading_style");
    bool sub = style && !strcmp(style, "subtitle");
    lv_obj_t *box = mk_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(box, sub ? 4 : 12, 0);
    lv_obj_set_style_pad_bottom(box, 4, 0);
    mk_label(box, sanitize(txt ? txt : ""), sub ? &lv_font_montserrat_16 : &lv_font_montserrat_24,
             sub ? C_TEXT2 : C_TEXT, w);
}

static void render_tile(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    const char *nm = jstr(card, "name");
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : ROW_H);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 10, 0);
    lv_obj_t *icon = mk_icon(c, 36);
    lv_obj_t *col = mk_box(c, w - 16 - 36 - 10, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    int tw = w - 16 - 36 - 10;
    lv_obj_t *ln = mk_label(col, "", &lv_font_montserrat_16, C_TEXT, tw);
    lv_obj_t *ls = mk_label(col, "", &lv_font_montserrat_14, C_TEXT2, tw);
    bind(eid, B_TILE, c, icon, ln, ls, nm ? sanitize(nm) : "");
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

static void render_button(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    const char *nm  = jstr(card, "name");
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 6, 0);
    if (!eid) {
        mk_label(c, sanitize(nm ? nm : "Pulsante"), &lv_font_montserrat_16, C_TEXT, 0);
        return;
    }
    lv_obj_t *icon = mk_icon(c, 48);
    const cJSON *sn = cJSON_GetObjectItem(card, "show_name");
    lv_obj_t *ln = NULL;
    if (!cJSON_IsFalse(sn)) {
        ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT, w - 24);
        lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *ls = NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItem(card, "show_state"))) {
        ls = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, w - 24);
        lv_obj_set_style_text_align(ls, LV_TEXT_ALIGN_CENTER, 0);
    }
    bind(eid, B_BUTTON, c, icon, ln, ls, nm ? sanitize(nm) : "");
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

static void render_entity(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    const char *nm = jstr(card, "name");
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);
    bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
    lv_obj_t *lv = mk_label(c, "", &lv_font_montserrat_32, C_TEXT, w - 28);
    bind(eid, B_BIGVALUE, lv, NULL, NULL, NULL, "");
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

static void switch_cb(lv_event_t *e)
{
    action_cb(e);
}

// Riga di una card "entities": nome a sinistra, stato o interruttore a destra.
static void render_entities_row(lv_obj_t *parent, const cJSON *row, int w)
{
    const char *eid = cJSON_IsString(row) ? row->valuestring : jstr(row, "entity");
    if (!eid) {
        const char *rt = jstr(row, "type");
        if (rt && !strcmp(rt, "section")) {
            const char *lbl = jstr(row, "label");
            mk_label(parent, sanitize(lbl ? lbl : ""), &lv_font_montserrat_16, C_TEXT2, w);
        } else if (rt && !strcmp(rt, "divider")) {
            lv_obj_t *d = mk_box(parent, w, 1);
            lv_obj_set_style_bg_color(d, lv_color_hex(0x3a3d42), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        }
        return;
    }
    const char *nm = cJSON_IsObject(row) ? jstr(row, "name") : NULL;
    lv_obj_t *r = mk_box(parent, w, 40);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_t *icon = mk_icon(r, 32);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_label_set_text(lv_obj_get_child(icon, 0), icon_symbol(eid).c_str());
    lv_obj_set_style_text_color(lv_obj_get_child(icon, 0), C_SENSOR, 0);

    std::string dom = domain_of(eid);
    bool as_switch = tap_service(eid) == "toggle" && dom != "climate" && dom != "media_player"
                     && dom != "cover";
    int right_w = as_switch ? 60 : (w - 42) / 2;
    lv_obj_t *ln = mk_label(r, "", &lv_font_montserrat_16, C_TEXT, w - 42 - right_w - 10);
    bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
    if (as_switch) {
        lv_obj_t *sw = lv_switch_create(r);
        lv_obj_set_size(sw, 54, 28);
        lv_obj_set_style_bg_color(sw, dom == "light" ? C_ON_LIGHT : C_ON,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        bind(eid, B_SWITCH, sw, NULL, NULL, NULL, "");
        s_actions.push_back({eid, dom, "toggle"});
        lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)(s_actions.size() - 1));
    } else {
        lv_obj_t *lv = mk_label(r, "", &lv_font_montserrat_16, C_TEXT, right_w);
        lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_RIGHT, 0);
        bind(eid, B_VALUE, lv, NULL, NULL, NULL, "");
        attach_action(r, eid, row, LV_EVENT_CLICKED);
    }
}

static void render_entities(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 4, 0);
    const char *title = jstr(card, "title");
    if (title) mk_label(c, sanitize(title), &lv_font_montserrat_22, C_TEXT, w - 28);
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(card, "entities"))
        render_entities_row(c, row, w - 28);
}

static void render_glance(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(c, 8, 0);
    const char *title = jstr(card, "title");
    if (title) mk_label(c, sanitize(title), &lv_font_montserrat_22, C_TEXT, w - 28);
    const cJSON *ents = cJSON_GetObjectItem(card, "entities");
    int n = cJSON_GetArraySize(ents);
    int per_row = LV_CLAMP(1, n, 5);
    const cJSON *cols = cJSON_GetObjectItem(card, "columns");
    if (cJSON_IsNumber(cols)) per_row = LV_CLAMP(1, cols->valueint, 8);
    int cw = (w - 28) / per_row;
    const cJSON *row;
    cJSON_ArrayForEach(row, ents) {
        const char *eid = cJSON_IsString(row) ? row->valuestring : jstr(row, "entity");
        if (!eid) continue;
        const char *nm = cJSON_IsObject(row) ? jstr(row, "name") : NULL;
        lv_obj_t *cell = mk_box(c, cw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *ln = mk_label(cell, "", &lv_font_montserrat_14, C_TEXT2, cw - 4);
        lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);
        bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
        lv_obj_t *lv = mk_label(cell, "", &lv_font_montserrat_16, C_TEXT, cw - 4);
        lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_CENTER, 0);
        bind(eid, B_VALUE, lv, NULL, NULL, NULL, "");
        attach_action(cell, eid, row, LV_EVENT_CLICKED);
    }
}

// ---- gauge ----

#define C_GREEN     lv_color_hex(0x43a047)
#define C_YELLOW    lv_color_hex(0xffa600)
#define C_RED       lv_color_hex(0xdb4437)
#define C_GAUGE_BG  lv_color_hex(0x3a3d42)
#define GAUGE_STEPS 1000        // la scala di lv_meter e' intera: 0..1000 = min..max

/* Colori delle card: "#rrggbb", "#rgb", "rgb(r,g,b)", le variabili del tema
   di HA (var(--success-color) ecc.) e i nomi piu' comuni. */
static bool parse_color(const char *s, lv_color_t *out)
{
    if (!s) return false;
    std::string c;
    for (const char *p = s; *p; p++) if (*p != ' ') c += (char)tolower((unsigned char)*p);
    if (c.empty()) return false;
    if (c[0] == '#') {
        unsigned v = 0;
        if (c.size() == 7 && sscanf(c.c_str() + 1, "%6x", &v) == 1) { *out = lv_color_hex(v); return true; }
        if (c.size() == 4 && sscanf(c.c_str() + 1, "%3x", &v) == 1) {
            unsigned r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            *out = lv_color_make(r * 17, g * 17, b * 17);
            return true;
        }
        return false;
    }
    int r, g, b;
    if (c.rfind("rgb", 0) == 0) {
        size_t o = c.find('(');
        if (o != std::string::npos && sscanf(c.c_str() + o + 1, "%d,%d,%d", &r, &g, &b) == 3) {
            *out = lv_color_make(r, g, b);
            return true;
        }
        return false;
    }
    if (c.find("success") != std::string::npos || c == "green")  { *out = C_GREEN;  return true; }
    if (c.find("warning") != std::string::npos || c == "yellow") { *out = C_YELLOW; return true; }
    if (c.find("error")   != std::string::npos || c == "red")    { *out = C_RED;    return true; }
    if (c == "orange")                                           { *out = lv_color_hex(0xff9800); return true; }
    if (c.find("info") != std::string::npos || c.find("primary") != std::string::npos ||
        c == "blue")                                             { *out = C_ON;     return true; }
    if (c == "grey" || c == "gray" || c.find("disabled") != std::string::npos)
                                                                 { *out = lv_color_hex(0x9e9e9e); return true; }
    if (c == "purple") { *out = lv_color_hex(0x9c27b0); return true; }
    if (c == "white")  { *out = lv_color_white(); return true; }
    return false;
}

static double jnum(const cJSON *o, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) { double d; if (state_number(v->valuestring, &d)) return d; }
    return def;
}

static int gauge_pos(const Gauge &g, double v)
{
    if (g.max <= g.min) return 0;
    double f = (v - g.min) / (g.max - g.min);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (int)(f * GAUGE_STEPS + 0.5);
}

// Colore del valore: quello della soglia piu' alta raggiunta, come in HA.
static lv_color_t gauge_color(const Gauge &g, double v)
{
    lv_color_t c = C_ON;
    for (auto &b : g.bands) if (v >= b.first) c = b.second;
    return c;
}

static void gauge_refresh(Gauge &g, const std::string &eid)
{
    Entity &e = ent(eid);
    double v = 0;
    bool num = state_number(e.state, &v);
    const std::string &unit = g.unit.empty() ? e.unit : g.unit;
    lv_label_set_text(g.l_value, (num ? format_number(v, e.state, unit) : format_state(eid)).c_str());
    int pos = num ? gauge_pos(g, v) : 0;
    if (g.value_arc) {
        lv_meter_set_indicator_end_value(g.meter, g.value_arc, pos);
        g.value_arc->type_data.arc.color = num ? gauge_color(g, v) : C_UNAVAIL;
        lv_obj_invalidate(g.meter);
    }
    if (g.needle_ind) lv_meter_set_indicator_value(g.meter, g.needle_ind, pos);
}

/* Card gauge di HA: semicerchio da min a max. Senza "needle" l'arco si riempie
   fino al valore col colore della fascia raggiunta; con "needle" le fasce sono
   disegnate sullo sfondo e una lancetta indica il valore. */
static void render_gauge(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    if (h <= 0) h = LV_MIN(w / 2 + 60, 240);

    Gauge g;
    g.min    = jnum(card, "min", 0);
    g.max    = jnum(card, "max", 100);
    g.needle = cJSON_IsTrue(cJSON_GetObjectItem(card, "needle"));
    const char *u = jstr(card, "unit");
    if (u) g.unit = sanitize(u);

    const cJSON *sev = cJSON_GetObjectItem(card, "severity");
    const cJSON *seg = cJSON_GetObjectItem(card, "segments");
    if (cJSON_IsArray(seg)) {
        const cJSON *sg;
        cJSON_ArrayForEach(sg, seg) {
            lv_color_t col;
            if (parse_color(jstr(sg, "color"), &col)) g.bands.push_back({jnum(sg, "from", g.min), col});
        }
    } else if (cJSON_IsObject(sev)) {
        static const char *keys[] = {"green", "yellow", "red"};
        static const lv_color_t cols[] = {C_GREEN, C_YELLOW, C_RED};
        for (int i = 0; i < 3; i++) {
            const cJSON *t = cJSON_GetObjectItem(sev, keys[i]);
            if (cJSON_IsNumber(t)) g.bands.push_back({t->valuedouble, cols[i]});
        }
    }
    std::sort(g.bands.begin(), g.bands.end(),
              [](const std::pair<double, lv_color_t> &a, const std::pair<double, lv_color_t> &b) {
                  return a.first < b.first; });

    lv_obj_t *c = mk_card(parent, w, h);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 2, 0);

    bool show_name = !cJSON_IsFalse(cJSON_GetObjectItem(card, "show_name"));
    int avail_h = h - 24 - (show_name ? 22 : 0);
    int d = LV_MIN(w - 24, avail_h * 2);
    const lv_font_t *vf = d >= 360 ? &lv_font_montserrat_40 : d >= 260 ? &lv_font_montserrat_32 :
                          d >= 180 ? &lv_font_montserrat_24 : &lv_font_montserrat_18;
    if (g.needle) {                       // il valore va sotto: la lancetta lo coprirebbe
        avail_h -= lv_font_get_line_height(vf);
        d = LV_MIN(w - 24, avail_h * 2);
    }
    d &= ~1;
    if (d < 40) d = 40;

    // Il meter e' un cerchio intero: il contenitore alto mezzo diametro ne ritaglia la meta' alta.
    lv_obj_t *box = mk_box(c, d, d / 2 + 2);
    lv_obj_t *m = lv_meter_create(box);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, d, d);
    lv_obj_set_pos(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    if (g.needle) {
        lv_obj_set_style_width(m, 14, LV_PART_INDICATOR);
        lv_obj_set_style_height(m, 14, LV_PART_INDICATOR);
        lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(m, C_TEXT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    lv_meter_scale_t *sc = lv_meter_add_scale(m);
    lv_meter_set_scale_ticks(m, sc, 0, 0, 0, C_TEXT);
    lv_meter_set_scale_range(m, sc, 0, GAUGE_STEPS, 180, 180);
    int aw = LV_MAX(10, d / 9);

    if (g.needle && !g.bands.empty()) {
        // fasce colorate sullo sfondo; sotto la prima soglia il colore normale
        int first = gauge_pos(g, g.bands[0].first);
        if (first > 0) {
            lv_meter_indicator_t *a = lv_meter_add_arc(m, sc, aw, C_ON, 0);
            lv_meter_set_indicator_start_value(m, a, 0);
            lv_meter_set_indicator_end_value(m, a, first);
        }
        for (size_t i = 0; i < g.bands.size(); i++) {
            int from = gauge_pos(g, g.bands[i].first);
            int to   = i + 1 < g.bands.size() ? gauge_pos(g, g.bands[i + 1].first) : GAUGE_STEPS;
            if (to <= from) continue;
            lv_meter_indicator_t *a = lv_meter_add_arc(m, sc, aw, g.bands[i].second, 0);
            lv_meter_set_indicator_start_value(m, a, from);
            lv_meter_set_indicator_end_value(m, a, to);
        }
    } else {
        lv_meter_indicator_t *bg = lv_meter_add_arc(m, sc, aw, C_GAUGE_BG, 0);
        lv_meter_set_indicator_start_value(m, bg, 0);
        lv_meter_set_indicator_end_value(m, bg, GAUGE_STEPS);
    }
    if (g.needle) {
        g.needle_ind = lv_meter_add_needle_line(m, sc, 4, C_TEXT, -aw / 2);
    } else {
        g.value_arc = lv_meter_add_arc(m, sc, aw, C_ON, 0);
        lv_meter_set_indicator_start_value(m, g.value_arc, 0);
        lv_meter_set_indicator_end_value(m, g.value_arc, 0);
    }
    g.meter = m;
    g.scale = sc;

    if (g.needle) {
        g.l_value = mk_label(c, "", vf, C_TEXT, 0);
    } else {
        g.l_value = mk_label(box, "", vf, C_TEXT, 0);
        lv_obj_align(g.l_value, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (show_name) {
        const char *nm = jstr(card, "name");
        lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 24);
        lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);
        bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
    }
    s_gauges.push_back(g);
    bind(eid, B_GAUGE, m, NULL, NULL, NULL, "", (int)s_gauges.size() - 1);
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

// ---- grafici storici (statistics-graph, history-graph) ----
//
// I dati arrivano da recorder/statistics_during_period: HA li ha gia' aggregati
// (media ogni 5 minuti, ogni ora...), quindi la risposta e' piccola anche per
// sensori che cambiano di continuo. Solo per le entita' senza statistiche
// (stati testuali, sensori senza state_class) la history-graph ripiega su
// history/history_during_period.
//
// Le richieste partono una alla volta da un task dedicato: il collegamento
// SDIO verso il C6 non ama le raffiche, e un recupero del trasporto in corso
// fa solo slittare l'aggiornamento (il grafico tiene gli ultimi dati).

#define CHART_MAX_POINTS 800
#define CHART_REFRESH_S  300

static const uint32_t SERIES_COLORS[] = {
    0x44739e, 0x984ea3, 0x00d2d5, 0xff7f00, 0xaf8d00, 0x7f80cd, 0xb3e900, 0xc42e60,
};

enum ChartKind { CK_STATS, CK_HISTORY, CK_NUMERO };   // CK_NUMERO = card "statistic"

struct ChartSeriesSpec { std::string eid, name; };

struct ChartSpec {
    const cJSON *node = NULL;                 // card nella vista (valida fino al prossimo set_config)
    ChartKind kind = CK_STATS;
    std::vector<ChartSeriesSpec> ents;
    std::string title, unit;
    std::vector<std::string> stat_types;      // richiesti a HA, in ordine di preferenza
    bool bar = false, legend = true;
    /* Solo per CK_NUMERO: la finestra di calendario che la card chiede.
       0 = nessuna (allora vale span_s), 1 giorno, 2 settimana, 3 mese, 4 anno.
       "arretra" e' quanti periodi indietro: -1 vuol dire "quello prima". */
    int cal = 0, arretra = 0;
    std::string period;                       // "5minute", "hour", "day", ...
    int period_s = 300;
    int64_t span_s = 86400;
    int npts = 288;
};

struct ChartSeriesData {
    bool has_data = false;
    bool numeric = true;
    std::vector<float> v;                                   // npts valori, NAN = buco
    std::vector<std::pair<int64_t, std::string>> timeline;  // (istante, stato) per gli stati testuali
};

struct ChartData {
    int64_t t0 = 0;                // istante del primo punto (s)
    int64_t fetched_s = 0;         // 0 = mai scaricato
    int64_t retry_at = 0;          // dopo un errore non riprovo prima di questo istante
    std::vector<ChartSeriesData> s;
    std::vector<std::string> need_history;   // history-graph: entita' senza statistiche
    std::string err;
};

struct ChartUI {
    int idx = -1;
    lv_obj_t *chart = NULL, *status = NULL, *timeline = NULL, *legend = NULL;
    std::vector<lv_chart_series_t *> ser;
    double ymin = 0, ymax = 1;
    int64_t t0 = 0, span = 86400;
    int w = 0;
};

static std::vector<ChartSpec> s_cspec;
static std::vector<ChartData> s_cdata;
static std::vector<ChartUI>   s_cui;

/* La card "statistic" non disegna un grafico ma un numero, quindi non entra
   in s_cui: ha un registro suo, aggiornato dagli stessi dati. */
struct NumeroUI {
    int idx = -1;
    lv_obj_t *valore = NULL;
    lv_obj_t *sotto  = NULL;
    std::string unita;
};
static std::vector<NumeroUI> s_num;
static uint32_t s_cgen = 0;              // cambia a ogni nuova configurazione
static TaskHandle_t s_fetch_task = NULL;

static int period_seconds(const std::string &p)
{
    if (p == "5minute") return 300;
    if (p == "hour")    return 3600;
    if (p == "day")     return 86400;
    if (p == "week")    return 7 * 86400;
    if (p == "month")   return 30 * 86400;
    return 300;
}

static const char *coarser_period(const std::string &p)
{
    if (p == "5minute") return "hour";
    if (p == "hour")    return "day";
    if (p == "day")     return "week";
    return "month";
}

static bool chart_card(const cJSON *card)
{
    std::string t = card_type(card);
    return t == "statistics-graph" || t == "history-graph" || t == "statistic";
}

static ChartSpec make_chart_spec(const cJSON *card)
{
    ChartSpec cs;
    cs.node = card;
    std::string t = card_type(card);
    cs.kind = t == "history-graph" ? CK_HISTORY : t == "statistic" ? CK_NUMERO : CK_STATS;
    const char *title = jstr(card, "title");
    if (title) cs.title = sanitize(title);
    const char *unit = jstr(card, "unit");
    if (unit) cs.unit = sanitize(unit);
    const char *ct = jstr(card, "chart_type");
    cs.bar = ct && !strcmp(ct, "bar");
    cs.legend = !cJSON_IsTrue(cJSON_GetObjectItem(card, "hide_legend"));

    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(card, "entities")) {
        const char *id = cJSON_IsString(e) ? e->valuestring : jstr(e, "entity");
        if (!id) continue;
        const char *nm = cJSON_IsObject(e) ? jstr(e, "name") : NULL;
        cs.ents.push_back({id, nm ? sanitize(nm) : ""});
        if (cs.ents.size() >= 8) break;
    }
    if (cs.ents.empty()) {                      // qualche card usa "entity" singolo
        const char *id = jstr(card, "entity");
        if (id) cs.ents.push_back({id, ""});
    }

    /* Finestra temporale: quella della card se indicata, altrimenti 24 ore a
       punti di 5 minuti (scelta per il pannello; HA di suo userebbe 30 giorni). */
    if (cs.kind == CK_HISTORY) {
        cs.span_s = (int64_t)(jnum(card, "hours_to_show", 24) * 3600);
        cs.period = "5minute";
    } else {
        const cJSON *dts = cJSON_GetObjectItem(card, "days_to_show");
        cs.span_s = (int64_t)((cJSON_IsNumber(dts) ? dts->valuedouble : 1) * 86400);
        const char *p = jstr(card, "period");
        if (p) cs.period = p;
        else cs.period = cs.span_s <= 2 * 86400 ? "5minute" : cs.span_s <= 35 * 86400 ? "hour" : "day";
    }
    if (cs.span_s < 3600) cs.span_s = 3600;
    cs.period_s = period_seconds(cs.period);
    while (cs.span_s / cs.period_s > CHART_MAX_POINTS && cs.period != "month") {
        cs.period = coarser_period(cs.period);
        cs.period_s = period_seconds(cs.period);
    }
    cs.npts = (int)LV_MAX(2, LV_MIN(CHART_MAX_POINTS, cs.span_s / cs.period_s));

    const cJSON *st;
    cJSON_ArrayForEach(st, cJSON_GetObjectItem(card, "stat_types"))
        if (cJSON_IsString(st)) cs.stat_types.push_back(st->valuestring);
    if (cs.stat_types.empty()) cs.stat_types = {"mean", "state"};

    /* La card "statistic" e' un caso a se': un solo numero, e la finestra la
       scrive in un modo tutto suo. Le tre forme che usa Home Assistant:

         period: {calendar: {period: month, offset: -1}}   il mese scorso
         period: {rolling_window: {duration: {hours: 24}}}  le ultime 24 ore
         period: {fixed_period: {start: ..., end: ...}}     due date precise

       Le prime due si servono; della terza si prende la durata e si tratta
       come una finestra che finisce adesso, che e' il meglio che si puo' fare
       senza portarsi dietro un calendario completo. */
    if (cs.kind == CK_NUMERO) {
        cs.ents.clear();
        const char *id = jstr(card, "entity");
        if (id) cs.ents.push_back({id, ""});
        const char *tipo = jstr(card, "stat_type");
        cs.stat_types.clear();
        cs.stat_types.push_back(tipo ? tipo : "change");
        cs.npts = 1;

        const cJSON *per = cJSON_GetObjectItem(card, "period");
        const cJSON *calendario = cJSON_GetObjectItem(per, "calendar");
        const cJSON *rotolo    = cJSON_GetObjectItem(per, "rolling_window");
        if (cJSON_IsObject(calendario)) {
            const char *q = jstr(calendario, "period");
            cs.cal = !q ? 1 : !strcmp(q, "week") ? 2 : !strcmp(q, "month") ? 3
                            : !strcmp(q, "year") ? 4 : 1;
            cs.arretra = (int)jnum(calendario, "offset", 0);
            cs.period  = cs.cal == 1 ? "day" : cs.cal == 2 ? "week"
                       : cs.cal == 3 ? "month" : "month";
        } else {
            const cJSON *d = cJSON_GetObjectItem(rotolo, "duration");
            double ore = jnum(d, "hours", 0) + jnum(d, "days", 0) * 24;
            cs.span_s = (int64_t)(ore > 0 ? ore * 3600 : 86400);
            cs.period = cs.span_s <= 86400 ? "hour" : "day";
            cs.cal = 0;
        }
        cs.period_s = period_seconds(cs.period);
        if (cs.cal) cs.span_s = cs.period_s;
    }
    return cs;
}

static void collect_charts(const cJSON *node)
{
    if (cJSON_IsArray(node)) {
        const cJSON *c;
        cJSON_ArrayForEach(c, node) collect_charts(c);
        return;
    }
    if (!cJSON_IsObject(node)) return;
    if (chart_card(node)) { s_cspec.push_back(make_chart_spec(node)); return; }
    collect_charts(cJSON_GetObjectItem(node, "cards"));
    collect_charts(cJSON_GetObjectItem(node, "card"));
    collect_charts(cJSON_GetObjectItem(node, "sections"));
}

static int64_t now_s(void) { return (int64_t)time(NULL); }
static bool clock_valid(void) { return now_s() > 1700000000; }   // SNTP gia' sincronizzato

static std::string iso_utc(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

// Istante di un record: HA recente manda millisecondi, versioni vecchie stringhe ISO.
static int64_t json_time_s(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        return (int64_t)(d > 1e11 ? d / 1000.0 : d);
    }
    if (cJSON_IsString(v)) {
        struct tm tm = {};
        int Y, M, D, h, m, s;
        if (sscanf(v->valuestring, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
            tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
            tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s;
            // timegm non c'e' in newlib: calcolo i giorni dall'epoca a mano
            int64_t days = 0;
            for (int y = 1970; y < Y; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
            static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            for (int i = 0; i < M - 1; i++) days += md[i] + (i == 1 && (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0)));
            days += D - 1;
            return days * 86400 + h * 3600 + m * 60 + s;
        }
    }
    return 0;
}

// ---- disegno ----

static std::string fmt_axis(double v)
{
    char b[24];
    double a = fabs(v);
    if (a >= 10000)     snprintf(b, sizeof(b), "%.0fk", v / 1000);
    else if (a >= 100)  snprintf(b, sizeof(b), "%.0f", v);
    else if (a >= 10)   snprintf(b, sizeof(b), "%.1f", v);
    else                snprintf(b, sizeof(b), "%.2f", v);
    // zeri finali inutili e virgola italiana
    std::string s = b;
    if (s.find('.') != std::string::npos && s.back() != 'k') {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    for (char &c : s) if (c == '.') c = ',';
    return s;
}

static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!dsc->text || dsc->part != LV_PART_TICKS) return;
    size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_cui.size()) return;
    const ChartUI &u = s_cui[i];
    if (dsc->id == LV_CHART_AXIS_PRIMARY_Y) {
        double v = u.ymin + (u.ymax - u.ymin) * dsc->value / 1000.0;
        lv_snprintf(dsc->text, dsc->text_length, "%s", fmt_axis(v).c_str());
    } else if (dsc->id == LV_CHART_AXIS_PRIMARY_X) {
        time_t t = (time_t)(u.t0 + u.span * dsc->value / 4);
        struct tm tm;
        localtime_r(&t, &tm);
        char b[16];
        strftime(b, sizeof(b), u.span <= 2 * 86400 ? "%H:%M" : "%d/%m", &tm);
        lv_snprintf(dsc->text, dsc->text_length, "%s", b);
    }
}

// Passo "tondo" (1, 2, 5 x 10^n) per la scala verticale.
static double nice_step(double range)
{
    double raw = range / 4;
    double mag = pow(10, floor(log10(raw)));
    double n = raw / mag;
    return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
}

static lv_color_t state_color(const std::string &eid, const std::string &st)
{
    if (st == "unavailable" || st == "unknown") return lv_color_hex(0x2a2c30);
    if (st == "off" || st == "closed" || st == "not_home" || st == "idle" || st == "locked")
        return lv_color_hex(0x4a4e55);
    if (st == "on" || st == "open" || st == "home" || st == "playing" || st == "unlocked")
        return domain_of(eid) == "light" ? C_ON_LIGHT : C_ON;
    uint32_t h = 5381;
    for (char c : st) h = h * 33 + (uint8_t)c;
    return lv_color_hex(SERIES_COLORS[h % 8]);
}

// Applica i dati scaricati ai widget del grafico i (lock del display preso).
static void chart_apply(size_t ui_i)
{
    ChartUI &u = s_cui[ui_i];
    if (u.idx < 0 || u.idx >= (int)s_cdata.size()) return;
    const ChartSpec &cs = s_cspec[u.idx];
    const ChartData &cd = s_cdata[u.idx];

    if (cd.fetched_s == 0) {
        lv_label_set_text(u.status, cd.err.empty() ? "Caricamento..." : cd.err.c_str());
        return;
    }
    u.t0 = cd.t0;
    u.span = (int64_t)cs.npts * cs.period_s;

    // scala verticale comune a tutte le serie numeriche
    double mn = INFINITY, mx = -INFINITY;
    bool any = false;
    for (auto &sd : cd.s) {
        if (!sd.has_data || !sd.numeric) continue;
        for (float v : sd.v) if (!std::isnan(v)) { mn = LV_MIN(mn, v); mx = LV_MAX(mx, v); any = true; }
    }
    if (any) {
        if (cs.bar && mn > 0) mn = 0;
        if (mx - mn < 1e-6) { mn -= 1; mx += 1; }
        double step = nice_step(mx - mn);
        u.ymin = floor(mn / step) * step;
        u.ymax = ceil(mx / step) * step;
        int ticks = (int)lround((u.ymax - u.ymin) / step) + 1;
        lv_chart_set_axis_tick(u.chart, LV_CHART_AXIS_PRIMARY_Y, 6, 0, LV_CLAMP(2, ticks, 7), 1, true, 60);
        lv_chart_set_div_line_count(u.chart, LV_CLAMP(2, ticks, 7), 0);
    }
    lv_chart_set_point_count(u.chart, cs.npts);
    for (size_t k = 0; k < u.ser.size() && k < cd.s.size(); k++) {
        const ChartSeriesData &sd = cd.s[k];
        lv_coord_t *ys = lv_chart_get_y_array(u.chart, u.ser[k]);
        for (int p = 0; p < cs.npts; p++) {
            float v = (sd.has_data && sd.numeric && p < (int)sd.v.size()) ? sd.v[p] : NAN;
            ys[p] = std::isnan(v) ? LV_CHART_POINT_NONE
                                  : (lv_coord_t)lround((v - u.ymin) * 1000.0 / (u.ymax - u.ymin));
        }
        lv_chart_set_x_start_point(u.chart, u.ser[k], 0);
    }
    lv_chart_refresh(u.chart);

    // strisce temporali per gli stati testuali
    if (u.timeline) {
        lv_obj_clean(u.timeline);
        int bw = lv_obj_get_content_width(u.timeline);
        int64_t t_end = u.t0 + u.span;
        /* Con la finestra sul giorno di calendario la fine e' a mezzanotte:
           l'ultimo stato noto va disegnato fino ad adesso, non fino a stanotte. */
        int64_t t_fill = LV_MIN(t_end, LV_MAX(u.t0, now_s()));
        for (size_t k = 0; k < cd.s.size(); k++) {
            const ChartSeriesData &sd = cd.s[k];
            if (!sd.has_data || sd.numeric) continue;
            lv_obj_t *row = mk_box(u.timeline, bw, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            const std::string &eid = cs.ents[k].eid;
            mk_label(row, sanitize(display_name(eid, cs.ents[k].name).c_str()),
                     &lv_font_montserrat_12, C_TEXT2, bw);
            lv_obj_t *bar = mk_box(row, bw, 20);
            lv_obj_set_style_radius(bar, 4, 0);
            lv_obj_set_style_clip_corner(bar, true, 0);
            const auto &tl = sd.timeline;
            for (size_t j = 0; j < tl.size() && j < 400; j++) {
                int64_t a = LV_MAX(tl[j].first, u.t0);
                int64_t b = j + 1 < tl.size() ? tl[j + 1].first : t_fill;
                if (b <= u.t0 || b <= a) continue;
                int x1 = (int)((a - u.t0) * bw / u.span), x2 = (int)((b - u.t0) * bw / u.span);
                if (x2 <= x1) continue;
                lv_obj_t *seg = mk_box(bar, x2 - x1, 20);
                lv_obj_set_pos(seg, x1, 0);
                lv_obj_set_style_bg_color(seg, state_color(eid, tl[j].second), 0);
                lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            }
        }
    }

    bool numeric_any = false;
    for (auto &sd : cd.s) if (sd.has_data && sd.numeric) numeric_any = true;
    if (!numeric_any) lv_obj_add_flag(u.chart, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_clear_flag(u.chart, LV_OBJ_FLAG_HIDDEN);

    bool none = true;
    for (auto &sd : cd.s) if (sd.has_data) none = false;
    if (none)                lv_label_set_text(u.status, "Nessun dato storico per queste entita'");
    else if (!cd.err.empty()) lv_label_set_text(u.status, cd.err.c_str());
    else                     lv_label_set_text(u.status, "");
}

static void numero_apply(NumeroUI &u);       // definita con gli altri aggiornamenti

static void render_statistic(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    int idx = -1;
    for (size_t i = 0; i < s_cspec.size(); i++) if (s_cspec[i].node == card) { idx = (int)i; break; }
    if (idx < 0 || s_cspec[idx].ents.empty()) { render_placeholder(parent, card, w, h); return; }

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const char *nm = jstr(card, "name");
    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);
    bind(s_cspec[idx].ents[0].eid.c_str(), B_NAME, ln, NULL, NULL, NULL,
         nm ? sanitize(nm) : "");

    NumeroUI u;
    u.idx = idx;
    u.valore = mk_label(c, "...", &lv_font_montserrat_32, C_TEXT, w - 28);
    u.sotto  = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, w - 28);
    const char *un = jstr(card, "unit");
    if (un) u.unita = sanitize(un);
    else {
        /* Senza unita' scritta nella card si prende quella dell'entita': e'
           quello che fa anche Home Assistant. */
        auto it = s_ent.find(s_cspec[idx].ents[0].eid);
        if (it != s_ent.end()) u.unita = it->second.unit;
    }
    s_num.push_back(u);
    numero_apply(s_num.back());
}

static void render_chart(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    int idx = -1;
    for (size_t i = 0; i < s_cspec.size(); i++) if (s_cspec[i].node == card) { idx = (int)i; break; }
    if (idx < 0 || s_cspec[idx].ents.empty()) { render_placeholder(parent, card, w, h); return; }
    const ChartSpec &cs = s_cspec[idx];

    int n_text = 0;                   // entita' testuali note: servono righe per le strisce
    for (auto &e : cs.ents) {
        double d;
        auto it = s_ent.find(e.eid);
        if (cs.kind == CK_HISTORY && it != s_ent.end() && !it->second.state.empty() &&
            !state_number(it->second.state, &d) && it->second.state != "unknown" &&
            it->second.state != "unavailable") n_text++;
    }
    if (h <= 0) h = 260 + n_text * 40;

    lv_obj_t *c = mk_card(parent, w, h);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    int iw = w - 24;

    // titolo e unita'
    std::string unit = cs.unit;
    if (unit.empty()) {
        auto it = s_ent.find(cs.ents[0].eid);
        if (it != s_ent.end()) unit = it->second.unit;
    }
    if (!cs.title.empty() || !unit.empty()) {
        lv_obj_t *hdr = mk_box(c, iw, LV_SIZE_CONTENT);
        if (!cs.title.empty()) mk_label(hdr, cs.title, &lv_font_montserrat_20, C_TEXT, iw - 70);
        if (!unit.empty()) {
            lv_obj_t *lu = mk_label(hdr, unit, &lv_font_montserrat_14, C_TEXT2, 0);
            lv_obj_align(lu, LV_ALIGN_TOP_RIGHT, 0, 2);
        }
    }

    // legenda
    lv_obj_t *legend = NULL;
    if (cs.legend && cs.ents.size() > 0) {
        legend = mk_box(c, iw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(legend, 14, 0);
        lv_obj_set_style_pad_row(legend, 2, 0);
        for (size_t k = 0; k < cs.ents.size(); k++) {
            lv_obj_t *it = mk_box(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(it, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(it, 6, 0);
            lv_obj_t *dot = mk_box(it, 10, 10);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(SERIES_COLORS[k % 8]), 0);
            lv_obj_t *ln = mk_label(it, "", &lv_font_montserrat_14, C_TEXT2, 0);
            bind(cs.ents[k].eid, B_NAME, ln, NULL, NULL, NULL, cs.ents[k].name);
        }
    }

    // area del grafico: a sinistra lo spazio per i valori, sotto per gli orari
    lv_obj_update_layout(c);
    int used = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(c); i++) used += lv_obj_get_height(lv_obj_get_child(c, i)) + 6;
    int tl_h = n_text * 40;
    int area_h = LV_MAX(80, h - 24 - used - tl_h);

    lv_obj_t *area = mk_box(c, iw, area_h);
    const int YL = 56, XL = 22;
    lv_obj_t *ch = lv_chart_create(area);
    lv_obj_set_size(ch, iw - YL - 22, area_h - XL);   // a destra spazio per l'ultimo orario
    lv_obj_set_pos(ch, YL, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(0x33373d), LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(ch, C_TEXT2, LV_PART_TICKS);
    lv_obj_set_style_text_font(ch, &lv_font_montserrat_12, LV_PART_TICKS);
    lv_obj_set_style_line_color(ch, lv_color_hex(0x4a4e55), LV_PART_TICKS);
    lv_obj_clear_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(ch, cs.bar ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_point_count(ch, cs.npts);
    lv_chart_set_div_line_count(ch, 5, 0);
    lv_chart_set_axis_tick(ch, LV_CHART_AXIS_PRIMARY_Y, 6, 0, 5, 1, true, 60);
    lv_chart_set_axis_tick(ch, LV_CHART_AXIS_PRIMARY_X, 6, 0, 5, 1, true, 30);
    if (cs.bar) lv_obj_set_style_pad_column(ch, 0, 0);

    ChartUI u;
    u.idx = idx;
    u.chart = ch;
    u.legend = legend;
    for (size_t k = 0; k < cs.ents.size(); k++) {
        lv_chart_series_t *se = lv_chart_add_series(ch, lv_color_hex(SERIES_COLORS[k % 8]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_value(ch, se, LV_CHART_POINT_NONE);
        u.ser.push_back(se);
    }
    u.status = mk_label(area, "", &lv_font_montserrat_14, C_TEXT2, 0);
    lv_obj_align(u.status, LV_ALIGN_CENTER, YL / 2, -XL / 2);

    if (cs.kind == CK_HISTORY) {
        u.timeline = mk_box(c, iw, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_left(u.timeline, YL, 0);
        lv_obj_set_style_pad_right(u.timeline, 22, 0);
        lv_obj_set_flex_flow(u.timeline, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(u.timeline, 4, 0);
    }
    u.w = iw;
    s_cui.push_back(u);
    size_t ui_i = s_cui.size() - 1;
    lv_obj_add_event_cb(ch, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, (void *)(intptr_t)ui_i);
    lv_obj_update_layout(c);
    chart_apply(ui_i);
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);   // magari i dati mancano ancora
}

// ---- recupero dati ----

struct FetchJob {
    uint32_t gen;
    int idx;
    bool history;                  // false = statistiche, true = storia grezza
    int64_t t0;
    std::vector<std::string> ents; // entita' richieste (solo per history)
};

static volatile bool    s_fetch_busy = false;
static volatile int64_t s_fetch_since = 0;

// Ricalcola la griglia: slot k = [t0 + k*periodo, t0 + (k+1)*periodo).
/* Mezzanotte locale del giorno che contiene t. */
static int64_t mezzanotte_di(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t m = mktime(&tm);
    return m == (time_t)-1 ? t : (int64_t)m;
}

/* L'inizio del giorno, della settimana, del mese o dell'anno in corso,
   eventualmente arretrato di qualche periodo. Sul calendario vero, non a
   multipli di 86400: "questo mese" comincia il primo del mese anche quando il
   mese scorso aveva 28 giorni. */
static void calendario_bounds(int cal, int arretra, int64_t *t0, int64_t *t1)
{
    int64_t oggi = mezzanotte_di(now_s());
    time_t tt = (time_t)oggi;
    struct tm tm;
    localtime_r(&tt, &tm);

    if (cal == 1) {                                   // giorno
        *t0 = oggi + (int64_t)arretra * 86400;
        *t1 = *t0 + 86400;
        return;
    }
    if (cal == 2) {                                   // settimana, da lunedi'
        int dow = (tm.tm_wday + 6) % 7;
        *t0 = oggi - (int64_t)dow * 86400 + (int64_t)arretra * 7 * 86400;
        *t1 = *t0 + 7 * 86400;
        return;
    }
    struct tm a = tm;
    a.tm_mday = 1;
    a.tm_hour = a.tm_min = a.tm_sec = 0;
    if (cal == 4) a.tm_mon = 0;
    a.tm_isdst = -1;
    struct tm b = a;
    if (cal == 3) { a.tm_mon += arretra; b.tm_mon = a.tm_mon + 1; }
    else          { a.tm_year += arretra; b.tm_year = a.tm_year + 1; b.tm_mon = 0; }
    b.tm_isdst = -1;
    *t0 = (int64_t)mktime(&a);
    *t1 = (int64_t)mktime(&b);
}

static void grid_bounds(const ChartSpec &cs, int64_t *t0)
{
    if (cs.kind == CK_NUMERO && cs.cal) {
        int64_t t1;
        calendario_bounds(cs.cal, cs.arretra, t0, &t1);
        return;
    }
    int64_t now = now_s();
    /* Finestre di giorni interi: il grafico copre il giorno di calendario
       (dalle 00:00 alle 23:59 locali), non le ultime 24 ore che scorrono
       all'indietro. Cosi' alle 21:25 si legge "oggi", non "da ieri alle
       21:25": la parte ancora da venire resta semplicemente vuota. */
    int64_t span = (int64_t)cs.npts * cs.period_s;
    if (span % 86400 == 0 && cs.period_s <= 86400) {
        time_t t = (time_t)now;
        struct tm tm;
        localtime_r(&t, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_isdst = -1;                       // lascio decidere all'ora legale
        time_t midnight = mktime(&tm);
        if (midnight != (time_t)-1) {
            *t0 = (int64_t)midnight - (span - 86400);   // piu' giorni: finisce stasera
            return;
        }
    }
    int64_t end = (now / cs.period_s + 1) * cs.period_s;     // fine dello slot corrente
    *t0 = end - span;
}

static void fetch_done(void)
{
    s_fetch_busy = false;
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}

/* Il numero della card "statistic". Un solo valore, quello della sola fascia
   che abbiamo chiesto. */
static void numero_apply(NumeroUI &u)
{
    if (u.idx < 0 || u.idx >= (int)s_cdata.size()) return;
    const ChartSpec &cs = s_cspec[u.idx];
    const ChartData &cd = s_cdata[u.idx];

    if (cd.fetched_s == 0) {
        lv_label_set_text(u.valore, cd.err.empty() ? "..." : "--");
        if (u.sotto && !cd.err.empty()) lv_label_set_text(u.sotto, cd.err.c_str());
        return;
    }
    float v = NAN;
    if (!cd.s.empty() && cd.s[0].has_data && !cd.s[0].v.empty()) v = cd.s[0].v[0];

    char b[48];
    if (std::isnan(v)) {
        /* Nessun dato non e' zero: un contatore che segna 0 e uno che non ha
           ancora niente da dire sono due cose diverse, e mostrarle uguali
           farebbe credere a un consumo nullo. */
        snprintf(b, sizeof(b), "--");
    } else {
        double a = fabs(v);
        if (a < 10)       snprintf(b, sizeof(b), "%.2f", v);
        else if (a < 100) snprintf(b, sizeof(b), "%.1f", v);
        else              snprintf(b, sizeof(b), "%.0f", v);
    }
    std::string t = b;
    if (!std::isnan(v) && !u.unita.empty()) t += " " + u.unita;
    lv_label_set_text(u.valore, t.c_str());
    if (u.sotto) {
        const char *q = cs.stat_types.empty() ? "" : cs.stat_types[0].c_str();
        const char *nome = !strcmp(q, "change") ? "variazione" : !strcmp(q, "mean") ? "media"
                         : !strcmp(q, "min") ? "minimo" : !strcmp(q, "max") ? "massimo"
                         : !strcmp(q, "sum") ? "totale" : q;
        const char *quando = cs.cal == 1 ? "oggi" : cs.cal == 2 ? "questa settimana"
                           : cs.cal == 3 ? "questo mese" : cs.cal == 4 ? "quest'anno" : "";
        char sotto[80];
        if (cs.cal && cs.arretra) snprintf(sotto, sizeof(sotto), "%s, periodo precedente", nome);
        else                      snprintf(sotto, sizeof(sotto), "%s %s", nome, quando);
        lv_label_set_text(u.sotto, sotto);
    }
}

static void apply_to_ui(int idx)
{
    for (size_t i = 0; i < s_cui.size(); i++) if (s_cui[i].idx == idx) chart_apply(i);
    for (NumeroUI &u : s_num) if (u.idx == idx) numero_apply(u);
}

/* I dati dei grafici si masticano FUORI dal lock del display.

   Prima questa funzione prendeva bsp_display_lock() all'inizio e lo restituiva
   alla fine: in mezzo ci stava tutta la lettura del JSON, serie per serie e
   riga per riga. Finche' quel lock e' preso LVGL non puo' disegnare, quindi il
   pannello si impuntava esattamente quando arrivavano i dati - e tanto piu' a
   lungo quanti piu' dati erano.

   Adesso il lock si prende due volte e per un attimo: la prima per copiarsi
   cio' che serve, la seconda per consegnare il risultato. In mezzo il display
   e' libero.

   Fra le due prese la vista puo' essere stata ricostruita. Per questo la
   generazione si ricontrolla anche alla consegna: se e' cambiata, questi dati
   riguardano una vista che non esiste piu' e si buttano. Due richieste non si
   sovrappongono mai (ci pensa s_fetch_busy), quindi nessun altro puo' aver
   scritto la stessa casella nel frattempo. */
static void on_stats_result(bool ok, cJSON *result, const char *error, void *ctx)
{
    FetchJob *job = (FetchJob *)ctx;

    // --- 1. copia di lavoro, col lock preso il minimo indispensabile ------
    ChartSpec cs;
    std::vector<ChartSeriesData> vecchie;
    bool valido = false;
    bsp_display_lock(0);
    if (job->gen == s_cgen && job->idx < (int)s_cdata.size()) {
        cs = s_cspec[job->idx];
        vecchie = s_cdata[job->idx].s;
        valido = true;
    }
    bsp_display_unlock();
    if (!valido) { delete job; fetch_done(); return; }

    // --- 2. il lavoro vero, con il display libero di disegnare ------------
    ChartData nd;
    std::string errore;
    if (!ok) {
        errore = std::string("Errore dati: ") + (error ? error : "?");
        ESP_LOGW(TAG, "grafico %d: %s", job->idx, errore.c_str());
    } else {
        {
            nd.t0 = job->t0;
            nd.s.resize(cs.ents.size());
            for (size_t k = 0; k < cs.ents.size(); k++) {
                ChartSeriesData &sd = nd.s[k];
                const cJSON *rows = cJSON_GetObjectItem(result, cs.ents[k].eid.c_str());
                if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0) {
                    if (cs.kind == CK_HISTORY) {
                        nd.need_history.push_back(cs.ents[k].eid);
                        if (k < vecchie.size()) sd = vecchie[k];   // tengo i vecchi dati finche' arriva la storia
                    }
                    continue;
                }
                // tipo di statistica: il primo richiesto che compare nei dati
                const cJSON *first = cJSON_GetArrayItem(rows, 0);
                std::string key;
                for (auto &st : cs.stat_types)
                    if (cJSON_IsNumber(cJSON_GetObjectItem(first, st.c_str()))) { key = st; break; }
                if (key.empty()) {
                    for (const char *k2 : {"mean", "state", "sum", "change", "max", "min"})
                        if (cJSON_IsNumber(cJSON_GetObjectItem(first, k2))) { key = k2; break; }
                }
                if (key.empty()) continue;
                sd.v.assign(cs.npts, NAN);
                const cJSON *r;
                cJSON_ArrayForEach(r, rows) {
                    const cJSON *val = cJSON_GetObjectItem(r, key.c_str());
                    if (!cJSON_IsNumber(val)) continue;
                    int64_t t = json_time_s(cJSON_GetObjectItem(r, "start"));
                    // arrotondo: i periodi giornalieri di HA partono dalla mezzanotte locale
                    int slot = (int)floor((double)(t - job->t0 + cs.period_s / 2) / cs.period_s);
                    if (slot >= 0 && slot < cs.npts) sd.v[slot] = (float)val->valuedouble;
                }
                sd.has_data = true;
            }
            nd.fetched_s = now_s();
        }
    }

    // --- 3. consegna ------------------------------------------------------
    bsp_display_lock(0);
    if (job->gen == s_cgen && job->idx < (int)s_cdata.size()) {
        ChartData &cd = s_cdata[job->idx];
        if (!ok) {
            cd.err = errore;
            cd.retry_at = now_s() + 60;
        } else {
            cd = std::move(nd);
            ESP_LOGI(TAG, "grafico %d: statistiche ricevute (%u senza statistiche)",
                     job->idx, (unsigned)cd.need_history.size());
        }
        apply_to_ui(job->idx);
    }
    bsp_display_unlock();
    delete job;
    fetch_done();
}

/* Come sopra: il lock si prende solo per prendere e per consegnare. Qui in
   piu' si smette di copiare il testo di ogni riga di storia.

   Prima ogni riga faceva nascere una std::string. Per una serie numerica -
   cioe' quasi tutte - quelle stringhe servivano soltanto a essere riconvertite
   in numero poche righe dopo, e poi venivano buttate: centinaia di allocazioni
   nella RAM interna, che e' la poca, per niente. Adesso il numero si ricava
   subito e del testo si tiene solo il puntatore dentro il JSON, che resta
   valido per tutta la chiamata. Le stringhe vere si costruiscono solo per le
   serie testuali, che sono quelle che devono davvero mostrarle. */
static void on_history_result(bool ok, cJSON *result, const char *error, void *ctx)
{
    FetchJob *job = (FetchJob *)ctx;

    // --- 1. copia di lavoro ----------------------------------------------
    ChartSpec cs;
    std::vector<ChartSeriesData> serie;
    bool valido = false;
    bsp_display_lock(0);
    if (job->gen == s_cgen && job->idx < (int)s_cdata.size()) {
        cs = s_cspec[job->idx];
        serie = s_cdata[job->idx].s;
        valido = true;
    }
    bsp_display_unlock();
    if (!valido) { delete job; fetch_done(); return; }
    /* La storia arriva sempre dopo le statistiche, che dimensionano le serie.
       Ma costa una riga assicurarsene, invece di fidarsi dell'ordine. */
    serie.resize(cs.ents.size());

    // --- 2. il lavoro vero, con il display libero -------------------------
    std::string errore;
    if (!ok) {
        errore = std::string("Errore storia: ") + (error ? error : "?");
    } else {
        struct Riga { int64_t t; const char *st; double num; };
        std::vector<Riga> righe;
        for (size_t k = 0; k < cs.ents.size(); k++) {
            const cJSON *rows = cJSON_GetObjectItem(result, cs.ents[k].eid.c_str());
            if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0) continue;
            ChartSeriesData &sd = serie[k];
            sd.timeline.clear();

            righe.clear();
            righe.reserve(cJSON_GetArraySize(rows));
            bool numeric = true;
            const cJSON *r;
            cJSON_ArrayForEach(r, rows) {
                const char *st = jstr(r, "s");
                if (!st) continue;
                const cJSON *tj = cJSON_GetObjectItem(r, "lu");
                if (!tj) tj = cJSON_GetObjectItem(r, "lc");
                double d;
                bool num_ok = state_number(st, &d);
                if (!num_ok && strcmp(st, "unavailable") && strcmp(st, "unknown"))
                    numeric = false;
                righe.push_back({json_time_s(tj), st, num_ok ? d : NAN});
            }
            sd.numeric = numeric;
            if (numeric) {
                // gradino: ogni slot prende l'ultimo valore noto a fine slot
                sd.v.assign(cs.npts, NAN);
                size_t j = 0;
                double cur = NAN;
                for (int p = 0; p < cs.npts; p++) {
                    int64_t te = job->t0 + (int64_t)(p + 1) * cs.period_s;
                    while (j < righe.size() && righe[j].t < te) { cur = righe[j].num; j++; }
                    if (righe.empty() || righe[0].t >= te) continue;
                    sd.v[p] = (float)cur;
                }
            } else {
                // solo qui il testo si copia davvero: serve per mostrarlo
                sd.timeline.reserve(righe.size());
                for (const Riga &x : righe) sd.timeline.push_back({x.t, x.st});
            }
            sd.has_data = true;
        }
    }

    // --- 3. consegna ------------------------------------------------------
    bsp_display_lock(0);
    if (job->gen == s_cgen && job->idx < (int)s_cdata.size()) {
        ChartData &cd = s_cdata[job->idx];
        cd.need_history.clear();
        if (!ok) {
            cd.err = errore;
            cd.retry_at = now_s() + 60;
        } else {
            cd.s = std::move(serie);
            ESP_LOGI(TAG, "grafico %d: storia ricevuta", job->idx);
        }
        apply_to_ui(job->idx);
    }
    bsp_display_unlock();
    delete job;
    fetch_done();
}

// Sceglie il prossimo grafico da aggiornare e prepara la richiesta (lock preso).
static FetchJob *next_job(std::string &body)
{
    int64_t now = now_s();
    for (size_t i = 0; i < s_cspec.size(); i++) {
        const ChartSpec &cs = s_cspec[i];
        ChartData &cd = s_cdata[i];
        if (!cd.need_history.empty()) {
            FetchJob *job = new FetchJob{s_cgen, (int)i, true, cd.t0, cd.need_history};
            std::string ids;
            for (size_t k = 0; k < job->ents.size(); k++) ids += (k ? ",\"" : "\"") + job->ents[k] + "\"";
            body = "\"type\":\"history/history_during_period\",\"start_time\":\"" + iso_utc(cd.t0) +
                   "\",\"entity_ids\":[" + ids + "],\"minimal_response\":true,\"no_attributes\":true,"
                   "\"significant_changes_only\":false";
            return job;
        }
        int refresh = LV_MAX(CHART_REFRESH_S, cs.period_s >= 86400 ? 1800 : CHART_REFRESH_S);
        if (cd.fetched_s != 0 && now - cd.fetched_s < refresh) continue;
        if (now < cd.retry_at) continue;
        int64_t t0;
        grid_bounds(cs, &t0);
        FetchJob *job = new FetchJob{s_cgen, (int)i, false, t0, {}};
        std::string ids, types;
        for (size_t k = 0; k < cs.ents.size(); k++) ids += (k ? ",\"" : "\"") + cs.ents[k].eid + "\"";
        for (size_t k = 0; k < cs.stat_types.size(); k++) types += (k ? ",\"" : "\"") + cs.stat_types[k] + "\"";
        body = "\"type\":\"recorder/statistics_during_period\",\"start_time\":\"" + iso_utc(t0) +
               "\",\"period\":\"" + cs.period + "\",\"statistic_ids\":[" + ids + "],\"types\":[" + types + "]";
        return job;
    }
    return NULL;
}

static void fetch_task(void *arg)
{
    /* Subito dopo una connessione arrivano dashboard e stati: sommarci i
       grafici e' proprio la raffica che il trasporto SDIO verso il C6 tollera
       peggio (i timeout si concentrano nei primi istanti di ogni connessione). */
    const int64_t SETTLE_US = 15LL * 1000000;
    int64_t conn_since = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        if (!ha_ws_connected()) { conn_since = 0; continue; }
        if (conn_since == 0) conn_since = esp_timer_get_time();
        if (esp_timer_get_time() - conn_since < SETTLE_US) continue;
        if (s_fetch_busy) {
            if (esp_timer_get_time() - s_fetch_since < 60LL * 1000000) continue;
            ESP_LOGW(TAG, "risposta ai dati del grafico mai arrivata: riprovo");
            s_fetch_busy = false;
        }
        if (!clock_valid()) {
            bsp_display_lock(0);
            for (auto &cd : s_cdata) if (cd.fetched_s == 0) cd.err = "In attesa dell'ora (SNTP)...";
            for (size_t i = 0; i < s_cui.size(); i++) chart_apply(i);
            bsp_display_unlock();
            continue;
        }
        std::string body;
        bsp_display_lock(0);
        FetchJob *job = next_job(body);
        bsp_display_unlock();
        if (!job) {
            /* Niente grafici da aggiornare: e' il turno dell'energia. Passa da
               qui e non da un filo suo apposta, perche' due richieste insieme
               sono proprio la raffica che il collegamento SDIO verso il C6
               regge peggio. L'energia manda la sua e si mette in attesa da
               sola: qui non si aspetta nulla. */
            std::string en;
            energy_model_prossima_richiesta(en);
            continue;
        }
        s_fetch_busy = true;
        s_fetch_since = esp_timer_get_time();
        int id = ha_ws_request(body.c_str(), job->history ? on_history_result : on_stats_result, job);
        if (id < 0) {
            delete job;
            s_fetch_busy = false;
        } else {
            ESP_LOGI(TAG, "grafico %d: richiesta %s (%u byte)", job->idx,
                     job->history ? "storia" : "statistiche", (unsigned)body.size());
        }
    }
}

void ll_charts_refresh(void)
{
    bsp_display_lock(0);
    for (auto &cd : s_cdata) { cd.fetched_s = 0; cd.retry_at = 0; }
    bsp_display_unlock();
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}

void ll_charts_start(void)
{
    if (!s_fetch_task) xTaskCreate(fetch_task, "ll_fetch", 6144, NULL, 3, &s_fetch_task);
}

static void render_stack(lv_obj_t *parent, const cJSON *card, int w, bool horizontal, int columns)
{
    const cJSON *cards = cJSON_GetObjectItem(card, "cards");
    int n = cJSON_GetArraySize(cards);
    if (n == 0) return;
    lv_obj_t *box = mk_box(parent, w, LV_SIZE_CONTENT);
    const char *title = jstr(card, "title");
    lv_obj_set_flex_flow(box, horizontal || columns > 1 ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, GAP, 0);
    lv_obj_set_style_pad_column(box, GAP, 0);
    if (title) mk_label(box, sanitize(title), &lv_font_montserrat_22, C_TEXT, w);
    int per_row = horizontal ? n : (columns > 1 ? columns : 1);
    int cw = (w - GAP * (per_row - 1)) / per_row;
    const cJSON *c;
    cJSON_ArrayForEach(c, cards) render_card(box, c, cw, card_rows_height(c));
}


// ---- markdown ----
//
// Il testo di queste card non e' testo: e' un modello Jinja che Home
// Assistant espande con i valori delle entita'. Espanderlo qui vorrebbe dire
// portarsi dentro un interprete di modelli, fuori questione su un pannello.
// HA pero' sa farlo per noi e continua a rifarlo da solo quando i valori
// cambiano: basta iscriversi con "render_template".
//
// Del markdown si tiene il poco che serve su uno schermo appeso al muro: via
// i cancelletti dei titoli e i segni del grassetto, i trattini degli elenchi
// diventano pallini. Tabelle e collegamenti non avrebbero senso qui.

struct MdCard { lv_obj_t *label; int sub; uint32_t gen; };
static std::vector<MdCard *> s_md;


static std::string md_plain(const std::string &in)
{
    std::string out;
    bool bol = true;                       // siamo a inizio riga
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (bol) {
            size_t j = i;
            while (j < in.size() && in[j] == '#') j++;
            if (j > i && j < in.size() && in[j] == ' ') { i = j; continue; }
            if ((c == '-' || c == '*' || c == '+') && i + 1 < in.size() && in[i + 1] == ' ') {
                /* I font montserrat di LVGL sono compilati con il grado e
                   il pallino oltre ai caratteri latini, quindi il pallino si
                   puo' usare davvero. */
                out += "• ";
                i++;
                bol = false;
                continue;
            }
        }
        if (c == '*' || c == '_' || c == '`') {
            if ((c == '*' || c == '_') && i + 1 < in.size() && in[i + 1] == c) i++;
            continue;
        }
        out += c;
        bol = (c == '\n');
    }
    return out;
}

static void md_update(const cJSON *ev, void *ctx)
{
    MdCard *m = (MdCard *)ctx;
    const cJSON *res = cJSON_GetObjectItem(ev, "result");
    if (!cJSON_IsString(res)) return;
    std::string txt = md_plain(res->valuestring);
    /* Arriva nel task del WebSocket, quindi il lock del display lo prendiamo
       qui. La generazione dice se la card esiste ancora: una risposta puo'
       arrivare mentre la vista viene ricostruita. */
    bsp_display_lock(0);
    if (m->gen == s_cgen && m->label) lv_label_set_text(m->label, txt.c_str());
    bsp_display_unlock();
}

static void render_markdown(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    const char *title = jstr(card, "title");
    if (title) mk_label(c, sanitize(title), &lv_font_montserrat_16, C_TEXT2, w - 20);

    lv_obj_t *l = mk_label(c, "...", &lv_font_montserrat_16, C_TEXT, w - 20);
    /* Le altre card hanno una riga sola e la troncano con i puntini; qui il
       testo e' il contenuto della card e deve andare a capo, con la card che
       cresce quanto serve. */
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    const char *content = jstr(card, "content");
    if (!content) return;

    MdCard *m = new MdCard{ l, -1, s_cgen };
    s_md.push_back(m);
    std::string body = "\"type\":\"render_template\",\"template\":" +
                       json_escape(content) + ",\"report_errors\":false";
    m->sub = ha_ws_subscribe(body.c_str(), md_update, m);
    if (m->sub < 0) lv_label_set_text(l, "Troppe card di testo insieme.");
}

// ---- meteo (weather-forecast) ----
//
// Le previsioni non stanno negli attributi dell'entita': Home Assistant le
// tiene da parte e le manda solo a chi si iscrive con
// "weather/subscribe_forecast". Lo dice la sua documentazione, ed e' il
// motivo per cui il pannello ha imparato a tenere aperte le sottoscrizioni.
//
// Della card si disegnano due parti: sopra la condizione di adesso, che
// arriva dallo stato dell'entita' come ogni altra cosa, e sotto le righe
// delle previsioni, che arrivano dalla sottoscrizione.

#define WX_RIGHE 5

struct WxCard {
    lv_obj_t *righe[WX_RIGHE];      // contenitore di ogni riga
    lv_obj_t *quando[WX_RIGHE];
    lv_obj_t *icona[WX_RIGHE];
    lv_obj_t *gradi[WX_RIGHE];
    int  sub;
    uint32_t gen;
    bool orarie;                    // previsioni a ore invece che a giorni
};
static std::vector<WxCard *> s_wx;

/* "2026-09-25T06:32:00+00:00" -> "ven" oppure "14:00". Le previsioni orarie
   vogliono l'ora, quelle giornaliere il giorno della settimana. */
static std::string wx_quando(const char *iso, bool orarie)
{
    struct tm d;
    if (!iso_to_local(iso, &d, NULL)) return "";
    char b[16];
    if (orarie) snprintf(b, sizeof(b), "%02d:%02d", d.tm_hour, d.tm_min);
    else        snprintf(b, sizeof(b), "%s", GIORNI_BREVI[d.tm_wday]);
    return b;
}

static void wx_update(const cJSON *ev, void *ctx)
{
    WxCard *c = (WxCard *)ctx;
    const cJSON *arr = cJSON_GetObjectItem(ev, "forecast");
    if (!cJSON_IsArray(arr)) return;

    bsp_display_lock(0);
    if (c->gen == s_cgen) {
        int i = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, arr) {
            if (i >= WX_RIGHE) break;
            const char *quando = jstr(f, "datetime");
            const char *cond   = jstr(f, "condition");
            const cJSON *tmax  = cJSON_GetObjectItem(f, "temperature");
            const cJSON *tmin  = cJSON_GetObjectItem(f, "templow");

            lv_obj_clear_flag(c->righe[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(c->quando[i], wx_quando(quando, c->orarie).c_str());
            lv_label_set_text(c->icona[i], wx_icona(cond).c_str());
            char g[32];
            if (cJSON_IsNumber(tmin))
                snprintf(g, sizeof(g), "%d / %d", (int)lround(tmax ? tmax->valuedouble : 0),
                         (int)lround(tmin->valuedouble));
            else if (cJSON_IsNumber(tmax))
                snprintf(g, sizeof(g), "%d", (int)lround(tmax->valuedouble));
            else
                snprintf(g, sizeof(g), "--");
            lv_label_set_text(c->gradi[i], g);
            i++;
        }
        for (; i < WX_RIGHE; i++) lv_obj_add_flag(c->righe[i], LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

static void render_weather(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    if (!eid) {
        mk_label(c, "Card meteo senza entita'", &lv_font_montserrat_16, C_TEXT2, w - 20);
        return;
    }

    /* Riga di sopra: icona grande, temperatura, nome e condizione. */
    lv_obj_t *top = mk_box(c, w - 20, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 12, 0);

    lv_obj_t *big = lv_label_create(top);
    lv_obj_set_style_text_font(big, mdi_icon_font(32), 0);
    lv_obj_set_style_text_color(big, C_TEXT, 0);
    lv_label_set_text(big, wx_icona(NULL).c_str());

    lv_obj_t *col = mk_box(top, w - 20 - 44 - 12, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *l_temp = mk_label(col, "--", &lv_font_montserrat_26, C_TEXT, 0);
    lv_obj_t *l_cond = mk_label(col, "", &lv_font_montserrat_14, C_TEXT2, w - 80);

    /* Righe delle previsioni, create vuote e riempite dalla sottoscrizione. */
    WxCard *wc = new WxCard{};
    wc->gen = s_cgen;
    wc->sub = -1;
    const char *tipo = jstr(card, "forecast_type");
    wc->orarie = tipo && !strcmp(tipo, "hourly");
    for (int i = 0; i < WX_RIGHE; i++) {
        lv_obj_t *r = mk_box(c, w - 20, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        wc->righe[i]  = r;
        wc->quando[i] = mk_label(r, "", &lv_font_montserrat_14, C_TEXT2, 0);
        wc->icona[i]  = lv_label_create(r);
        lv_obj_set_style_text_font(wc->icona[i], mdi_icon_font(20), 0);
        lv_obj_set_style_text_color(wc->icona[i], C_TEXT, 0);
        lv_label_set_text(wc->icona[i], "");
        wc->gradi[i]  = mk_label(r, "", &lv_font_montserrat_14, C_TEXT, 0);
    }
    s_wx.push_back(wc);

    std::string body = std::string("\"type\":\"weather/subscribe_forecast\",\"entity_id\":") +
                       json_escape(eid) + ",\"forecast_type\":\"" +
                       (wc->orarie ? "hourly" : "daily") + "\"";
    wc->sub = ha_ws_subscribe(body.c_str(), wx_update, wc);

    bind(eid, B_WEATHER, c, big, l_cond, l_temp, "");
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

// ---- card che comandano (light, thermostat, media-control) ----
//
// Fin qui il pannello mostrava: il tocco su una card faceva al massimo un
// "toggle". Queste tre invece regolano: quanto accendere la luce, a che
// temperatura tenere la casa, cosa suonare e quanto forte.
//
// Due accortezze valgono per tutte e tre. La prima: il comando parte quando
// si lascia il dito, non mentre lo si trascina, se no ogni pixel diventerebbe
// una chiamata a Home Assistant. La seconda: mentre il dito e' giu' gli
// aggiornamenti che arrivano da HA non toccano il cursore, altrimenti la
// vecchia posizione tornerebbe sotto le dita di chi sta regolando.

/* Il tasto deve sapere su quale entita' agire. All'evento posso passare solo
   un numero, non un puntatore a una std::string che sta in un vettore che
   cresce, quindi l'identificativo lo tengo qui e passo la riga. */
static std::vector<std::string> s_cmd_eid;

static int cmd_slot(const std::string &eid)
{
    s_cmd_eid.push_back(eid);
    return (int)s_cmd_eid.size() - 1;
}

static const std::string *cmd_eid(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= (int)s_cmd_eid.size()) return NULL;
    return &s_cmd_eid[i];
}

static bool in_mano(lv_obj_t *o)          // il dito e' ancora sul cursore
{
    return o && (lv_obj_get_state(o) & LV_STATE_PRESSED);
}

/* Tasto tondo con dentro un'icona: e' il mattone di tutte e tre le card. */
static lv_obj_t *mk_tasto(lv_obj_t *parent, int size, const char *icona,
                          lv_color_t sfondo, lv_event_cb_t cb, int slot)
{
    lv_obj_t *b = mk_box(parent, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, sfondo, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3f45), LV_STATE_PRESSED);
    lv_obj_t *s = lv_label_create(b);
    lv_obj_set_style_text_color(s, lv_color_white(), 0);
    lv_obj_set_style_text_font(s, mdi_icon_font(size >= 48 ? 32 : 20), 0);
    char buf[8];
    lv_label_set_text(s, mdi_icon_text(icona, buf, sizeof(buf)) ? buf : "");
    lv_obj_center(s);
    if (cb) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    }
    return b;
}

static void cmd_toggle_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    ha_ws_call_service(domain_of(*eid).c_str(), "toggle", eid->c_str());
}

// ---- luce ----

static void luce_arco_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    int pct = lv_arc_get_value(lv_event_get_target(e));
    char extra[32];
    snprintf(extra, sizeof(extra), "\"brightness_pct\":%d", pct);
    ha_ws_call_service_data("light", "turn_on", eid->c_str(), extra);
}

static void luce_refresh(LuceCard &c, const std::string &eid)
{
    const Entity &e = ent(eid);
    bool spenta = e.state != "on";
    bool guasta = unavailable(eid);

    /* A luce spenta la luminosita' non la guardo: HA non avvisa quando un
       attributo sparisce, quindi li' dentro c'e' ancora quella di prima. */
    int pct = 100;
    if (!spenta && e.brightness >= 0)
        pct = LV_CLAMP(1, (int)lround(e.brightness * 100.0 / 255.0), 100);

    lv_color_t col = guasta ? C_UNAVAIL : (spenta ? C_ICON_OFF : C_ON_LIGHT);
    if (c.arco) {
        if (!in_mano(c.arco)) lv_arc_set_value(c.arco, spenta ? 1 : pct);
        lv_obj_set_style_arc_color(c.arco, col, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c.arco, col, LV_PART_KNOB);
        lv_obj_set_style_opa(c.arco, spenta ? LV_OPA_40 : LV_OPA_COVER, LV_PART_KNOB);
    }
    if (c.bulbo) lv_obj_set_style_bg_color(c.bulbo, col, 0);
    if (c.simbolo) {
        char b[8];
        const char *n = spenta ? "lightbulb" : "lightbulb-on";
        if (!e.icon.empty() && mdi_icon_text(e.icon.c_str(), b, sizeof(b)))
            lv_label_set_text(c.simbolo, b);
        else if (mdi_icon_text(n, b, sizeof(b)))
            lv_label_set_text(c.simbolo, b);
    }
    if (c.l_stato) {
        char b[24];
        if (guasta)      snprintf(b, sizeof(b), "Non disponibile");
        else if (spenta) snprintf(b, sizeof(b), "Spenta");
        else             snprintf(b, sizeof(b), "%d%%", pct);
        lv_label_set_text(c.l_stato, b);
    }
}

static void render_light(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    const char *nm = jstr(card, "name");

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);

    int d = LV_MIN(w - 40, 170);
    if (h > 0) d = LV_MIN(d, h - 80);
    if (d < 90) d = 90;

    lv_obj_t *box = mk_box(c, d, d);
    int slot = cmd_slot(eid);

    LuceCard lc;
    lc.arco = lv_arc_create(box);
    lv_obj_set_size(lc.arco, d, d);
    lv_obj_center(lc.arco);
    lv_arc_set_rotation(lc.arco, 135);
    lv_arc_set_bg_angles(lc.arco, 0, 270);
    lv_arc_set_range(lc.arco, 1, 100);
    lv_obj_set_style_arc_width(lc.arco, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(lc.arco, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(lc.arco, lv_color_hex(0x2b2f35), LV_PART_MAIN);
    lv_obj_set_style_pad_all(lc.arco, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(lc.arco, luce_arco_cb, LV_EVENT_RELEASED, (void *)(intptr_t)slot);

    /* La lampadina sta sopra al cerchio: cosi' il tocco al centro accende e
       spegne invece di far saltare la luminosita' al valore di quel punto. */
    lc.bulbo = mk_box(box, d / 2, d / 2);
    lv_obj_center(lc.bulbo);
    lv_obj_set_style_radius(lc.bulbo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(lc.bulbo, LV_OPA_COVER, 0);
    lv_obj_add_flag(lc.bulbo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lc.bulbo, cmd_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    lc.simbolo = lv_label_create(lc.bulbo);
    lv_obj_set_style_text_color(lc.simbolo, lv_color_white(), 0);
    lv_obj_set_style_text_font(lc.simbolo, mdi_icon_font(32), 0);
    lv_obj_center(lc.simbolo);

    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT, w - 24);
    lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);
    lc.l_stato = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, 0);

    s_luci.push_back(lc);
    bind(eid, B_LIGHT, c, NULL, ln, NULL, nm ? sanitize(nm) : "", (int)s_luci.size() - 1);
}

// ---- termostato ----

/* I modi e quello che il termostato sta facendo davvero, in italiano. In HA
   sono due cose diverse: "heat" e' come e' impostato, "heating" e' che in
   questo momento sta scaldando. Se c'e', vale la seconda. */
static std::string termo_modo(const std::string &s)
{
    if (s == "off")       return "Spento";
    if (s == "heat")      return "Riscaldamento";
    if (s == "cool")      return "Raffrescamento";
    if (s == "heat_cool") return "Automatico";
    if (s == "auto")      return "Automatico";
    if (s == "dry")       return "Deumidifica";
    if (s == "fan_only")  return "Ventilazione";
    return sanitize(s.c_str());
}

static std::string termo_azione(const std::string &a)
{
    if (a == "heating") return "Sta riscaldando";
    if (a == "cooling") return "Sta raffrescando";
    if (a == "drying")  return "Sta deumidificando";
    if (a == "fan")     return "Sta ventilando";
    if (a == "idle")    return "Fermo";
    if (a == "off")     return "Spento";
    return "";
}

static std::string gradi(double v)
{
    if (std::isnan(v)) return "--";
    char b[16];
    snprintf(b, sizeof(b), "%.1f", v);
    for (char *p = b; *p; p++) if (*p == '.') *p = ',';
    size_t n = strlen(b);
    if (n > 2 && b[n - 1] == '0' && b[n - 2] == ',') b[n - 2] = 0;   // 20,0 -> 20
    return std::string(b) + "\xC2\xB0";
}

static void termo_manda(const std::string &eid, double t)
{
    const Entity &e = ent(eid);
    double mn = e.temp_min, mx = e.temp_max;
    if (!(mx > mn)) { mn = 7; mx = 35; }
    t = LV_CLAMP(mn, t, mx);
    char extra[40];
    snprintf(extra, sizeof(extra), "\"temperature\":%.1f", t);
    ha_ws_call_service_data("climate", "set_temperature", eid.c_str(), extra);
}

static void termo_arco_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    /* Il cerchio lavora in decimi di grado perche' vuole numeri interi: qui
       torno ai gradi e li arrotondo al passo del termostato. */
    double passo = ent(*eid).temp_step;
    if (!(passo > 0)) passo = 0.5;
    double t = lv_arc_get_value(lv_event_get_target(e)) / 10.0;
    termo_manda(*eid, round(t / passo) * passo);
}

static void termo_passo_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    /* Il segno sta nella parte alta del numero passato all'evento: il piu'
       e il meno sono lo stesso tasto con due slot diversi. */
    const Entity &en = ent(*eid);
    double passo = en.temp_step > 0 ? en.temp_step : 0.5;
    double base = std::isnan(en.temp_set) ? en.temp_now : en.temp_set;
    if (std::isnan(base)) return;
    int verso = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    termo_manda(*eid, base + verso * passo);
}

static void termo_refresh(TermoCard &c, const std::string &eid)
{
    const Entity &e = ent(eid);
    bool guasto = unavailable(eid);
    bool spento = e.state == "off" || guasto;

    double mn = e.temp_min, mx = e.temp_max;
    if (!(mx > mn)) { mn = 7; mx = 35; }

    const std::string &az = e.azione;
    lv_color_t col = C_ICON_OFF;
    if (guasto)                              col = C_UNAVAIL;
    else if (az == "heating" || (az.empty() && e.state == "heat")) col = C_ON_LIGHT;
    else if (az == "cooling" || (az.empty() && e.state == "cool")) col = C_ON;
    else if (!spento)                        col = C_SENSOR;

    if (c.arco) {
        lv_arc_set_range(c.arco, (int)lround(mn * 10), (int)lround(mx * 10));
        if (!in_mano(c.arco) && !std::isnan(e.temp_set))
            lv_arc_set_value(c.arco, (int)lround(e.temp_set * 10));
        lv_obj_set_style_arc_color(c.arco, col, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c.arco, col, LV_PART_KNOB);
    }
    if (c.l_voluta) lv_label_set_text(c.l_voluta, gradi(e.temp_set).c_str());
    if (c.l_misurata) {
        std::string t = std::isnan(e.temp_now) ? "" : "adesso " + gradi(e.temp_now);
        lv_label_set_text(c.l_misurata, t.c_str());
    }
    if (c.l_azione) {
        std::string t = guasto ? "Non disponibile" : termo_azione(az);
        if (t.empty()) t = termo_modo(e.state);
        lv_label_set_text(c.l_azione, t.c_str());
    }
    if (c.simbolo) {
        char b[8];
        const char *n = (az == "heating") ? "fire"
                      : (az == "cooling") ? "snowflake"
                      : (az == "fan")     ? "fan" : "thermostat";
        if (mdi_icon_text(n, b, sizeof(b))) lv_label_set_text(c.simbolo, b);
        lv_obj_set_style_text_color(c.simbolo, col, 0);
    }
}

static void render_thermostat(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    const char *nm = jstr(card, "name");

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);

    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 24);
    lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);

    int d = LV_MIN(w - 40, 190);
    if (h > 0) d = LV_MIN(d, h - 110);
    if (d < 110) d = 110;

    lv_obj_t *box = mk_box(c, d, d);
    int slot = cmd_slot(eid);

    TermoCard tc;
    tc.arco = lv_arc_create(box);
    lv_obj_set_size(tc.arco, d, d);
    lv_obj_center(tc.arco);
    lv_arc_set_rotation(tc.arco, 135);
    lv_arc_set_bg_angles(tc.arco, 0, 270);
    lv_arc_set_range(tc.arco, 70, 350);
    lv_obj_set_style_arc_width(tc.arco, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(tc.arco, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(tc.arco, lv_color_hex(0x2b2f35), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tc.arco, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(tc.arco, termo_arco_cb, LV_EVENT_RELEASED, (void *)(intptr_t)slot);

    /* Al centro il numero grande, che e' la temperatura voluta: e' quella che
       si regola, mentre quella misurata sta scritta piccola sotto. */
    lv_obj_t *mezzo = mk_box(tc.arco, d - 40, LV_SIZE_CONTENT);
    lv_obj_center(mezzo);
    lv_obj_set_flex_flow(mezzo, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mezzo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(mezzo, LV_OBJ_FLAG_CLICKABLE);
    tc.l_voluta = mk_label(mezzo, "--", d >= 160 ? &lv_font_montserrat_40 : &lv_font_montserrat_28,
                           C_TEXT, 0);
    tc.l_misurata = mk_label(mezzo, "", &lv_font_montserrat_14, C_TEXT2, 0);

    /* Meno, quello che sta facendo, piu'. */
    lv_obj_t *riga = mk_box(c, w - 24, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(riga, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(riga, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *meno = mk_tasto(riga, 40, "minus", lv_color_hex(0x2b2f35), termo_passo_cb, slot);
    lv_obj_set_user_data(meno, (void *)(intptr_t)-1);

    lv_obj_t *mid = mk_box(riga, w - 24 - 2 * 40 - 16, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 6, 0);
    tc.simbolo = lv_label_create(mid);
    lv_obj_set_style_text_font(tc.simbolo, mdi_icon_font(20), 0);
    lv_obj_set_style_text_color(tc.simbolo, C_TEXT2, 0);
    lv_label_set_text(tc.simbolo, "");
    tc.l_azione = mk_label(mid, "", &lv_font_montserrat_14, C_TEXT2, 0);

    lv_obj_t *piu = mk_tasto(riga, 40, "plus", lv_color_hex(0x2b2f35), termo_passo_cb, slot);
    lv_obj_set_user_data(piu, (void *)(intptr_t)1);

    s_termo.push_back(tc);
    bind(eid, B_CLIMATE, c, NULL, ln, NULL, nm ? sanitize(nm) : "", (int)s_termo.size() - 1);
}

// ---- lettore multimediale ----

/* I bit con cui HA dice cosa sa fare un lettore. Servono per non mettere
   tasti che poi non funzionano: non tutti i lettori sanno saltare al brano
   dopo, e su una radio il volume c'e' ma il "precedente" no. */
#define MP_PAUSE      (1 << 0)
#define MP_VOLUME_SET (1 << 2)
#define MP_PREC       (1 << 4)
#define MP_SUCC       (1 << 5)
#define MP_PLAY       (1 << 14)

static void media_tasto_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    const char *svc = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
    if (svc) ha_ws_call_service("media_player", svc, eid->c_str());
}

static void media_volume_cb(lv_event_t *e)
{
    const std::string *eid = cmd_eid(e);
    if (!eid) return;
    char extra[40];
    snprintf(extra, sizeof(extra), "\"volume_level\":%.2f",
             lv_slider_get_value(lv_event_get_target(e)) / 100.0);
    ha_ws_call_service_data("media_player", "volume_set", eid->c_str(), extra);
}

static void media_refresh(MediaCard &c, const std::string &eid)
{
    const Entity &e = ent(eid);
    bool suona = e.state == "playing";
    bool fermo = e.state == "off" || e.state == "idle" || e.state == "standby" ||
                 unavailable(eid);

    if (c.simbolo) {
        char b[8];
        if (mdi_icon_text(suona ? "pause" : "play", b, sizeof(b)))
            lv_label_set_text(c.simbolo, b);
    }
    /* A lettore spento il titolo di prima e' rimasto negli attributi: HA non
       avvisa quando un attributo sparisce, quindi lo nascondo io. */
    if (c.l_titolo)
        lv_label_set_text(c.l_titolo, fermo || e.titolo.empty()
                          ? sanitize(display_name(eid, "").c_str()).c_str()
                          : sanitize(e.titolo.c_str()).c_str());
    if (c.l_artista)
        lv_label_set_text(c.l_artista, fermo ? "" : sanitize(e.artista.c_str()).c_str());
    if (c.l_stato) lv_label_set_text(c.l_stato, format_state(eid).c_str());

    if (c.barra_vol && !in_mano(c.barra_vol) && e.volume >= 0)
        lv_slider_set_value(c.barra_vol, (int)lround(e.volume * 100), LV_ANIM_OFF);

    /* Quali tasti servono lo dice HA. Finche' non l'ha detto (0) li lascio
       tutti: meglio un tasto che non fa niente di un lettore senza tasti. */
    int f = e.funzioni;
    if (f) {
        if (c.prec) lv_obj_add_flag(c.prec, LV_OBJ_FLAG_HIDDEN);
        if (c.succ) lv_obj_add_flag(c.succ, LV_OBJ_FLAG_HIDDEN);
        if (c.volume) lv_obj_add_flag(c.volume, LV_OBJ_FLAG_HIDDEN);
        if ((f & MP_PREC) && c.prec) lv_obj_clear_flag(c.prec, LV_OBJ_FLAG_HIDDEN);
        if ((f & MP_SUCC) && c.succ) lv_obj_clear_flag(c.succ, LV_OBJ_FLAG_HIDDEN);
        if ((f & MP_VOLUME_SET) && c.volume) lv_obj_clear_flag(c.volume, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_media(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    const char *nm = jstr(card, "name");

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);
    int tw = w - 24;
    int slot = cmd_slot(eid);

    MediaCard mc;
    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, tw);
    lv_obj_set_style_text_align(ln, LV_TEXT_ALIGN_CENTER, 0);
    mc.l_titolo = mk_label(c, "", &lv_font_montserrat_20, C_TEXT, tw);
    lv_obj_set_style_text_align(mc.l_titolo, LV_TEXT_ALIGN_CENTER, 0);
    mc.l_artista = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, tw);
    lv_obj_set_style_text_align(mc.l_artista, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *tasti = mk_box(c, tw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tasti, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tasti, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tasti, 14, 0);
    lv_obj_set_style_pad_top(tasti, 6, 0);

    mc.prec = mk_tasto(tasti, 40, "skip-previous", lv_color_hex(0x2b2f35), media_tasto_cb, slot);
    lv_obj_set_user_data(mc.prec, (void *)"media_previous_track");
    lv_obj_t *play = mk_tasto(tasti, 52, "play", C_ON, media_tasto_cb, slot);
    lv_obj_set_user_data(play, (void *)"media_play_pause");
    mc.simbolo = lv_obj_get_child(play, 0);
    mc.succ = mk_tasto(tasti, 40, "skip-next", lv_color_hex(0x2b2f35), media_tasto_cb, slot);
    lv_obj_set_user_data(mc.succ, (void *)"media_next_track");

    mc.volume = mk_box(c, tw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mc.volume, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mc.volume, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mc.volume, 10, 0);
    lv_obj_set_style_pad_top(mc.volume, 8, 0);
    lv_obj_t *vi = lv_label_create(mc.volume);
    lv_obj_set_style_text_font(vi, mdi_icon_font(20), 0);
    lv_obj_set_style_text_color(vi, C_TEXT2, 0);
    {
        char b[8];
        lv_label_set_text(vi, mdi_icon_text("volume-high", b, sizeof(b)) ? b : "");
    }
    mc.barra_vol = lv_slider_create(mc.volume);
    lv_obj_set_width(mc.barra_vol, tw - 50);
    lv_slider_set_range(mc.barra_vol, 0, 100);
    lv_obj_set_style_bg_color(mc.barra_vol, C_ON, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mc.barra_vol, C_ON, LV_PART_KNOB);
    lv_obj_add_event_cb(mc.barra_vol, media_volume_cb, LV_EVENT_RELEASED, (void *)(intptr_t)slot);

    mc.l_stato = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, tw);
    lv_obj_set_style_text_align(mc.l_stato, LV_TEXT_ALIGN_CENTER, 0);

    s_media.push_back(mc);
    bind(eid, B_MEDIA, c, NULL, ln, NULL, nm ? sanitize(nm) : "", (int)s_media.size() - 1);
}

// ------------------------------------------------------------------ orologio

/* Card "clock": l'unica che non guarda ne' entita' ne' Home Assistant. */
struct Orologio {
    lv_obj_t   *l;
    lv_timer_t *t;
    bool        secondi;
    bool        ore12;
};
static std::vector<Orologio *> s_orologi;

static void orologio_scrivi(Orologio *o)
{
    time_t tt = time(NULL);
    struct tm tm;
    localtime_r(&tt, &tm);
    char b[32];
    if (o->ore12) {
        int h = tm.tm_hour % 12; if (!h) h = 12;
        if (o->secondi) snprintf(b, sizeof(b), "%d:%02d:%02d %s", h, tm.tm_min, tm.tm_sec,
                                 tm.tm_hour < 12 ? "AM" : "PM");
        else            snprintf(b, sizeof(b), "%d:%02d %s", h, tm.tm_min,
                                 tm.tm_hour < 12 ? "AM" : "PM");
    } else {
        if (o->secondi) snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        else            snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    lv_label_set_text(o->l, b);
}

static void orologio_tick(lv_timer_t *t) { orologio_scrivi((Orologio *)t->user_data); }

static void render_clock(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char *title = jstr(card, "title");
    if (title) mk_label(c, sanitize(title), &lv_font_montserrat_16, C_TEXT2, w - 28);

    const char *dim = jstr(card, "clock_size");
    const lv_font_t *f = &lv_font_montserrat_32;
    if (dim && !strcmp(dim, "small"))  f = &lv_font_montserrat_24;
    if (dim && !strcmp(dim, "medium")) f = &lv_font_montserrat_32;
    if (dim && !strcmp(dim, "large"))  f = &lv_font_montserrat_48;

    Orologio *o = new Orologio();
    o->l = mk_label(c, "--:--", f, C_TEXT, w - 28);
    const cJSON *sec = cJSON_GetObjectItem(card, "show_seconds");
    o->secondi = cJSON_IsTrue(sec);
    const char *fmt = jstr(card, "time_format");
    o->ore12 = fmt && !strcmp(fmt, "12");
    /* Un secondo se li mostra, altrimenti mezzo minuto: svegliare il pannello
       sessanta volte al minuto per cambiare una cifra che non cambia sarebbe
       lavoro buttato. */
    o->t = lv_timer_create(orologio_tick, o->secondi ? 1000 : 30000, o);
    orologio_scrivi(o);
    s_orologi.push_back(o);
}

// ------------------------------------------------------------------ alert

/* Card "alert": un riquadro che si accende di colore quando l'entita' e'
   attiva. Serve a farsi notare - porta aperta, allarme, perdita d'acqua - e
   quindi a spento deve sparire nel fondo, non restare acceso a meta'. */
static void alert_refresh(Bind &b)
{
    bool on = is_on(b.eid);
    lv_color_t col = lv_color_hex(0xdb4437);
    if (b.extra >= 0) col = lv_color_hex((uint32_t)b.extra);
    lv_obj_set_style_bg_color(b.obj, on ? col : C_CARD, 0);
    if (b.l_name) {
        /* Il nome va scritto, non solo colorato: senza questa riga la card
           mostrava lo stato accanto a uno spazio vuoto, e chi guarda non
           sapeva di cosa gli si stesse parlando. */
        lv_label_set_text(b.l_name, sanitize(display_name(b.eid, b.name_override).c_str()).c_str());
        lv_obj_set_style_text_color(b.l_name, on ? lv_color_white() : C_TEXT2, 0);
    }
    if (b.l_state) {
        lv_label_set_text(b.l_state, format_state(b.eid).c_str());
        lv_obj_set_style_text_color(b.l_state, on ? lv_color_white() : C_TEXT, 0);
    }
    if (b.icon) lv_obj_set_style_text_color(lv_obj_get_child(b.icon, 0),
                                            on ? lv_color_white() : C_ICON_OFF, 0);
}

/* I colori che HA scrive per nome. Quelli che non conosciamo diventano il
   rosso predefinito: meglio un avviso del colore sbagliato che nessun avviso. */
static uint32_t colore_ha(const char *n)
{
    if (!n) return 0xdb4437;
    struct { const char *n; uint32_t c; } t[] = {
        {"red",0xdb4437},{"pink",0xe91e63},{"purple",0x926bc7},{"deep-purple",0x6e41ab},
        {"indigo",0x3f51b5},{"blue",0x2196f3},{"light-blue",0x03a9f4},{"cyan",0x00bcd4},
        {"teal",0x009688},{"green",0x43a047},{"light-green",0x8bc34a},{"lime",0xcddc39},
        {"yellow",0xffeb3b},{"amber",0xffc107},{"orange",0xff9800},{"deep-orange",0xff5722},
        {"brown",0x795548},{"grey",0x9e9e9e},{"gray",0x9e9e9e},{"blue-grey",0x607d8b},
        {"black",0x000000},{"white",0xffffff},
    };
    for (auto &x : t) if (!strcmp(n, x.n)) return x.c;
    return 0xdb4437;
}

static void render_alert(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : ROW_H);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 10, 0);

    lv_obj_t *icon = mk_icon(c, 30);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_label_set_text(lv_obj_get_child(icon, 0), icon_symbol(eid).c_str());

    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, (w - 70) / 2);
    lv_obj_set_flex_grow(ln, 1);
    lv_obj_t *ls = mk_label(c, "", &lv_font_montserrat_16, C_TEXT, (w - 70) / 2);
    lv_obj_set_style_text_align(ls, LV_TEXT_ALIGN_RIGHT, 0);

    const char *nm = jstr(card, "name");
    Bind b; b.eid = eid; b.kind = B_ALERT; b.obj = c; b.icon = icon;
    b.l_name = ln; b.l_state = ls;
    b.name_override = nm ? sanitize(nm) : "";
    const char *col = jstr(card, "color");
    b.extra = (col && strcmp(col, "none")) ? (int)colore_ha(col) : (int)0xdb4437;
    s_binds.push_back(b);
    refresh(s_binds.back());
    attach_action(c, eid, card, LV_EVENT_CLICKED);
}

// ------------------------------------------------------------------ toggle-group

/* Card "toggle-group": un solo tasto per un gruppo di entita'. Mostra quante
   sono accese e le comanda tutte insieme con un messaggio solo. */
struct Gruppo {
    std::vector<std::string> eid;
    std::string dominio;
    lv_obj_t *l_conteggio;
    lv_obj_t *icona;
    uint32_t colore;
};
static std::vector<Gruppo *> s_gruppi;

static void gruppo_refresh(Gruppo *g)
{
    int accese = 0;
    for (const std::string &e : g->eid) if (is_on(e)) accese++;
    char b[48];
    if (accese == 0)                       snprintf(b, sizeof(b), "Tutte spente");
    else if (accese == (int)g->eid.size()) snprintf(b, sizeof(b), "Tutte accese");
    else                                   snprintf(b, sizeof(b), "%d di %u accese",
                                                    accese, (unsigned)g->eid.size());
    lv_label_set_text(g->l_conteggio, b);
    lv_obj_set_style_bg_color(g->icona, accese ? lv_color_hex(g->colore) : C_ICON_OFF, 0);
}

static void gruppo_cb(lv_event_t *e)
{
    Gruppo *g = (Gruppo *)lv_event_get_user_data(e);
    if (!g || g->eid.empty()) return;
    /* Se ne e' accesa almeno una si spegne tutto, altrimenti si accende tutto:
       e' il comportamento di HA, e soprattutto e' prevedibile - due tocchi
       riportano sempre allo stesso punto. */
    bool qualcuna = false;
    for (const std::string &x : g->eid) if (is_on(x)) { qualcuna = true; break; }
    std::string lista;
    for (const std::string &x : g->eid) lista += (lista.empty() ? "\"" : ",\"") + x + "\"";
    ha_ws_call_service_many(g->dominio.c_str(), qualcuna ? "turn_off" : "turn_on", lista.c_str());
}

static void render_toggle_group(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    Gruppo *g = new Gruppo();
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(card, "entities")) {
        const char *e = cJSON_IsString(row) ? row->valuestring : jstr(row, "entity");
        if (e) g->eid.push_back(e);
    }
    if (g->eid.empty()) { delete g; render_placeholder(parent, card, w, h); return; }
    g->dominio = domain_of(g->eid[0]);
    const char *col = jstr(card, "color");
    g->colore = (col && strcmp(col, "none")) ? colore_ha(col) : 0x03a9f4;

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 2 * ROW_H + GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);

    g->icona = mk_icon(c, 44);
    lv_label_set_text(lv_obj_get_child(g->icona, 0), icon_symbol(g->eid[0]).c_str());

    const char *title = jstr(card, "title");
    mk_label(c, sanitize(title ? title : "Gruppo"), &lv_font_montserrat_16, C_TEXT, w - 28);
    g->l_conteggio = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);

    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, gruppo_cb, LV_EVENT_CLICKED, g);
    s_gruppi.push_back(g);
    gruppo_refresh(g);
}

// ------------------------------------------------------------------ humidifier

struct Umid { lv_obj_t *ora, *set, *modo, *sw; };
std::vector<Umid *> s_umid;

static void umid_refresh(Umid *u, const std::string &eid)
{
    const Entity &e = ent(eid);
    char b[48];
    if (std::isfinite(e.umidita_ora)) snprintf(b, sizeof(b), "%.0f%%", e.umidita_ora);
    else                         snprintf(b, sizeof(b), "--");
    lv_label_set_text(u->ora, b);
    if (std::isfinite(e.umidita_set)) snprintf(b, sizeof(b), "obiettivo %.0f%%", e.umidita_set);
    else                         b[0] = '\0';
    lv_label_set_text(u->set, b);
    lv_label_set_text(u->modo, sanitize(e.modo.c_str()).c_str());
    if (is_on(eid)) lv_obj_add_state(u->sw, LV_STATE_CHECKED);
    else            lv_obj_clear_state(u->sw, LV_STATE_CHECKED);
}

static void render_humidifier(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 3 * ROW_H + 2 * GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);

    const char *nm = jstr(card, "name");
    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);
    bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");

    Umid *u = new Umid();
    u->ora  = mk_label(c, "--", &lv_font_montserrat_48, C_TEXT, w - 28);
    u->set  = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);
    u->modo = mk_label(c, "", &lv_font_montserrat_14, C_TEXT2, w - 28);
    u->sw   = lv_switch_create(c);
    lv_obj_set_size(u->sw, 54, 28);
    lv_obj_set_style_bg_color(u->sw, C_ON, LV_PART_INDICATOR | LV_STATE_CHECKED);
    s_actions.push_back({eid, "humidifier", "toggle"});
    lv_obj_add_event_cb(u->sw, switch_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)(s_actions.size() - 1));

    s_umid.push_back(u);
    Bind b; b.eid = eid; b.kind = B_UMID; b.obj = c;
    b.icon = NULL; b.l_name = NULL; b.l_state = NULL;
    b.extra = (int)s_umid.size() - 1;
    s_binds.push_back(b);
    umid_refresh(u, eid);
}

// ------------------------------------------------------------------ alarm-panel

/* Card "alarm-panel": stato dell'allarme e i tasti per inserirlo o
   disinserirlo. Il codice, se l'impianto lo chiede, lo scrive chi sta davanti
   al pannello sulla tastiera: non viene mai salvato ne' scritto nel log. */
struct Allarme {
    std::string eid;
    lv_obj_t   *stato;
    lv_obj_t   *codice;      // NULL se l'impianto non chiede codice
    std::vector<std::string> servizi;
};
std::vector<Allarme *> s_allarmi;

static void allarme_refresh(Allarme *a)
{
    const std::string &st = ent(a->eid).state;
    const char *t = st.c_str();
    lv_color_t col = C_TEXT;
    if (st == "disarmed")                          { t = "Disinserito"; col = lv_color_hex(0x43a047); }
    else if (st == "armed_home")                   { t = "Inserito in casa"; col = lv_color_hex(0xff9800); }
    else if (st == "armed_away")                   { t = "Inserito fuori casa"; col = lv_color_hex(0xff9800); }
    else if (st == "armed_night")                  { t = "Inserito notte"; col = lv_color_hex(0xff9800); }
    else if (st == "armed_vacation")               { t = "Inserito vacanza"; col = lv_color_hex(0xff9800); }
    else if (st == "arming" || st == "pending")    { t = "In inserimento..."; col = lv_color_hex(0xffa600); }
    else if (st == "triggered")                    { t = "ALLARME"; col = lv_color_hex(0xdb4437); }
    lv_label_set_text(a->stato, t);
    lv_obj_set_style_text_color(a->stato, col, 0);
}

static void allarme_cb(lv_event_t *e)
{
    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
    Allarme *a = (Allarme *)lv_obj_get_user_data(b);
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (!a || i < 0 || i >= (intptr_t)a->servizi.size()) return;

    const char *cod = a->codice ? lv_textarea_get_text(a->codice) : NULL;
    if (cod && *cod) {
        /* Il codice viaggia dentro la chiamata al servizio e basta: non si
           salva, non si stampa, e la casella si svuota subito dopo. */
        std::string extra = std::string("\"code\":\"") + cod + "\"";
        ha_ws_call_service_data("alarm_control_panel", a->servizi[i].c_str(),
                                a->eid.c_str(), extra.c_str());
        lv_textarea_set_text(a->codice, "");
    } else {
        ha_ws_call_service("alarm_control_panel", a->servizi[i].c_str(), a->eid.c_str());
    }
}

static void render_alarm_panel(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    Allarme *a = new Allarme();
    a->eid = eid;

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : 4 * ROW_H + 3 * GAP);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);

    const char *nm = jstr(card, "name");
    lv_obj_t *ln = mk_label(c, "", &lv_font_montserrat_16, C_TEXT2, w - 28);
    bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
    a->stato = mk_label(c, "", &lv_font_montserrat_24, C_TEXT, w - 28);

    /* Gli stati che l'utente ha scelto, piu' il disinserimento che c'e'
       sempre: una card da cui non si puo' spegnere l'allarme non serve. */
    std::vector<std::string> nomi;
    a->servizi.push_back("alarm_disarm"); nomi.push_back("Disinserisci");
    const cJSON *st;
    cJSON_ArrayForEach(st, cJSON_GetObjectItem(card, "states")) {
        if (!cJSON_IsString(st)) continue;
        std::string v = st->valuestring;
        if      (v == "arm_home")     { a->servizi.push_back("alarm_arm_home");     nomi.push_back("In casa"); }
        else if (v == "arm_away")     { a->servizi.push_back("alarm_arm_away");     nomi.push_back("Fuori casa"); }
        else if (v == "arm_night")    { a->servizi.push_back("alarm_arm_night");    nomi.push_back("Notte"); }
        else if (v == "arm_vacation") { a->servizi.push_back("alarm_arm_vacation"); nomi.push_back("Vacanza"); }
    }
    if (a->servizi.size() == 1) { a->servizi.push_back("alarm_arm_away"); nomi.push_back("Fuori casa"); }

    a->codice = lv_textarea_create(c);
    lv_obj_set_width(a->codice, w - 28);
    lv_textarea_set_one_line(a->codice, true);
    lv_textarea_set_password_mode(a->codice, true);
    lv_textarea_set_placeholder_text(a->codice, "Codice, se serve");

    lv_obj_t *riga = mk_box(c, w - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(riga, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(riga, 6, 0);
    lv_obj_set_style_pad_row(riga, 6, 0);
    for (size_t i = 0; i < a->servizi.size(); i++) {
        lv_obj_t *b = lv_btn_create(riga);
        lv_obj_set_height(b, 42);
        lv_obj_set_width(b, (w - 40) / 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2b2d31), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_user_data(b, a);
        lv_obj_add_event_cb(b, allarme_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, nomi[i].c_str());
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, C_TEXT, 0);
        lv_obj_center(l);
    }

    s_allarmi.push_back(a);
    Bind b; b.eid = eid; b.kind = B_ALLARME; b.obj = c;
    b.icon = NULL; b.l_name = NULL; b.l_state = NULL;
    b.extra = (int)s_allarmi.size() - 1;
    s_binds.push_back(b);
    allarme_refresh(a);
}

// ------------------------------------------------------------------ condizioni

/* Le condizioni di Home Assistant, usate da "conditional" e da
   "entity-filter". Ne esistono due scritture: quella breve di una volta
   ({entity, state}) e quella nuova con "condition". Si accettano entrambe,
   perche' le dashboard scritte anni fa sono ancora in giro.

   Una condizione che non sappiamo valutare vale VERA. E' una scelta: il
   dubbio deve far vedere la card, non nasconderla. Una card sparita non si
   cerca, e chi guarda il pannello non saprebbe nemmeno che manca. */
static bool condizione_vera(const cJSON *cond);

static bool condizioni_vere(const cJSON *arr)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, arr) if (!condizione_vera(c)) return false;
    return true;
}

static bool condizione_vera(const cJSON *cond)
{
    if (!cJSON_IsObject(cond)) return true;
    const char *tipo = jstr(cond, "condition");
    const char *eid  = jstr(cond, "entity");

    if (tipo && !strcmp(tipo, "and")) return condizioni_vere(cJSON_GetObjectItem(cond, "conditions"));
    if (tipo && !strcmp(tipo, "or")) {
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(cond, "conditions"))
            if (condizione_vera(c)) return true;
        return false;
    }
    if (tipo && !strcmp(tipo, "not")) return !condizioni_vere(cJSON_GetObjectItem(cond, "conditions"));

    if (tipo && !strcmp(tipo, "numeric_state")) {
        if (!eid) return true;
        double v;
        if (!state_number(ent(eid).state.c_str(), &v)) return false;
        const cJSON *a = cJSON_GetObjectItem(cond, "above");
        const cJSON *b = cJSON_GetObjectItem(cond, "below");
        if (cJSON_IsNumber(a) && !(v > a->valuedouble)) return false;
        if (cJSON_IsNumber(b) && !(v < b->valuedouble)) return false;
        return true;
    }

    /* "state" esplicito, oppure la scrittura breve senza "condition". */
    if (!tipo || !strcmp(tipo, "state")) {
        if (!eid) return true;
        const std::string &st = ent(eid).state;
        const cJSON *want = cJSON_GetObjectItem(cond, "state");
        const cJSON *nope = cJSON_GetObjectItem(cond, "state_not");
        auto combacia = [&](const cJSON *v) {
            if (cJSON_IsString(v)) return st == v->valuestring;
            if (cJSON_IsArray(v)) {
                const cJSON *x;
                cJSON_ArrayForEach(x, v) if (cJSON_IsString(x) && st == x->valuestring) return true;
                return false;
            }
            return false;
        };
        if (want && !combacia(want)) return false;
        if (nope && combacia(nope)) return false;
        return true;
    }

    return true;      // screen, user, location e i tipi futuri
}

/* Raccoglie le entita' che compaiono in un albero di condizioni: sono quelle
   il cui cambiamento deve far rivalutare la card. */
static void entita_delle_condizioni(const cJSON *n, std::vector<std::string> &out)
{
    if (!n) return;
    if (cJSON_IsObject(n)) {
        const char *e = jstr(n, "entity");
        if (e) out.push_back(e);
    }
    const cJSON *f;
    cJSON_ArrayForEach(f, n)
        if (cJSON_IsObject(f) || cJSON_IsArray(f)) entita_delle_condizioni(f, out);
}

// --------------------------------------------- card che si rifanno da sole

/* Le card il cui *contenuto* cambia quando cambiano gli stati, non solo le
   etichette: "conditional" appare e sparisce, "entity-filter" cambia elenco.
   Non basta aggiornare un'etichetta: va rifatto il disegno.

   Stesso schema delle card dell'energia: il contenitore si crea una volta, il
   dentro si rigenera al suo posto. Ricostruire l'intera vista sarebbe stato
   piu' semplice, ma farebbe saltare via lo scorrimento sotto le dita. */
struct DynCard {
    lv_obj_t    *box;
    const cJSON *card;                  // vive dentro s_view
    std::string  tipo;
    int w, h;
    std::vector<std::string> guarda;    // entita' che la fanno rivalutare
};
static std::vector<DynCard> s_dyn;

static void fill_dyn(DynCard &d);

static void render_dyn(lv_obj_t *parent, const cJSON *card, const std::string &tipo,
                       int w, int h, const std::vector<std::string> &guarda)
{
    lv_obj_t *box = mk_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 0, 0);
    DynCard d; d.box = box; d.card = card; d.tipo = tipo; d.w = w; d.h = h; d.guarda = guarda;
    s_dyn.push_back(d);
    fill_dyn(s_dyn.back());
}

static void dyn_aggiorna(const std::string &eid)
{
    for (DynCard &d : s_dyn) {
        bool mia = false;
        for (const std::string &g : d.guarda) if (g == eid) { mia = true; break; }
        if (!mia || !d.box) continue;
        lv_obj_clean(d.box);
        fill_dyn(d);
    }
}

/* Il contenuto di una card che si rifa' da sola. Sta qui e non con le altre
   perche' ha bisogno di render_card: una "conditional" disegna dentro di se'
   una card qualunque, comprese le altre che si rifanno da sole. */
static void fill_dyn(DynCard &d)
{
    if (d.tipo == "conditional") {
        if (!condizioni_vere(cJSON_GetObjectItem(d.card, "conditions"))) return;   // nascosta
        const cJSON *dentro = cJSON_GetObjectItem(d.card, "card");
        if (dentro) render_card(d.box, dentro, d.w, d.h);
        return;
    }

    /* entity-filter: si tengono le entita' che passano il filtro e si passano
       alla card interna, che se non e' indicata e' una "entities". */
    const cJSON *cond = cJSON_GetObjectItem(d.card, "conditions");
    const cJSON *vecchio = cJSON_GetObjectItem(d.card, "state_filter");   // scrittura di una volta

    cJSON *tenute = cJSON_CreateArray();
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(d.card, "entities")) {
        const char *eid = cJSON_IsString(row) ? row->valuestring : jstr(row, "entity");
        if (!eid) continue;
        bool passa = true;
        if (cJSON_IsArray(cond) && cJSON_GetArraySize(cond) > 0) {
            /* Le condizioni possono non nominare l'entita': valgono per quella
               della riga. Si valuta una copia con l'entita' riempita. */
            const cJSON *c;
            cJSON_ArrayForEach(c, cond) {
                cJSON *cc = cJSON_Duplicate(c, true);
                if (!cJSON_GetObjectItem(cc, "entity"))
                    cJSON_AddStringToObject(cc, "entity", eid);
                bool ok = condizione_vera(cc);
                cJSON_Delete(cc);
                if (!ok) { passa = false; break; }
            }
        } else if (cJSON_IsArray(vecchio)) {
            passa = false;
            const cJSON *v;
            const std::string &st = ent(eid).state;
            cJSON_ArrayForEach(v, vecchio) {
                if (cJSON_IsString(v) && st == v->valuestring) { passa = true; break; }
                if (cJSON_IsObject(v)) {
                    cJSON *cc = cJSON_Duplicate(v, true);
                    if (!cJSON_GetObjectItem(cc, "entity"))
                        cJSON_AddStringToObject(cc, "entity", eid);
                    bool ok = condizione_vera(cc);
                    cJSON_Delete(cc);
                    if (ok) { passa = true; break; }
                }
            }
        }
        if (passa) cJSON_AddItemToArray(tenute, cJSON_Duplicate(row, true));
    }

    if (cJSON_GetArraySize(tenute) == 0) {
        /* Nessuna entita' passa: meglio dirlo che lasciare un buco muto, che
           sembrerebbe un guasto. */
        const cJSON *mostra = cJSON_GetObjectItem(d.card, "show_empty");
        if (!mostra || cJSON_IsTrue(mostra)) {
            lv_obj_t *c = mk_card(d.box, d.w, ROW_H);
            lv_obj_set_style_pad_all(c, 14, 0);
            mk_label(c, "Nessuna corrisponde", &lv_font_montserrat_16, C_TEXT2, d.w - 28);
        }
        cJSON_Delete(tenute);
        return;
    }

    const cJSON *modello = cJSON_GetObjectItem(d.card, "card");
    cJSON *dentro = modello ? cJSON_Duplicate(modello, true) : cJSON_CreateObject();
    if (!cJSON_GetObjectItem(dentro, "type")) cJSON_AddStringToObject(dentro, "type", "entities");
    cJSON_DeleteItemFromObject(dentro, "entities");
    cJSON_AddItemToObject(dentro, "entities", tenute);      // "dentro" se ne prende carico

    /* La card interna vive il tempo del disegno: quello che le serve lo copia
       nelle proprie etichette, e i bind tengono gli id per conto loro. */
    render_card(d.box, dentro, d.w, d.h);
    cJSON_Delete(dentro);
}

// ------------------------------------------------------------------ elenchi
//
// Quattro card che non mostrano lo stato di un'entita' ma un elenco che va
// chiesto a parte: le cose da fare, la lista della spesa, gli appuntamenti e
// il registro degli eventi.
//
// Hanno tutte la stessa forma - si chiede, si aspetta, si riempie - quindi
// condividono il registro e il modo di disegnare le righe. Cambia solo dove si
// va a chiedere: le prime due dal WebSocket, le altre due dall'API HTTP, che
// per calendario e registro e' l'unica che le espone.
//
// Le richieste passano una alla volta da un filo solo, come i grafici e
// l'energia: mandarne quattro insieme appena costruita la vista e' esattamente
// la raffica che il collegamento SDIO verso il C6 regge peggio.

enum TipoElenco { EL_TODO, EL_SPESA, EL_CALENDARIO, EL_REGISTRO };

struct Riga { std::string testo, sotto; bool fatta = false; std::string uid; };

struct Elenco {
    TipoElenco tipo;
    lv_obj_t  *box   = NULL;      // dove vanno le righe
    lv_obj_t  *stato = NULL;      // "Caricamento...", o il motivo
    std::string eid;              // entita', dove serve
    std::vector<std::string> eids;  // calendario/registro: possono essere piu' d'una
    int  giorni = 7;              // calendario
    int  ore = 24;                // registro
    int  massimo = 10;
    int  larghezza = 0;
    bool nascondi_fatte = false;
    bool in_corso = false;
    int64_t prossimo = 0;         // quando richiedere (secondi)
    std::vector<Riga> righe;
    bool arrivato = false;
};
static std::vector<Elenco *> s_elenchi;

/* Ridisegna le righe di un elenco. */
static void elenco_disegna(Elenco *e)
{
    if (!e->box) return;
    lv_obj_clean(e->box);

    if (!e->arrivato) {
        lv_label_set_text(e->stato, "Caricamento...");
        return;
    }
    if (e->righe.empty()) {
        lv_label_set_text(e->stato,
            e->tipo == EL_TODO || e->tipo == EL_SPESA ? "Niente da fare"
            : e->tipo == EL_CALENDARIO ? "Nessun appuntamento" : "Niente da segnalare");
        return;
    }
    lv_label_set_text(e->stato, "");

    int w = e->larghezza - 28;
    int n = 0;
    for (const Riga &r : e->righe) {
        if (n++ >= e->massimo) break;
        lv_obj_t *riga = mk_box(e->box, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(riga, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(riga, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(riga, 8, 0);
        lv_obj_set_style_pad_ver(riga, 4, 0);

        if (e->tipo == EL_TODO || e->tipo == EL_SPESA) {
            lv_obj_t *i = mk_icon(riga, 22);
            lv_obj_set_style_bg_opa(i, LV_OPA_TRANSP, 0);
            char gl[8];
            /* La coppia delle caselle, non i cerchi: "circle-outline" non e'
               fra le icone compilate dentro il pannello, e una voce da fare
               compariva senza niente accanto - cioe' senza il segno che dice
               che c'e' qualcosa da spuntare. */
            lv_label_set_text(lv_obj_get_child(i, 0),
                mdi_icon_text(r.fatta ? "checkbox-marked-outline" : "checkbox-blank-outline",
                              gl, sizeof(gl)) ? gl : "");
            lv_obj_set_style_text_color(lv_obj_get_child(i, 0),
                                        r.fatta ? C_ICON_OFF : C_ON, 0);
        }

        lv_obj_t *col = mk_box(riga, w - 34, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_t *t = mk_label(col, r.testo, &lv_font_montserrat_16,
                               r.fatta ? C_TEXT2 : C_TEXT, w - 34);
        (void)t;
        if (!r.sotto.empty())
            mk_label(col, r.sotto, &lv_font_montserrat_14, C_TEXT2, w - 34);
    }
    if ((int)e->righe.size() > e->massimo) {
        char b[48];
        snprintf(b, sizeof(b), "e altre %d", (int)e->righe.size() - e->massimo);
        mk_label(e->box, b, &lv_font_montserrat_14, C_TEXT2, w);
    }
}

/* ---- lettura delle risposte ---- */

static void elenco_da_todo(Elenco *e, const cJSON *res)
{
    e->righe.clear();
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "items")) {
        const char *sum = jstr(it, "summary");
        if (!sum) continue;
        const char *st = jstr(it, "status");
        Riga r;
        r.testo = sanitize(sum);
        r.fatta = st && !strcmp(st, "completed");
        if (e->nascondi_fatte && r.fatta) continue;
        const char *due = jstr(it, "due");
        if (due) r.sotto = std::string("entro ") + sanitize(due);
        const char *uid = jstr(it, "uid");
        if (uid) r.uid = uid;
        e->righe.push_back(r);
    }
    /* Prima le cose da fare, poi quelle fatte: e' l'ordine in cui si guarda
       una lista, e su un pannello si vedono solo le prime righe. */
    std::stable_sort(e->righe.begin(), e->righe.end(),
                     [](const Riga &a, const Riga &b) { return !a.fatta && b.fatta; });
}

static void elenco_da_spesa(Elenco *e, const cJSON *res)
{
    e->righe.clear();
    const cJSON *it;
    cJSON_ArrayForEach(it, res) {
        const char *nome = jstr(it, "name");
        if (!nome) continue;
        Riga r;
        r.testo = sanitize(nome);
        r.fatta = cJSON_IsTrue(cJSON_GetObjectItem(it, "complete"));
        if (e->nascondi_fatte && r.fatta) continue;
        e->righe.push_back(r);
    }
    std::stable_sort(e->righe.begin(), e->righe.end(),
                     [](const Riga &a, const Riga &b) { return !a.fatta && b.fatta; });
}

/* "2026-09-30T18:30:00+02:00" -> "mar 30 set, 18:30". Le date tutte-il-giorno
   arrivano come "2026-09-30" e non hanno ora da mostrare. */
static std::string quando_leggibile(const char *iso)
{
    if (!iso) return "";
    int Y = 0, M = 0, D = 0, h = -1, m = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) < 3) return sanitize(iso);
    static const char *mesi[] = {"gen","feb","mar","apr","mag","giu",
                                 "lug","ago","set","ott","nov","dic"};
    char b[48];
    if (h < 0) snprintf(b, sizeof(b), "%d %s", D, (M >= 1 && M <= 12) ? mesi[M-1] : "");
    else       snprintf(b, sizeof(b), "%d %s, %02d:%02d", D,
                        (M >= 1 && M <= 12) ? mesi[M-1] : "", h, m);
    return b;
}

static void elenco_da_calendario(Elenco *e, const cJSON *arr)
{
    const cJSON *ev;
    cJSON_ArrayForEach(ev, arr) {
        const char *sum = jstr(ev, "summary");
        if (!sum) continue;
        const cJSON *st = cJSON_GetObjectItem(ev, "start");
        const char *quando = NULL;
        if (cJSON_IsString(st)) quando = st->valuestring;
        else if (cJSON_IsObject(st)) {
            const cJSON *dt = cJSON_GetObjectItem(st, "dateTime");
            if (!cJSON_IsString(dt)) dt = cJSON_GetObjectItem(st, "date");
            if (cJSON_IsString(dt)) quando = dt->valuestring;
        }
        Riga r;
        r.testo = sanitize(sum);
        r.sotto = quando_leggibile(quando);
        e->righe.push_back(r);
    }
}

static void elenco_da_registro(Elenco *e, const cJSON *arr)
{
    e->righe.clear();
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        const char *nome = jstr(v, "name");
        const char *msg  = jstr(v, "message");
        const char *st   = jstr(v, "state");
        if (!nome && !msg) continue;
        Riga r;
        r.testo = sanitize(nome ? nome : "");
        if (msg)      r.sotto = sanitize(msg);
        else if (st)  r.sotto = sanitize(st);
        const char *q = jstr(v, "when");
        if (q) {
            std::string t = quando_leggibile(q);
            if (!t.empty()) r.sotto += (r.sotto.empty() ? "" : "  ") + t;
        }
        e->righe.push_back(r);
    }
    /* Il registro arriva dal piu' vecchio al piu' nuovo; su un pannello
       interessa quello che e' appena successo. */
    std::reverse(e->righe.begin(), e->righe.end());
}


/* ---- la raccolta ----

   Un filo solo, una richiesta alla volta, e fra una e l'altra una pausa. Non
   e' pigrizia: le card di una vista si costruiscono tutte insieme, e se
   ognuna partisse per conto suo il collegamento verso il C6 si troverebbe
   quattro richieste in volo nello stesso istante. E' il guasto che questo
   progetto ha gia' inseguito una volta. */

#define ELENCO_RINFRESCO_S 300

static TaskHandle_t s_el_task = NULL;

static void elenco_risposta_ws(bool ok, cJSON *result, const char *error, void *ctx)
{
    Elenco *e = (Elenco *)ctx;
    bsp_display_lock(0);
    /* Fra la richiesta e la risposta la vista puo' essere stata rifatta: se
       questo elenco non c'e' piu', quello che e' arrivato non riguarda
       nessuno. */
    bool vivo = false;
    for (Elenco *x : s_elenchi) if (x == e) { vivo = true; break; }
    if (vivo) {
        if (ok && result) {
            if (e->tipo == EL_TODO) elenco_da_todo(e, result);
            else                    elenco_da_spesa(e, result);
            e->arrivato = true;
        } else if (e->stato) {
            lv_label_set_text(e->stato, error ? error : "Non arrivato");
        }
        e->in_corso = false;
        e->prossimo = now_s() + ELENCO_RINFRESCO_S;
        elenco_disegna(e);
    }
    bsp_display_unlock();
}

/* Calendario e registro non stanno sul WebSocket: si chiedono all'API HTTP.
   La risposta puo' essere lunga, quindi il buffer sta in PSRAM. */
static void elenco_http(Elenco *e)
{
    char base[HA_URL_MAX];
    if (!ha_http_base(base, sizeof(base))) return;

    time_t ora = (time_t)now_s();
    struct tm tm;
    char da[40], a[40], url[512];

    if (e->tipo == EL_CALENDARIO) {
        gmtime_r(&ora, &tm);
        strftime(da, sizeof(da), "%Y-%m-%dT%H:%M:%SZ", &tm);
        time_t fine = ora + (time_t)e->giorni * 86400;
        gmtime_r(&fine, &tm);
        strftime(a, sizeof(a), "%Y-%m-%dT%H:%M:%SZ", &tm);
    } else {
        time_t inizio = ora - (time_t)e->ore * 3600;
        gmtime_r(&inizio, &tm);
        strftime(da, sizeof(da), "%Y-%m-%dT%H:%M:%SZ", &tm);
        a[0] = 0;
    }

    const size_t N = 24 * 1024;
    char *resp = (char *)heap_caps_malloc(N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) resp = (char *)malloc(N);
    if (!resp) return;

    std::vector<Riga> raccolte;
    bool almeno_una = false;
    for (const std::string &eid : e->eids) {
        if (e->tipo == EL_CALENDARIO)
            snprintf(url, sizeof(url), "%s/api/calendars/%s?start=%s&end=%s",
                     base, eid.c_str(), da, a);
        else
            snprintf(url, sizeof(url), "%s/api/logbook/%s?entity=%s", base, da, eid.c_str());

        int codice = ha_http_get_auth(url, resp, N);
        if (codice != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", e->tipo == EL_CALENDARIO ? "calendario" : "registro", codice);
            continue;
        }
        cJSON *j = cJSON_Parse(resp);
        if (!cJSON_IsArray(j)) { cJSON_Delete(j); continue; }
        almeno_una = true;
        bsp_display_lock(0);
        bool vivo = false;
        for (Elenco *x : s_elenchi) if (x == e) { vivo = true; break; }
        if (vivo) {
            if (e->tipo == EL_CALENDARIO) elenco_da_calendario(e, j);
            else                          elenco_da_registro(e, j);
        }
        bsp_display_unlock();
        cJSON_Delete(j);
    }
    free(resp);
    (void)raccolte;

    bsp_display_lock(0);
    bool vivo = false;
    for (Elenco *x : s_elenchi) if (x == e) { vivo = true; break; }
    if (vivo) {
        e->arrivato = almeno_una;
        if (!almeno_una && e->stato) lv_label_set_text(e->stato, "Non arrivato");
        e->in_corso = false;
        e->prossimo = now_s() + ELENCO_RINFRESCO_S;
        elenco_disegna(e);
    }
    bsp_display_unlock();
}

static void elenco_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
        if (!ha_ws_connected() || !clock_valid()) continue;

        Elenco *da_fare = NULL;
        bsp_display_lock(0);
        for (Elenco *e : s_elenchi) {
            if (e->in_corso || now_s() < e->prossimo) continue;
            da_fare = e;
            e->in_corso = true;
            break;
        }
        bsp_display_unlock();
        if (!da_fare) continue;

        if (da_fare->tipo == EL_TODO || da_fare->tipo == EL_SPESA) {
            std::string body;
            if (da_fare->tipo == EL_TODO)
                body = "\"type\":\"todo/item/list\",\"entity_id\":\"" + da_fare->eid + "\"";
            else
                body = "\"type\":\"shopping_list/items\"";
            if (ha_ws_request(body.c_str(), elenco_risposta_ws, da_fare) < 0) {
                bsp_display_lock(0);
                da_fare->in_corso = false;
                da_fare->prossimo = now_s() + 60;
                bsp_display_unlock();
            }
        } else {
            elenco_http(da_fare);
        }
        /* Una pausa fra un elenco e l'altro: e' il punto di tutto questo. */
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (s_el_task) xTaskNotifyGive(s_el_task);
    }
}

static void elenco_sveglia(void)
{
    if (!s_el_task)
        xTaskCreate(elenco_task, "ll_liste", 6144, NULL, 3, &s_el_task);
    else
        xTaskNotifyGive(s_el_task);
}

/* ---- disegno delle card ---- */

static Elenco *elenco_nuovo(lv_obj_t *parent, const cJSON *card, int w, int h,
                            TipoElenco tipo, const char *titolo_def)
{
    Elenco *e = new Elenco();
    e->tipo = tipo;
    e->larghezza = w;
    e->massimo = (int)jnum(card, "max_items", tipo == EL_REGISTRO ? 8 : 10);
    e->nascondi_fatte = cJSON_IsTrue(cJSON_GetObjectItem(card, "hide_completed"));
    e->giorni = (int)jnum(card, "days_to_show", 7);
    e->ore    = (int)jnum(card, "hours_to_show", 24);

    lv_obj_t *c = mk_card(parent, w, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 2, 0);
    const char *tit = jstr(card, "title");
    mk_label(c, sanitize(tit ? tit : titolo_def), &lv_font_montserrat_22, C_TEXT, w - 28);
    e->stato = mk_label(c, "Caricamento...", &lv_font_montserrat_14, C_TEXT2, w - 28);
    e->box   = mk_box(c, w - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(e->box, LV_FLEX_FLOW_COLUMN);

    s_elenchi.push_back(e);
    elenco_sveglia();
    return e;
}

static void render_todo(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    const char *eid = jstr(card, "entity");
    if (!eid) { render_placeholder(parent, card, w, h); return; }
    Elenco *e = elenco_nuovo(parent, card, w, h, EL_TODO, "Da fare");
    e->eid = eid;
}

static void render_shopping(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    elenco_nuovo(parent, card, w, h, EL_SPESA, "Lista della spesa");
}

static void raccogli_eids(const cJSON *card, std::vector<std::string> &out)
{
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(card, "entities")) {
        const char *id = cJSON_IsString(r) ? r->valuestring : jstr(r, "entity");
        if (id) out.push_back(id);
    }
    if (out.empty()) {
        const cJSON *t = cJSON_GetObjectItem(card, "target");
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(t, "entity_id"))
            if (cJSON_IsString(r)) out.push_back(r->valuestring);
        const cJSON *uno = cJSON_GetObjectItem(t, "entity_id");
        if (out.empty() && cJSON_IsString(uno)) out.push_back(uno->valuestring);
    }
    if (out.empty()) {
        const char *id = jstr(card, "entity");
        if (id) out.push_back(id);
    }
}

static void render_calendar(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    std::vector<std::string> ids;
    raccogli_eids(card, ids);
    if (ids.empty()) { render_placeholder(parent, card, w, h); return; }
    Elenco *e = elenco_nuovo(parent, card, w, h, EL_CALENDARIO, "Calendario");
    e->eids = ids;
}

static void render_logbook(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    std::vector<std::string> ids;
    raccogli_eids(card, ids);
    if (ids.empty()) { render_placeholder(parent, card, w, h); return; }
    Elenco *e = elenco_nuovo(parent, card, w, h, EL_REGISTRO, "Registro");
    e->eids = ids;
}

// ------------------------------------------------------------------ figure
//
// Le quattro card che mostrano un'immagine. Quello che hanno in comune e' il
// modo di prenderla: si chiede a web_image, e se non e' ancora pronta si
// lascia un riquadro con scritto perche'. Quando arriva, la card si rifa' da
// sola - lo stesso schema delle card dell'energia e di "conditional".

struct FiguraUI {
    lv_obj_t    *box = NULL;
    const cJSON *card = NULL;      // vive dentro s_view
    std::string  tipo;
    int w = 0, h = 0;
};
static std::vector<FiguraUI> s_figure;

static void fill_figura(FiguraUI &f);

/* Da dove prende l'immagine questa card.

   "image" e' un indirizzo o un percorso; "camera_image" e' una telecamera, e
   allora l'indirizzo lo si compone come fa Home Assistant. "image_entity" e'
   la forma nuova: un'entita' image.* il cui stato porta l'indirizzo. */
static std::string sorgente_figura(const cJSON *card, bool *dal_vivo)
{
    *dal_vivo = false;
    const char *cam = jstr(card, "camera_image");
    if (cam) {
        *dal_vivo = true;                 // una telecamera cambia: non si tiene da parte
        return std::string("/api/camera_proxy/") + cam;
    }
    const char *ie = jstr(card, "image_entity");
    if (ie) {
        auto it = s_ent.find(ie);
        if (it != s_ent.end() && !it->second.state.empty()) {
            /* Le entita' image.* tengono l'indirizzo negli attributi; se non
               c'e', lo stato stesso spesso lo e'. */
            return it->second.state;
        }
    }
    const char *img = jstr(card, "image");
    if (img) return img;
    return std::string();
}

/* Riempie il contenitore di una card figura. */
static void fill_figura(FiguraUI &f)
{
    bool dal_vivo = false;
    std::string src = sorgente_figura(f.card, &dal_vivo);
    const std::string &t = f.tipo;

    if (src.empty()) {
        mk_label(f.box, "Nessuna immagine indicata", &lv_font_montserrat_14, C_TEXT2, f.w - 20);
        return;
    }

    web_image_t img;
    if (!web_image_prendi(src.c_str(), &img, dal_vivo)) {
        mk_label(f.box, "Scarico l'immagine...", &lv_font_montserrat_14, C_TEXT2, f.w - 20);
        return;
    }

    /* L'immagine sta sotto; quello che si sovrappone va sopra, dentro lo
       stesso contenitore. */
    lv_obj_t *sotto = mk_box(f.box, f.w, f.h);
    lv_obj_set_style_clip_corner(sotto, true, 0);
    lv_obj_t *o = web_image_mostra(sotto, &img, f.w, f.h);
    if (!o) {
        mk_label(f.box, "Immagine non leggibile", &lv_font_montserrat_14, C_TEXT2, f.w - 20);
        return;
    }

    /* Il riquadro si stringe sulla figura.

       Una figura larga 480 dentro una card larga 300 viene rimpicciolita, e
       quello che resta e' piu' piccolo del riquadro: lasciare il riquadro
       della misura della card vuol dire che la fascia col nome finisce in
       fondo al riquadro invece che appoggiata al bordo dell'immagine, con una
       striscia di vuoto in mezzo. Le percentuali di picture-elements hanno lo
       stesso problema: in Home Assistant sono riferite alla figura, non allo
       spazio che le sta intorno. */
    lv_obj_update_layout(o);
    int fw = lv_obj_get_width(o);
    int fh = lv_obj_get_height(o);
    if (fw > 0 && fh > 0) {
        lv_obj_set_size(sotto, fw, fh);
        lv_obj_set_style_pad_all(sotto, 0, 0);
    } else {
        fw = f.w; fh = f.h;
    }
    lv_obj_center(o);

    if (t == "picture") return;        // solo la figura, niente altro

    if (t == "picture-entity") {
        /* Una fascia in basso con nome e stato, come fa Home Assistant. */
        const char *eid = jstr(f.card, "entity");
        if (!eid) return;
        bool mostra_nome  = !cJSON_IsFalse(cJSON_GetObjectItem(f.card, "show_name"));
        bool mostra_stato = !cJSON_IsFalse(cJSON_GetObjectItem(f.card, "show_state"));
        if (!mostra_nome && !mostra_stato) { attach_action(sotto, eid, f.card, LV_EVENT_CLICKED); return; }

        lv_obj_t *fascia = mk_box(sotto, fw, 34);
        lv_obj_align(fascia, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(fascia, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fascia, LV_OPA_60, 0);
        lv_obj_set_flex_flow(fascia, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(fascia, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(fascia, 10, 0);
        if (mostra_nome) {
            const char *nm = jstr(f.card, "name");
            lv_obj_t *ln = mk_label(fascia, "", &lv_font_montserrat_16, C_TEXT, fw / 2);
            bind(eid, B_NAME, ln, NULL, NULL, NULL, nm ? sanitize(nm) : "");
        }
        if (mostra_stato) {
            lv_obj_t *ls = mk_label(fascia, "", &lv_font_montserrat_16, C_TEXT, fw / 2);
            lv_obj_set_style_text_align(ls, LV_TEXT_ALIGN_RIGHT, 0);
            bind(eid, B_VALUE, ls, NULL, NULL, NULL, "");
        }
        attach_action(sotto, eid, f.card, LV_EVENT_CLICKED);
        return;
    }

    if (t == "picture-glance") {
        /* Una riga di icone in basso: quelle che si possono comandare a
           sinistra, le altre a destra, come in Home Assistant. */
        lv_obj_t *fascia = mk_box(sotto, fw, 40);
        lv_obj_align(fascia, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(fascia, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fascia, LV_OPA_60, 0);
        lv_obj_set_flex_flow(fascia, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(fascia, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(fascia, 8, 0);
        lv_obj_set_style_pad_column(fascia, 10, 0);

        const char *titolo = jstr(f.card, "title");
        if (titolo) {
            lv_obj_t *lt = mk_box(sotto, fw, 30);
            lv_obj_align(lt, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_set_style_bg_color(lt, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(lt, LV_OPA_50, 0);
            lv_obj_set_style_pad_hor(lt, 8, 0);
            mk_label(lt, sanitize(titolo), &lv_font_montserrat_16, C_TEXT, fw - 16);
        }

        const cJSON *r;
        int n = 0;
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(f.card, "entities")) {
            const char *eid = cJSON_IsString(r) ? r->valuestring : jstr(r, "entity");
            if (!eid || n++ >= 8) continue;
            lv_obj_t *i = mk_icon(fascia, 28);
            lv_obj_set_style_bg_opa(i, LV_OPA_TRANSP, 0);
            Bind b; b.eid = eid; b.kind = B_TILE; b.obj = i; b.icon = i;
            b.l_name = NULL; b.l_state = NULL;
            s_binds.push_back(b);
            refresh(s_binds.back());
            attach_action(i, eid, r, LV_EVENT_CLICKED);
        }
        return;
    }

    /* picture-elements: gli elementi stanno sopra la figura, collocati in
       percentuale. Si disegnano i tipi che si incontrano davvero su un
       pannello - lo stato scritto, l'icona, l'etichetta, il tasto - e gli
       altri si lasciano stare invece di metterci un segnaposto che
       sporcherebbe la planimetria. */
    const cJSON *el;
    cJSON_ArrayForEach(el, cJSON_GetObjectItem(f.card, "elements")) {
        const char *et = jstr(el, "type");
        if (!et) continue;
        const char *eid = jstr(el, "entity");

        /* La posizione e' in percentuale ("34%"), riferita al centro
           dell'elemento. */
        const cJSON *st = cJSON_GetObjectItem(el, "style");
        double px = 50, py = 50;
        const char *sl = jstr(st, "left");
        const char *sp = jstr(st, "top");
        if (sl) px = atof(sl);
        if (sp) py = atof(sp);
        int cx = (int)(fw * px / 100.0);
        int cy = (int)(fh * py / 100.0);

        lv_obj_t *e = NULL;
        if (!strcmp(et, "state-icon") || !strcmp(et, "icon")) {
            e = mk_icon(sotto, 32);
            lv_obj_set_style_bg_opa(e, LV_OPA_50, 0);
            if (eid) {
                Bind b; b.eid = eid; b.kind = B_TILE; b.obj = e; b.icon = e;
                b.l_name = NULL; b.l_state = NULL;
                s_binds.push_back(b);
                refresh(s_binds.back());
            } else {
                char gl[8];
                const char *nome = jstr(el, "icon");
                lv_label_set_text(lv_obj_get_child(e, 0),
                    nome && mdi_icon_text(nome, gl, sizeof(gl)) ? gl : "");
            }
        } else if (!strcmp(et, "state-label") && eid) {
            e = mk_label(sotto, "", &lv_font_montserrat_16, C_TEXT, fw / 2);
            lv_obj_set_style_bg_color(e, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(e, LV_OPA_50, 0);
            lv_obj_set_style_pad_all(e, 4, 0);
            bind(eid, B_VALUE, e, NULL, NULL, NULL, "");
        } else if (!strcmp(et, "state-badge") && eid) {
            e = mk_label(sotto, "", &lv_font_montserrat_14, C_TEXT, fw / 3);
            lv_obj_set_style_bg_color(e, C_CARD, 0);
            lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(e, 10, 0);
            lv_obj_set_style_pad_all(e, 5, 0);
            bind(eid, B_VALUE, e, NULL, NULL, NULL, "");
        }
        if (!e) continue;
        /* Il punto indicato e' il CENTRO dell'elemento, come in Home
           Assistant: appoggiarci l'angolo in alto a sinistra sposterebbe
           tutto in basso a destra di mezza etichetta. */
        lv_obj_update_layout(e);
        lv_obj_align(e, LV_ALIGN_TOP_LEFT,
                     cx - lv_obj_get_width(e) / 2, cy - lv_obj_get_height(e) / 2);
        if (eid) attach_action(e, eid, el, LV_EVENT_CLICKED);
    }
}

static void render_figura(lv_obj_t *parent, const cJSON *card,
                          const std::string &tipo, int w, int h)
{
    if (h <= 0) h = (int)(w * 9 / 16);        // 16:9, la forma piu' comune
    lv_obj_t *c = mk_card(parent, w, h);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    FiguraUI f;
    f.box = c; f.card = card; f.tipo = tipo; f.w = w; f.h = h;
    s_figure.push_back(f);
    fill_figura(s_figure.back());
}

/* Un'immagine e' arrivata: si rifanno le card che la aspettavano. Chiamata
   dal filo che scarica, quindi prende il lock da se'. */
static void figure_aggiorna(void)
{
    bsp_display_lock(0);
    for (FiguraUI &f : s_figure) {
        if (!f.box) continue;
        lv_obj_clean(f.box);
        fill_figura(f);
    }
    bsp_display_unlock();
}

static void render_card(lv_obj_t *parent, const cJSON *card, int w, int h)
{
    std::string t = card_type(card);
    if (t == "heading")                          render_heading(parent, card, w);
    else if (t == "tile")                        render_tile(parent, card, w, h);
    else if (t == "button")                      render_button(parent, card, w, h);
    else if (t == "entity" || t == "sensor")     render_entity(parent, card, w, h);
    else if (t == "entities")                    render_entities(parent, card, w, h);
    else if (t == "glance")                      render_glance(parent, card, w, h);
    else if (t == "gauge")                       render_gauge(parent, card, w, h);
    else if (t == "markdown")                    render_markdown(parent, card, w, h);
    else if (t == "weather-forecast")            render_weather(parent, card, w, h);
    else if (t == "light")                       render_light(parent, card, w, h);
    else if (t == "thermostat")                  render_thermostat(parent, card, w, h);
    else if (t == "media-control")               render_media(parent, card, w, h);
    else if (t == "clock")                       render_clock(parent, card, w, h);
    else if (t == "alert")                       render_alert(parent, card, w, h);
    else if (t == "toggle-group")                render_toggle_group(parent, card, w, h);
    else if (t == "humidifier")                  render_humidifier(parent, card, w, h);
    else if (t == "alarm-panel")                 render_alarm_panel(parent, card, w, h);
    else if (t == "conditional" || t == "entity-filter") {
        std::vector<std::string> guarda;
        entita_delle_condizioni(cJSON_GetObjectItem(card, "conditions"), guarda);
        entita_delle_condizioni(cJSON_GetObjectItem(card, "state_filter"), guarda);
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItem(card, "entities")) {
            const char *e = cJSON_IsString(row) ? row->valuestring : jstr(row, "entity");
            if (e) guarda.push_back(e);
        }
        render_dyn(parent, card, t, w, h, guarda);
    }
    else if (is_energy_card(t))                  render_energy_card(parent, card, t, w, h);
    else if (t == "statistic")                   render_statistic(parent, card, w, h);
    else if (t == "todo-list")                   render_todo(parent, card, w, h);
    else if (t == "shopping-list")               render_shopping(parent, card, w, h);
    else if (t == "calendar")                    render_calendar(parent, card, w, h);
    else if (t == "logbook")                     render_logbook(parent, card, w, h);
    else if (t == "picture" || t == "picture-entity" ||
             t == "picture-glance" || t == "picture-elements")
                                                 render_figura(parent, card, t, w, h);
    else if (chart_card(card))                   render_chart(parent, card, w, h);
    else if (t == "vertical-stack")              render_stack(parent, card, w, false, 1);
    else if (t == "horizontal-stack")            render_stack(parent, card, w, true, 1);
    else if (t == "grid") {
        const cJSON *cols = cJSON_GetObjectItem(card, "columns");
        render_stack(parent, card, w, false, cJSON_IsNumber(cols) ? cols->valueint : 3);
    }
    else                                         render_placeholder(parent, card, w, h);
}

// ------------------------------------------------------------------ viste

static void build_sections(lv_obj_t *parent, int w)
{
    const cJSON *sections = cJSON_GetObjectItem(s_view, "sections");
    int nsec = cJSON_GetArraySize(sections);
    const cJSON *mc = cJSON_GetObjectItem(s_view, "max_columns");
    int maxc = cJSON_IsNumber(mc) ? mc->valueint : 4;
    int ncol = LV_CLAMP(1, w / SECTION_MIN_W, maxc);
    if (nsec > 0 && ncol > nsec) ncol = nsec;
    int sw = (w - GAP * 2 * (ncol - 1)) / ncol;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(parent, GAP * 2, 0);
    lv_obj_set_style_pad_row(parent, GAP * 2, 0);

    const cJSON *sec;
    cJSON_ArrayForEach(sec, sections) {
        const cJSON *span = cJSON_GetObjectItem(sec, "column_span");
        int sp = cJSON_IsNumber(span) ? LV_CLAMP(1, span->valueint, ncol) : 1;
        int secw = sw * sp + GAP * 2 * (sp - 1);
        lv_obj_t *box = mk_box(parent, secw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(box, GAP, 0);
        lv_obj_set_style_pad_column(box, GAP, 0);
        int cu = (secw - GAP * 11) / 12;                   // una colonna della griglia
        const cJSON *card;
        cJSON_ArrayForEach(card, cJSON_GetObjectItem(sec, "cards")) {
            int cols = card_columns(card);
            int cw = cu * cols + GAP * (cols - 1);
            if (cols == 12) cw = secw;
            render_card(box, card, cw, card_rows_height(card));
        }
    }
}

/* Vista "masonry" (quella classica): colonne da ~350 px, ogni card va nella
   colonna piu' bassa, come fa il frontend. */
static void build_masonry(lv_obj_t *parent, int w)
{
    const cJSON *cards = cJSON_GetObjectItem(s_view, "cards");
    int ncol = LV_CLAMP(1, w / 350, 4);
    int n = cJSON_GetArraySize(cards);
    if (n > 0 && ncol > n) ncol = n;
    int cw = (w - GAP * (ncol - 1)) / ncol;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(parent, GAP, 0);
    std::vector<lv_obj_t *> cols;
    for (int i = 0; i < ncol; i++) {
        lv_obj_t *c = mk_box(parent, cw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, GAP, 0);
        cols.push_back(c);
    }
    const cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        lv_obj_t *best = cols[0];
        lv_coord_t best_h = 0x7fff;
        for (lv_obj_t *c : cols) {
            lv_obj_update_layout(c);
            lv_coord_t hh = lv_obj_get_height(c);
            if (hh < best_h) { best_h = hh; best = c; }
        }
        render_card(best, card, cw, card_rows_height(card));
    }
}

static void build_panel(lv_obj_t *parent, int w)
{
    const cJSON *cards = cJSON_GetObjectItem(s_view, "cards");
    const cJSON *first = cJSON_GetArrayItem(cards, 0);
    if (first) render_card(parent, first, w, 0);
}

void ll_energy_refresh(void)
{
    energy_model_invalida(true);
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}

void ll_build(lv_obj_t *parent, int w)
{
    ll_unbind();
    web_image_on_arrivo(figure_aggiorna);
    if (!s_view) return;
    lv_obj_set_style_bg_color(parent, C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    const char *vt = jstr(s_view, "type");
    if (cJSON_GetObjectItem(s_view, "sections")) build_sections(parent, w);
    else if (vt && !strcmp(vt, "panel"))         build_panel(parent, w);
    else                                         build_masonry(parent, w);
    ESP_LOGI(TAG, "vista costruita: %u widget collegati", (unsigned)s_binds.size());
}

void ll_unbind(void)
{
    /* Le iscrizioni aperte per le card di testo si chiudono qui: se no HA
       continuerebbe a mandare aggiornamenti per card che non esistono piu'. */
    for (MdCard *m : s_md) {
        if (m->sub > 0) ha_ws_unsubscribe(m->sub);
        delete m;
    }
    s_md.clear();
    for (WxCard *c : s_wx) {
        if (c->sub > 0) ha_ws_unsubscribe(c->sub);
        delete c;
    }
    s_wx.clear();
    for (Orologio *o : s_orologi) { if (o->t) lv_timer_del(o->t); delete o; }
    s_orologi.clear();
    for (Gruppo *g : s_gruppi)  delete g;
    s_gruppi.clear();
    for (Umid *u : s_umid)      delete u;
    s_umid.clear();
    for (Allarme *a : s_allarmi) delete a;
    s_allarmi.clear();
    s_dyn.clear();
    s_num.clear();
    /* Gli elenchi si buttano qui. Una risposta che arriva dopo non trova piu'
       il suo elenco fra quelli vivi e si ferma da sola. */
    for (Elenco *e : s_elenchi) delete e;
    s_elenchi.clear();
    s_figure.clear();
    s_binds.clear();
    s_encards.clear();          // i contenitori muoiono con la vista
    s_gauges.clear();
    s_luci.clear();
    s_termo.clear();
    s_media.clear();
    s_cmd_eid.clear();
    s_actions.clear();
    s_cui.clear();
}

// ------------------------------------------------------------------ modello

static void collect(const cJSON *node)
{
    if (cJSON_IsArray(node)) {
        const cJSON *c;
        cJSON_ArrayForEach(c, node) collect(c);
        return;
    }
    if (!cJSON_IsObject(node)) return;
    const cJSON *c;
    cJSON_ArrayForEach(c, node) {
        if (c->string && !strcmp(c->string, "entity") && cJSON_IsString(c)) {
            std::string id = c->valuestring;
            if (id.find('.') != std::string::npos &&
                std::find(s_ids.begin(), s_ids.end(), id) == s_ids.end() && s_ids.size() < 96)
                s_ids.push_back(id);
        } else if (c->string && !strcmp(c->string, "entities") && cJSON_IsArray(c)) {
            const cJSON *e;
            cJSON_ArrayForEach(e, c) {
                if (cJSON_IsString(e)) {
                    std::string id = e->valuestring;
                    if (id.find('.') != std::string::npos &&
                        std::find(s_ids.begin(), s_ids.end(), id) == s_ids.end() && s_ids.size() < 96)
                        s_ids.push_back(id);
                } else collect(e);
            }
        } else {
            collect(c);
        }
    }
}

/* Entita' seguite oltre a quelle della vista (sovrimpressione dello standby). */
static std::vector<std::string> s_extra;

static void rebuild_ids_c(void)
{
    s_ids_c.clear();
    for (auto &s : s_ids) s_ids_c.push_back(s.c_str());
    for (auto &s : s_extra)
        if (std::find(s_ids.begin(), s_ids.end(), s) == s_ids.end()) s_ids_c.push_back(s.c_str());
    s_ids_c.push_back(NULL);
}

void ll_set_extra_entities(const char *const *ids)
{
    s_extra.clear();
    for (int i = 0; ids && ids[i]; i++) s_extra.push_back(ids[i]);
    rebuild_ids_c();
}

bool ll_entity_text(const char *entity_id, char *name, size_t name_sz, char *value, size_t value_sz)
{
    auto it = s_ent.find(entity_id);
    if (it == s_ent.end()) return false;
    std::string n = sanitize(display_name(entity_id, "").c_str());
    std::string v = format_state(entity_id);
    if (name)  strlcpy(name, n.c_str(), name_sz);
    if (value) strlcpy(value, v.c_str(), value_sz);
    return true;
}

bool ll_set_config(const cJSON *config, int view, char *err, size_t err_sz)
{
    if (cJSON_GetObjectItem(config, "strategy")) {
        snprintf(err, err_sz, "Dashboard generata automaticamente da HA:\n"
                 "aprila in HA e scegli \"Prendi il controllo\".");
        return false;
    }
    const cJSON *views = cJSON_GetObjectItem(config, "views");
    int nv = cJSON_GetArraySize(views);
    if (nv == 0) { snprintf(err, err_sz, "La dashboard non contiene viste."); return false; }
    if (view < 0 || view >= nv) view = 0;
    const cJSON *v = cJSON_GetArrayItem(views, view);
    if (cJSON_GetObjectItem(v, "strategy")) {
        snprintf(err, err_sz, "La vista %d e' generata automaticamente: non supportata.", view);
        return false;
    }
    /* HA rimanda la configurazione a ogni riconnessione: se non e' cambiata
       tengo vista e dati dei grafici, senza riscaricare lo storico. */
    if (s_view && cJSON_Compare(s_view, v, true)) return true;
    if (s_view) cJSON_Delete(s_view);
    s_view = cJSON_Duplicate(v, true);

    s_cgen++;
    s_cspec.clear();
    collect_charts(cJSON_GetObjectItem(s_view, "sections"));
    collect_charts(cJSON_GetObjectItem(s_view, "cards"));
    s_cdata.assign(s_cspec.size(), ChartData());

    /* L'energia si accende solo se la vista la mostra davvero. */
    bool en = cerca_energia(cJSON_GetObjectItem(s_view, "sections")) ||
              cerca_energia(cJSON_GetObjectItem(s_view, "cards"));
    energy_model_serve(en);
    if (en) {
        energy_model_on_change(energia_aggiorna);
        energy_model_invalida(true);      // vista nuova: preferenze e dati da rileggere
    }

    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);

    s_ids.clear();
    collect(s_view);
    rebuild_ids_c();
    const char *title = jstr(s_view, "title");
    ESP_LOGI(TAG, "vista %d '%s': %u entita'", view, title ? title : "", (unsigned)s_ids.size());
    return true;
}

bool ll_has_view(void) { return s_view != NULL; }

char *ll_view_json(void)
{
    return s_view ? cJSON_PrintUnformatted(s_view) : NULL;
}

const char *const *ll_entity_ids(void)
{
    if (s_ids_c.empty()) rebuild_ids_c();
    return s_ids_c.data();
}

void ll_entity_update(const char *entity_id, const char *state, const cJSON *attrs)
{
    Entity &e = s_ent[entity_id];
    if (state) e.state = state;
    if (attrs) {
        const char *s;
        if ((s = jstr(attrs, "friendly_name")))       e.name = s;
        if ((s = jstr(attrs, "unit_of_measurement"))) e.unit = sanitize(s);
        if ((s = jstr(attrs, "device_class")))        e.device_class = s;
        if ((s = jstr(attrs, "icon")))                e.icon = s;
        {
            const cJSON *t = cJSON_GetObjectItem(attrs, "temperature");
            if (cJSON_IsNumber(t)) {
                char tb[16];
                snprintf(tb, sizeof(tb), "%d", (int)lround(t->valuedouble));
                e.temperatura = tb;
                /* Per il meteo "temperature" e' quella di adesso, per un
                   termostato e' quella a cui si vuole arrivare. */
                e.temp_set = t->valuedouble;
            }
        }
        /* Quello che serve alle card che comandano. */
        const cJSON *n;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "brightness")))          e.brightness = n->valuedouble;
        else if (cJSON_IsNull(n))                                                  e.brightness = -1;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "current_temperature"))) e.temp_now = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "min_temp")))            e.temp_min = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "max_temp")))            e.temp_max = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "target_temp_step")))    e.temp_step = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "humidity")))            e.umidita_set = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "current_humidity")))    e.umidita_ora = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "min_humidity")))        e.umidita_min = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "max_humidity")))        e.umidita_max = n->valuedouble;
        if ((s = jstr(attrs, "mode")))             e.modo = s;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "volume_level")))        e.volume = n->valuedouble;
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(attrs, "supported_features")))  e.funzioni = n->valueint;
        if ((s = jstr(attrs, "hvac_action")))      e.azione = s;
        if ((s = jstr(attrs, "media_title")))      e.titolo = s;
        if ((s = jstr(attrs, "media_artist")))     e.artista = s;
        else if ((s = jstr(attrs, "media_series_title"))) e.artista = s;
        else if ((s = jstr(attrs, "media_channel")))      e.artista = s;
    }
    for (Bind &b : s_binds)
        if (b.eid == entity_id) refresh(b);
    /* I gruppi contano piu' entita' insieme, quindi non hanno un bind proprio:
       si rinfrescano tutti quelli che contengono questa. */
    for (Gruppo *g : s_gruppi)
        for (const std::string &e : g->eid)
            if (e == entity_id) { gruppo_refresh(g); break; }
    /* E le card che cambiano forma, non solo scritte. */
    dyn_aggiorna(entity_id);
}

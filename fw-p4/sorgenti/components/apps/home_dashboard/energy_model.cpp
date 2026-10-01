#include "energy_model.h"
#include "ha_ws.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"

static const char *TAG = "energia";

/* Ogni quanto rinfrescare da solo: l'energia non cambia al secondo, e ogni
   richiesta e' traffico sul collegamento SDIO verso il C6, che e' la cosa
   piu' delicata di questo pannello. */
#define RINFRESCO_S      300
#define RIPROVA_S        60

static EnergyModel        s_m;
static energy_model_cb_t  s_cb = NULL;
static bool               s_serve = false;      // c'e' una card dell'energia nella vista?
static bool               s_vuole_prefs = true;
static bool               s_vuole_dati = false;
/* L'esempio nel log si stampa una volta per configurazione: serve a capire
   cosa manda questo Home Assistant, e una volta saputo ripeterlo ogni cinque
   minuti sarebbe solo rumore. Torna in conto quando la configurazione cambia,
   perche' allora cambiano anche gli id e le loro unita'. */
static bool               s_mostra_esempio = true;
static bool               s_in_volo = false;
static int64_t            s_riprova_dopo = 0;
static uint32_t           s_gen = 0;            // cresce a ogni cambio di periodo

/* Gli id da chiedere, e a cosa servono. Un id puo' comparire in piu' ruoli
   (chi usa lo stesso contatore per due cose), quindi si tiene una lista di
   coppie e non una mappa. */
enum Ruolo {
    R_RETE_PRESA, R_RETE_IMMESSA, R_SOLARE,
    R_BATT_SCARICA, R_BATT_CARICA, R_GAS, R_ACQUA,
    R_COSTO, R_COMPENSO, R_DISPOSITIVO,
};
/* "k" porta il valore nell'unita' in cui lo mostriamo: per l'energia i kWh,
   per gas e acqua l'unita' che usa l'utente (li' vale 1 e si mostra la sua). */
struct Richiesto { std::string id; Ruolo ruolo; int voce; double k = 1; };
static std::vector<Richiesto> s_rich;
static bool s_vuole_meta = false;

// ------------------------------------------------------------------ tempo

static int64_t ora_s(void) { return (int64_t)time(NULL); }
static bool orologio_ok(void) { return ora_s() > 1700000000; }

static std::string iso_utc(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

/* HA recente manda i millisecondi, le versioni vecchie una stringa ISO. */
static int64_t json_time_s(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        return (int64_t)(d > 1e11 ? d / 1000.0 : d);
    }
    if (cJSON_IsString(v)) {
        int Y, M, D, h, m, s;
        if (sscanf(v->valuestring, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
            int64_t days = 0;
            for (int y = 1970; y < Y; y++)
                days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
            static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            for (int i = 0; i < M - 1; i++)
                days += md[i] + (i == 1 && (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0)));
            days += D - 1;
            return days * 86400 + h * 3600 + m * 60 + s;
        }
    }
    return 0;
}

/* Mezzanotte locale del giorno che contiene t. */
static int64_t mezzanotte(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;                       // decide l'ora legale da se'
    time_t m = mktime(&tm);
    return m == (time_t)-1 ? t : (int64_t)m;
}

const char *energy_periodo_nome(EnergyPeriodo p)
{
    switch (p) {
        case EN_OGGI:      return "Oggi";
        case EN_SETTIMANA: return "Settimana";
        case EN_MESE:      return "Mese";
        default:           return "Anno";
    }
}

/* La finestra del periodo scelto, piu' il passo con cui chiedere i dati.

   HA le calcola sul calendario locale, non sulle ultime N ore: "questa
   settimana" comincia lunedi', "questo mese" il primo del mese. Un pannello al
   muro che dicesse "ultimi 7 giorni" quando l'utente legge "settimana" sarebbe
   un pannello che mente educatamente. */
static const char *finestra(EnergyPeriodo p, int64_t *t0, int64_t *t1,
                            int *npunti, int64_t *passo)
{
    int64_t oggi = mezzanotte(ora_s());
    time_t tt = (time_t)oggi;
    struct tm tm;
    localtime_r(&tt, &tm);

    switch (p) {
        case EN_OGGI:
            *t0 = oggi; *t1 = oggi + 86400;
            *npunti = 24; *passo = 3600;
            return "hour";

        case EN_SETTIMANA: {
            int dow = (tm.tm_wday + 6) % 7;           // 0 = lunedi'
            *t0 = oggi - (int64_t)dow * 86400;
            *t1 = *t0 + 7 * 86400;
            *npunti = 7; *passo = 86400;
            return "day";
        }

        case EN_MESE: {
            struct tm p0 = tm;
            p0.tm_mday = 1; p0.tm_isdst = -1;
            *t0 = (int64_t)mktime(&p0);
            struct tm p1 = p0;
            p1.tm_mon += 1;                            // mktime normalizza dicembre
            p1.tm_isdst = -1;
            *t1 = (int64_t)mktime(&p1);
            *npunti = (int)((*t1 - *t0 + 43200) / 86400);
            *passo = 86400;
            return "day";
        }

        default: {
            struct tm p0 = tm;
            p0.tm_mday = 1; p0.tm_mon = 0; p0.tm_isdst = -1;
            *t0 = (int64_t)mktime(&p0);
            struct tm p1 = p0;
            p1.tm_year += 1; p1.tm_isdst = -1;
            *t1 = (int64_t)mktime(&p1);
            *npunti = 12; *passo = 0;                  // i mesi non sono tutti uguali
            return "month";
        }
    }
}

/* In quale fascia cade un istante. Per ore e giorni basta una divisione; per
   i mesi no, perche' durano diverso, e allora si guarda il calendario. */
static int fascia(const EnergyModel &m, int64_t t)
{
    if (t < m.t0 || t >= m.t1) return -1;
    if (m.passo_s > 0) {
        int i = (int)((t - m.t0) / m.passo_s);
        return (i >= 0 && i < m.npunti) ? i : -1;
    }
    time_t a = (time_t)m.t0, b = (time_t)t;
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    int i = (tb.tm_year - ta.tm_year) * 12 + (tb.tm_mon - ta.tm_mon);
    return (i >= 0 && i < m.npunti) ? i : -1;
}

// ------------------------------------------------------------------ preferenze

static void aggiungi(const char *id, Ruolo r, int voce = -1)
{
    if (!id || !*id) return;
    s_rich.push_back({id, r, voce});
}

/* Una sorgente della rete puo' arrivare in due forme.

   Le versioni recenti di Home Assistant mettono i due contatori direttamente
   dentro la sorgente ("stat_energy_from" e "stat_energy_to"). Quelle piu'
   vecchie li mettevano in due elenchi, "flow_from" e "flow_to", perche' si
   potevano avere piu' contatori per lo stesso allacciamento. Le due forme
   esistono ancora entrambe in giro, e un pannello che ne conoscesse una sola
   funzionerebbe a casa di chi l'ha scritto e non a casa degli altri. */
static void leggi_rete(const cJSON *src)
{
    const cJSON *ff = cJSON_GetObjectItem(src, "flow_from");
    const cJSON *ft = cJSON_GetObjectItem(src, "flow_to");
    bool a_elenchi = cJSON_IsArray(ff) || cJSON_IsArray(ft);

    if (a_elenchi) {
        const cJSON *f;
        cJSON_ArrayForEach(f, ff) {
            const cJSON *s = cJSON_GetObjectItem(f, "stat_energy_from");
            if (cJSON_IsString(s)) { aggiungi(s->valuestring, R_RETE_PRESA); s_m.c_e_rete = true; }
            const cJSON *c = cJSON_GetObjectItem(f, "stat_cost");
            if (cJSON_IsString(c)) { aggiungi(c->valuestring, R_COSTO); s_m.c_e_costo = true; }
        }
        cJSON_ArrayForEach(f, ft) {
            const cJSON *s = cJSON_GetObjectItem(f, "stat_energy_to");
            if (cJSON_IsString(s)) { aggiungi(s->valuestring, R_RETE_IMMESSA); s_m.c_e_rete = true; }
            const cJSON *c = cJSON_GetObjectItem(f, "stat_compensation");
            if (cJSON_IsString(c)) { aggiungi(c->valuestring, R_COMPENSO); s_m.c_e_costo = true; }
        }
        return;
    }

    const cJSON *s;
    if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_from")))
        { aggiungi(s->valuestring, R_RETE_PRESA); s_m.c_e_rete = true; }
    if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_to")))
        { aggiungi(s->valuestring, R_RETE_IMMESSA); s_m.c_e_rete = true; }
    if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_cost")))
        { aggiungi(s->valuestring, R_COSTO); s_m.c_e_costo = true; }
    if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_compensation")))
        { aggiungi(s->valuestring, R_COMPENSO); s_m.c_e_costo = true; }
}

static void nomina(const cJSON *src, const char *id, const char *fallback)
{
    const cJSON *n = cJSON_GetObjectItem(src, "name");
    EnergyVoce v;
    v.id   = id ? id : "";
    v.nome = (cJSON_IsString(n) && n->valuestring[0]) ? n->valuestring : fallback;
    s_m.sorgenti.push_back(v);
}

static void leggi_prefs(const cJSON *res)
{
    s_rich.clear();
    s_m.sorgenti.clear();
    s_m.dispositivi.clear();
    s_m.c_e_rete = s_m.c_e_solare = s_m.c_e_batteria = false;
    s_m.c_e_gas = s_m.c_e_acqua = s_m.c_e_dispositivi = s_m.c_e_costo = false;

    const cJSON *srcs = cJSON_GetObjectItem(res, "energy_sources");
    const cJSON *src;
    cJSON_ArrayForEach(src, srcs) {
        const cJSON *ty = cJSON_GetObjectItem(src, "type");
        if (!cJSON_IsString(ty)) continue;
        const char *t = ty->valuestring;
        const cJSON *s;

        if (!strcmp(t, "grid")) {
            leggi_rete(src);
            nomina(src, "", "Rete");
        } else if (!strcmp(t, "solar")) {
            if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_from"))) {
                aggiungi(s->valuestring, R_SOLARE);
                s_m.c_e_solare = true;
                nomina(src, s->valuestring, "Fotovoltaico");
            }
        } else if (!strcmp(t, "battery")) {
            bool c = false;
            if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_from")))
                { aggiungi(s->valuestring, R_BATT_SCARICA); c = true; }
            if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_to")))
                { aggiungi(s->valuestring, R_BATT_CARICA); c = true; }
            if (c) { s_m.c_e_batteria = true; nomina(src, "", "Batteria"); }
        } else if (!strcmp(t, "gas")) {
            if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_from"))) {
                aggiungi(s->valuestring, R_GAS);
                s_m.c_e_gas = true;
                nomina(src, s->valuestring, "Gas");
            }
        } else if (!strcmp(t, "water")) {
            if (cJSON_IsString(s = cJSON_GetObjectItem(src, "stat_energy_from"))) {
                aggiungi(s->valuestring, R_ACQUA);
                s_m.c_e_acqua = true;
                nomina(src, s->valuestring, "Acqua");
            }
        }
        /* Un tipo che non conosciamo non e' un errore: HA puo' aggiungerne, e
           il resto della pagina deve continuare a funzionare lo stesso. */
    }

    /* I consumi per dispositivo. HA ne tiene due elenchi: quello dell'energia
       e, nelle versioni recenti, uno dell'acqua. Si leggono tutti e due. */
    for (const char *chiave : {"device_consumption", "device_consumption_water"}) {
        const cJSON *devs = cJSON_GetObjectItem(res, chiave);
        const cJSON *d;
        cJSON_ArrayForEach(d, devs) {
            const cJSON *s = cJSON_GetObjectItem(d, "stat_consumption");
            if (!cJSON_IsString(s)) continue;
            const cJSON *n = cJSON_GetObjectItem(d, "name");
            EnergyVoce v;
            v.id   = s->valuestring;
            v.nome = (cJSON_IsString(n) && n->valuestring[0]) ? n->valuestring : s->valuestring;
            s_m.dispositivi.push_back(v);
            aggiungi(v.id.c_str(), R_DISPOSITIVO, (int)s_m.dispositivi.size() - 1);
        }
    }
    s_m.c_e_dispositivi = !s_m.dispositivi.empty();

    s_m.prefs_lette = true;
    ESP_LOGI(TAG, "preferenze lette: rete=%d solare=%d batteria=%d gas=%d acqua=%d "
                  "dispositivi=%u, %u statistiche da chiedere",
             s_m.c_e_rete, s_m.c_e_solare, s_m.c_e_batteria, s_m.c_e_gas, s_m.c_e_acqua,
             (unsigned)s_m.dispositivi.size(), (unsigned)s_rich.size());
}

/* Da qualunque unita' di energia ai kWh.

   Questa funzione e' il motivo per cui il pannello non e' utilizzabile solo a
   casa di chi l'ha scritto. I sensori di energia in giro sono in Wh, in kWh o
   in MWh a seconda di chi ha fatto l'integrazione, e Home Assistant li tiene
   cosi' come sono: chiedere le statistiche senza chiedere anche l'unita' vuol
   dire sommare numeri e chiamarli come viene.

   Si e' visto subito: i due contatori di prova erano in Wh, e il pannello
   annunciava 11393 kWh consumati in un giorno - un numero da quartiere, non da
   casa - semplicemente perche' nessuno aveva chiesto in che unita' fossero.

   Un'unita' che non si riconosce vale 1: meglio un numero non convertito che
   un numero moltiplicato a caso. */
static double a_kwh(const char *u)
{
    if (!u || !*u) return 1;
    if (!strcasecmp(u, "Wh"))  return 0.001;
    if (!strcasecmp(u, "kWh")) return 1;
    if (!strcasecmp(u, "MWh")) return 1000;
    if (!strcasecmp(u, "GWh")) return 1000000;
    if (!strcasecmp(u, "J"))   return 1.0 / 3600000.0;
    if (!strcasecmp(u, "kJ"))  return 1.0 / 3600.0;
    if (!strcasecmp(u, "MJ"))  return 1.0 / 3.6;
    if (!strcasecmp(u, "GJ"))  return 1000.0 / 3.6;
    ESP_LOGW(TAG, "unita' di energia sconosciuta \"%s\": lascio il numero com'e'", u);
    return 1;
}

static bool ruolo_energia(Ruolo r)
{
    return r == R_RETE_PRESA || r == R_RETE_IMMESSA || r == R_SOLARE ||
           r == R_BATT_SCARICA || r == R_BATT_CARICA || r == R_DISPOSITIVO;
}

/* Le unita' che Home Assistant dichiara per le statistiche chieste.

   Il nome del campo e' cambiato fra le versioni, percio' si prova in ordine:
   quella in cui le statistiche sono *salvate* viene prima, perche' e' quella
   dei numeri che arrivano; le altre sono ripieghi per gli HA piu' vecchi. */
static void leggi_meta(const cJSON *res)
{
    const cJSON *m;
    cJSON_ArrayForEach(m, res) {
        const cJSON *sid = cJSON_GetObjectItem(m, "statistic_id");
        if (!cJSON_IsString(sid)) continue;
        const char *u = NULL;
        for (const char *chiave : {"statistics_unit_of_measurement",
                                   "unit_of_measurement",
                                   "display_unit_of_measurement"}) {
            const cJSON *v = cJSON_GetObjectItem(m, chiave);
            if (cJSON_IsString(v) && v->valuestring[0]) { u = v->valuestring; break; }
        }
        for (Richiesto &r : s_rich) {
            if (r.id != sid->valuestring) continue;
            if (ruolo_energia(r.ruolo)) {
                r.k = a_kwh(u);
            } else if (r.ruolo == R_GAS) {
                r.k = 1;
                s_m.unita_gas = u ? u : "";
            } else if (r.ruolo == R_ACQUA) {
                r.k = 1;
                s_m.unita_acqua = u ? u : "";
            }
        }
    }
    ESP_LOGI(TAG, "unita' delle statistiche lette");
}

// ------------------------------------------------------------------ conti

static double pct(double parte, double tutto)
{
    if (tutto <= 0.0001) return -1;
    double v = parte / tutto * 100.0;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void ricava(void)
{
    EnergyModel &m = s_m;

    /* Il consumo di casa non e' un sensore: si ricava, ed e' la stessa formula
       della pagina Energia di HA. Tutto cio' che entra in casa meno tutto cio'
       che ne esce. */
    m.casa = m.rete_presa - m.rete_immessa + m.solare + m.batteria_scarica - m.batteria_carica;
    if (m.casa < 0) m.casa = 0;      // capita con contatori che si azzerano

    /* Autosufficienza: quanta parte di quel che consumi NON hai comprato. */
    m.autosufficienza = m.casa > 0 ? pct(m.casa - m.rete_presa, m.casa) : -1;

    /* Del sole prodotto, quanto e' rimasto in casa invece di finire in rete. */
    m.solare_usato = m.solare > 0 ? pct(m.solare - m.rete_immessa, m.solare) : -1;

    /* Neutralita': quanto pesa cio' che immetti sul totale scambiato con la
       rete. Sopra il 50% dai alla rete piu' di quanto le chiedi. */
    double scambio = m.rete_presa + m.rete_immessa;
    m.neutralita = scambio > 0 ? pct(m.rete_immessa, scambio) : -1;

    m.dati_pronti = true;
    m.aggiornato_s = ora_s();
}

static void azzera_totali(void)
{
    EnergyModel &m = s_m;
    m.rete_presa = m.rete_immessa = m.solare = 0;
    m.batteria_scarica = m.batteria_carica = 0;
    m.gas = m.acqua = m.costo = m.compenso = 0;
    m.casa = 0;
    m.autosufficienza = m.solare_usato = m.neutralita = -1;

    auto azz = [&](std::vector<float> &v) { v.assign(m.npunti, 0.0f); };
    azz(m.g_rete_presa); azz(m.g_rete_immessa); azz(m.g_solare);
    azz(m.g_batt_scarica); azz(m.g_batt_carica); azz(m.g_gas); azz(m.g_acqua);

    for (auto &v : s_m.sorgenti)    { v.totale = 0; v.visto = false; }
    for (auto &v : s_m.dispositivi) { v.totale = 0; v.visto = false; }
}

/* Dove finisce ogni ruolo: un totale e, quando ha senso, una serie. */
static void versa(Ruolo r, int voce, int f, double val)
{
    EnergyModel &m = s_m;
    double *tot = NULL;
    std::vector<float> *serie = NULL;

    switch (r) {
        case R_RETE_PRESA:   tot = &m.rete_presa;       serie = &m.g_rete_presa;   break;
        case R_RETE_IMMESSA: tot = &m.rete_immessa;     serie = &m.g_rete_immessa; break;
        case R_SOLARE:       tot = &m.solare;           serie = &m.g_solare;       break;
        case R_BATT_SCARICA: tot = &m.batteria_scarica; serie = &m.g_batt_scarica; break;
        case R_BATT_CARICA:  tot = &m.batteria_carica;  serie = &m.g_batt_carica;  break;
        case R_GAS:          tot = &m.gas;              serie = &m.g_gas;          break;
        case R_ACQUA:        tot = &m.acqua;            serie = &m.g_acqua;        break;
        case R_COSTO:        tot = &m.costo;            break;
        case R_COMPENSO:     tot = &m.compenso;         break;
        case R_DISPOSITIVO:
            if (voce >= 0 && voce < (int)m.dispositivi.size()) {
                m.dispositivi[voce].totale += val;
                m.dispositivi[voce].visto = true;
            }
            return;
    }
    if (tot) *tot += val;
    if (serie && f >= 0 && f < (int)serie->size()) (*serie)[f] += (float)val;
}

static void leggi_statistiche(const cJSON *res)
{
    azzera_totali();

    bool primo = s_mostra_esempio;
    s_mostra_esempio = false;
    for (const Richiesto &rq : s_rich) {
        const cJSON *righe = cJSON_GetObjectItem(res, rq.id.c_str());
        if (!cJSON_IsArray(righe)) continue;
        if (primo) {
            /* Quali campi manda davvero questo HA, e qual e' la riga piu'
               grossa: quando i numeri non tornano, e' da qui che si capisce
               se sbaglia il pannello o se il dato arriva gia' cosi'. */
            char *d = cJSON_PrintUnformatted(cJSON_GetArrayItem(righe, 0));
            double mx = 0; int64_t mxt = 0; double somma = 0;
            const cJSON *q;
            cJSON_ArrayForEach(q, righe) {
                const cJSON *v = cJSON_GetObjectItem(q, "change");
                if (!cJSON_IsNumber(v)) continue;
                somma += v->valuedouble;
                if (fabs(v->valuedouble) > fabs(mx)) {
                    mx = v->valuedouble;
                    mxt = json_time_s(cJSON_GetObjectItem(q, "start"));
                }
            }
            ESP_LOGI(TAG, "esempio %s (%d righe): %s", rq.id.c_str(),
                     cJSON_GetArraySize(righe), d ? d : "?");
            time_t tt = (time_t)mxt; struct tm tm; localtime_r(&tt, &tm);
            ESP_LOGI(TAG, "  somma grezza=%.3f  riga piu' grossa=%.3f alle %02d:%02d "
                          "(moltiplicate per %.4g per averle in kWh)",
                     somma, mx, tm.tm_hour, tm.tm_min, rq.k);
            free(d);
            primo = false;
        }
        const cJSON *r;
        cJSON_ArrayForEach(r, righe) {
            /* "change" e' quanto e' cresciuto il contatore in quella fascia: e'
               il valore giusto per l'energia, perche' i contatori sono
               cumulativi e la differenza fra due letture e' il consumo. Se HA
               non lo manda (statistiche non cumulative) si ripiega su "sum" e
               poi su "state", come fa la pagina Energia. */
            const cJSON *v = cJSON_GetObjectItem(r, "change");
            if (!cJSON_IsNumber(v)) v = cJSON_GetObjectItem(r, "sum");
            if (!cJSON_IsNumber(v)) v = cJSON_GetObjectItem(r, "state");
            if (!cJSON_IsNumber(v)) continue;
            double val = v->valuedouble * rq.k;
            if (!isfinite(val)) continue;
            int f = fascia(s_m, json_time_s(cJSON_GetObjectItem(r, "start")));
            versa(rq.ruolo, rq.voce, f, val);
        }
    }

    ricava();
    ESP_LOGI(TAG, "%s: casa %.2f kWh (rete +%.2f -%.2f, sole %.2f, batteria +%.2f -%.2f)",
             energy_periodo_nome(s_m.periodo), s_m.casa, s_m.rete_presa, s_m.rete_immessa,
             s_m.solare, s_m.batteria_scarica, s_m.batteria_carica);
}

// ------------------------------------------------------------------ richieste

static void avvisa(void) { if (s_cb) s_cb(); }

struct Lavoro { uint32_t gen; bool prefs; };

static void su_prefs(bool ok, cJSON *result, const char *error, void *ctx)
{
    Lavoro *l = (Lavoro *)ctx;
    bsp_display_lock(0);
    if (l->gen == s_gen) {
        if (ok && result) {
            leggi_prefs(result);
            s_m.errore.clear();
            s_vuole_prefs = false;
            s_vuole_meta = !s_rich.empty();
            s_vuole_dati = true;
        } else {
            s_m.errore = std::string("Preferenze Energia non lette: ") + (error ? error : "?");
            s_riprova_dopo = ora_s() + RIPROVA_S;
            ESP_LOGW(TAG, "%s", s_m.errore.c_str());
        }
    }
    s_in_volo = false;
    bsp_display_unlock();
    delete l;
    avvisa();
}

static void su_meta(bool ok, cJSON *result, const char *error, void *ctx)
{
    Lavoro *l = (Lavoro *)ctx;
    bsp_display_lock(0);
    if (l->gen == s_gen) {
        if (ok && result) leggi_meta(result);
        else ESP_LOGW(TAG, "unita' non lette (%s): i numeri restano come arrivano",
                      error ? error : "?");
        /* Anche se non arrivano si tira dritto: meglio numeri senza conversione
           che nessun numero. */
        s_vuole_meta = false;
    }
    s_in_volo = false;
    bsp_display_unlock();
    delete l;
    avvisa();
}

static void su_dati(bool ok, cJSON *result, const char *error, void *ctx)
{
    Lavoro *l = (Lavoro *)ctx;

    /* Come per i grafici: il lavoro pesante si fa col display libero. Qui pero'
       i totali si scrivono dentro il modello, quindi la lettura del JSON e la
       scrittura restano insieme sotto lock - e' una manciata di righe per
       statistica, non la sfilza di allocazioni della storia. */
    bsp_display_lock(0);
    if (l->gen == s_gen) {
        if (ok && result) {
            leggi_statistiche(result);
            s_m.errore.clear();
            s_vuole_dati = false;
        } else {
            s_m.errore = std::string("Dati Energia non arrivati: ") + (error ? error : "?");
            s_riprova_dopo = ora_s() + RIPROVA_S;
            ESP_LOGW(TAG, "%s", s_m.errore.c_str());
        }
    }
    s_in_volo = false;
    bsp_display_unlock();
    delete l;
    avvisa();
}

bool energy_model_prossima_richiesta(std::string &body)
{
    if (!s_serve || s_in_volo) return false;
    if (!ha_ws_connected()) return false;
    if (!orologio_ok()) return false;                  // senza ora non si sa nemmeno che giorno e'
    if (ora_s() < s_riprova_dopo) return false;

    if (s_vuole_prefs) {
        body = "\"type\":\"energy/get_prefs\"";
        Lavoro *l = new Lavoro{s_gen, true};
        s_in_volo = true;
        if (ha_ws_request(body.c_str(), su_prefs, l) < 0) { s_in_volo = false; delete l; return false; }
        return true;
    }

    if (s_vuole_meta) {
        std::string ids;
        std::vector<std::string> visti;
        for (const Richiesto &r : s_rich) {
            bool gia = false;
            for (const std::string &v : visti) if (v == r.id) { gia = true; break; }
            if (gia) continue;
            visti.push_back(r.id);
            ids += (ids.empty() ? "\"" : ",\"") + r.id + "\"";
        }
        body = "\"type\":\"recorder/get_statistics_metadata\",\"statistic_ids\":[" + ids + "]";
        Lavoro *l = new Lavoro{s_gen, false};
        s_in_volo = true;
        if (ha_ws_request(body.c_str(), su_meta, l) < 0) { s_in_volo = false; delete l; return false; }
        return true;
    }

    if (!s_vuole_dati && s_m.dati_pronti &&
        ora_s() - s_m.aggiornato_s < RINFRESCO_S) return false;
    if (s_rich.empty()) return false;                  // niente di configurato: niente da chiedere

    int npunti; int64_t passo;
    const char *per = finestra(s_m.periodo, &s_m.t0, &s_m.t1, &npunti, &passo);
    s_m.npunti = npunti;
    s_m.passo_s = passo;

    /* Gli id si chiedono una volta sola anche se compaiono in piu' ruoli. */
    std::string ids;
    std::vector<std::string> visti;
    for (const Richiesto &r : s_rich) {
        bool gia = false;
        for (const std::string &v : visti) if (v == r.id) { gia = true; break; }
        if (gia) continue;
        visti.push_back(r.id);
        ids += (ids.empty() ? "\"" : ",\"") + r.id + "\"";
    }

    body = "\"type\":\"recorder/statistics_during_period\""
           ",\"start_time\":\"" + iso_utc(s_m.t0) + "\""
           ",\"end_time\":\""   + iso_utc(s_m.t1) + "\""
           ",\"period\":\"" + per + "\""
           ",\"statistic_ids\":[" + ids + "]"
           ",\"types\":[\"change\"]";

    Lavoro *l = new Lavoro{s_gen, false};
    s_in_volo = true;
    if (ha_ws_request(body.c_str(), su_dati, l) < 0) { s_in_volo = false; delete l; return false; }
    ESP_LOGI(TAG, "%s: chiedo %u statistiche (%u byte)",
             energy_periodo_nome(s_m.periodo), (unsigned)visti.size(), (unsigned)body.size());
    return true;
}

// ------------------------------------------------------------------ fuori

const EnergyModel *energy_model_get(void) { return &s_m; }

void energy_model_set_periodo(EnergyPeriodo p)
{
    if (p == s_m.periodo) return;
    s_m.periodo = p;
    s_gen++;                       // le risposte in arrivo riguardano il periodo vecchio
    s_in_volo = false;
    s_vuole_dati = true;
    s_m.dati_pronti = false;
    s_riprova_dopo = 0;
    avvisa();
}

void energy_model_invalida(bool anche_prefs)
{
    s_gen++;
    s_in_volo = false;
    s_vuole_dati = true;
    s_m.dati_pronti = false;
    s_riprova_dopo = 0;
    if (anche_prefs) {
        s_vuole_prefs = true;
        s_m.prefs_lette = false;
        s_vuole_meta = false;
        s_mostra_esempio = true;
    }
}

static bool s_forzato = false;

void energy_model_forza(void)
{
    s_forzato = true;
    s_serve = true;
    energy_model_invalida(true);
}

void energy_model_serve(bool serve)
{
    if (s_forzato) return;                        // il comando della console ha la precedenza
    if (serve && !s_serve) s_riprova_dopo = 0;    // appena serve, non far aspettare
    s_serve = serve;
}

void energy_model_on_change(energy_model_cb_t cb) { s_cb = cb; }

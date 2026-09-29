#include "dash_override.h"
#include "lovelace_ui.h"
#include <string>
#include <vector>
#include <map>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "dashovr";
#define OVR_PATH "/spiffs/overrides.json"

#define CID_SECTIONS "@sections"      // contenitore delle sezioni
#define CID_CARDS    "cards"          // vista senza sezioni: un contenitore solo

static cJSON      *s_all = NULL;    // contenuto del file: {"v":1,"views":{...}}
static cJSON      *s_src = NULL;    // vista come l'ha mandata HA (copia)
static std::string s_key;           // "lovelace/0"

struct Item {
    std::string key;
    std::string home;               // contenitore di provenienza
    cJSON      *node;
};

// ------------------------------------------------------------------ utilita'

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

/* Testo che identifica la card: l'entita' se c'e', altrimenti il titolo. */
static std::string card_base(const cJSON *card)
{
    const char *s;
    if ((s = jstr(card, "entity")))  return s;
    const cJSON *e0 = cJSON_GetArrayItem(cJSON_GetObjectItem(card, "entities"), 0);
    if (e0) {
        if (cJSON_IsString(e0))          return e0->valuestring;
        if ((s = jstr(e0, "entity")))    return s;
    }
    if ((s = jstr(card, "heading")))     return s;
    if ((s = jstr(card, "title")))       return s;
    if ((s = jstr(card, "name")))        return s;
    return "";
}

static std::string section_base(const cJSON *sec)
{
    const char *s;
    if ((s = jstr(sec, "title"))) return s;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(sec, "cards")) {
        if ((s = jstr(c, "heading"))) return s;
        if ((s = jstr(c, "title")))   return s;
        break;
    }
    return "";
}

/* Chiave di una card o di una sezione: tipo + contenuto, con #n per i
   doppioni. Non dipende dalla sezione in cui si trova, cosi' spostandola la
   chiave non cambia e i ritocchi la seguono. Non e' un id vero (Lovelace non
   ne da' uno): se la card viene cambiata su HA la chiave cambia e la card
   torna al suo posto originale, visibile. */
static std::string item_key(const cJSON *item, bool section, std::map<std::string, int> &seen)
{
    const char *t = jstr(item, "type");
    std::string k = section ? "sec" : (t ? t : "card");
    std::string b = section ? section_base(item) : card_base(item);
    if (!b.empty()) k += "|" + b;
    int n = seen[k]++;
    if (n) k += "#" + std::to_string(n);
    return k;
}

static bool in_array(const cJSON *arr, const std::string &s)
{
    const cJSON *i;
    cJSON_ArrayForEach(i, arr)
        if (cJSON_IsString(i) && s == i->valuestring) return true;
    return false;
}

// ------------------------------------------------------------------ file

static void ensure_all(void)
{
    if (s_all) return;
    s_all = cJSON_CreateObject();
    cJSON_AddNumberToObject(s_all, "v", 1);
    cJSON_AddItemToObject(s_all, "views", cJSON_CreateObject());
}

void dash_override_init(void)
{
    FILE *f = fopen(OVR_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 64 * 1024) {
            char *buf = (char *)malloc(n + 1);
            if (buf) {
                size_t rd = fread(buf, 1, n, f);
                buf[rd] = 0;
                s_all = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }
    if (s_all && !cJSON_GetObjectItem(s_all, "views")) { cJSON_Delete(s_all); s_all = NULL; }
    ensure_all();
    ESP_LOGI(TAG, "ritocchi locali: %d viste",
             cJSON_GetArraySize(cJSON_GetObjectItem(s_all, "views")));
}

static bool save_file(void)
{
    char *txt = cJSON_PrintUnformatted(s_all);
    if (!txt) return false;
    FILE *f = fopen(OVR_PATH, "w");
    bool ok = false;
    if (f) {
        size_t len = strlen(txt);
        ok = fwrite(txt, 1, len, f) == len;
        fclose(f);
    } else {
        ESP_LOGE(TAG, "non riesco a scrivere %s", OVR_PATH);
    }
    cJSON_free(txt);
    return ok;
}

static cJSON *current_ov(void)
{
    ensure_all();
    if (s_key.empty()) return NULL;
    return cJSON_GetObjectItem(cJSON_GetObjectItem(s_all, "views"), s_key.c_str());
}

// ------------------------------------------------------------------ dimensioni

/* Scrive nella card la misura scelta dall'utente, nello stesso formato che usa
   Home Assistant: cosi' il pannello la disegna senza sapere da dove arriva. */
static void apply_size(cJSON *card, const cJSON *ov, const std::string &key)
{
    const cJSON *sizes = cJSON_GetObjectItem(ov, "size");
    const cJSON *s = sizes ? cJSON_GetObjectItem(sizes, key.c_str()) : NULL;
    if (!cJSON_IsObject(s)) return;
    const cJSON *c = cJSON_GetObjectItem(s, "columns");
    const cJSON *r = cJSON_GetObjectItem(s, "rows");

    cJSON *go = cJSON_GetObjectItem(card, "grid_options");
    if (!cJSON_IsObject(go)) {
        go = cJSON_CreateObject();
        cJSON_DeleteItemFromObject(card, "grid_options");
        cJSON_AddItemToObject(card, "grid_options", go);
    }
    if (cJSON_IsNumber(c)) {
        cJSON_DeleteItemFromObject(go, "columns");
        cJSON_AddNumberToObject(go, "columns", c->valueint);
    }
    if (cJSON_IsNumber(r)) {
        cJSON_DeleteItemFromObject(go, "rows");
        if (r->valueint > 0) cJSON_AddNumberToObject(go, "rows", r->valueint);
        else                 cJSON_AddStringToObject(go, "rows", "auto");
    }
    /* layout_options e' il formato vecchio su 4 colonne: se resta, vince lui. */
    cJSON_DeleteItemFromObject(card, "layout_options");
}

// ------------------------------------------------------------------ applica

/* Svuota un array di card mettendole nel sacchetto comune. */
static void collect(cJSON *parent, const char *field, const std::string &home,
                    bool sections, std::map<std::string, int> &seen,
                    std::vector<Item> &pool)
{
    cJSON *arr = cJSON_GetObjectItem(parent, field);
    if (!cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_DetachItemFromArray(arr, 0);
        pool.push_back({ item_key(it, sections, seen), home, it });
    }
}

/* Distribuisce le card fra i contenitori. In due giri, e l'ordine conta:
   prima ogni contenitore prende quelle che l'utente ha elencato — anche se
   arrivano da un'altra sezione — e solo dopo ognuno si riprende quelle che
   erano gia' sue. Facendolo in un giro solo, la sezione di partenza si
   riprendeva la card prima che quella di destinazione la reclamasse, e lo
   spostamento fra sezioni non si vedeva. */
static void distribute(const std::vector<std::string> &cids, const cJSON *ov,
                       std::vector<Item> &pool,
                       std::vector<std::vector<size_t>> &out)
{
    const cJSON *orders = cJSON_GetObjectItem(ov, "order");
    std::vector<bool> taken(pool.size(), false);
    out.assign(cids.size(), {});

    for (size_t c = 0; c < cids.size(); c++) {
        const cJSON *order = cJSON_GetObjectItem(orders, cids[c].c_str());
        const cJSON *k;
        cJSON_ArrayForEach(k, order) {
            if (!cJSON_IsString(k)) continue;
            for (size_t i = 0; i < pool.size(); i++)
                if (!taken[i] && pool[i].key == k->valuestring) { taken[i] = true; out[c].push_back(i); break; }
        }
    }
    for (size_t i = 0; i < pool.size(); i++) {
        if (taken[i]) continue;
        for (size_t c = 0; c < cids.size(); c++)
            if (pool[i].home == cids[c]) { taken[i] = true; out[c].push_back(i); break; }
    }
}

/* Attacca al contenitore le card che gli sono toccate, saltando le nascoste e
   scrivendo le misure scelte. Restituisce quelle rimaste. */
static std::vector<Item> attach(cJSON *parent, const char *field, const cJSON *ov,
                                std::vector<Item> &pool, const std::vector<size_t> &idx,
                                std::vector<bool> &placed, bool do_hide)
{
    std::vector<Item> out;
    cJSON *arr = cJSON_GetObjectItem(parent, field);
    if (!cJSON_IsArray(arr)) return out;
    const cJSON *hidden = cJSON_GetObjectItem(ov, "hidden");

    for (size_t i : idx) {
        Item &it = pool[i];
        placed[i] = true;
        if (do_hide && hidden && in_array(hidden, it.key)) { cJSON_Delete(it.node); it.node = NULL; continue; }
        apply_size(it.node, ov, it.key);
        cJSON_AddItemToArray(arr, it.node);
        out.push_back(it);
    }
    return out;
}

static void apply_view(cJSON *view, const cJSON *ov, bool do_hide)
{
    std::map<std::string, int> seen;
    const cJSON *extras = cJSON_GetObjectItem(ov, "extra");

    if (cJSON_GetObjectItem(view, "sections")) {
        std::vector<Item> secs, cards;
        collect(view, "sections", CID_SECTIONS, true, seen, secs);
        for (Item &s : secs) collect(s.node, "cards", s.key, false, seen, cards);
        const cJSON *ex;
        cJSON_ArrayForEach(ex, extras) {
            if (!ex->string) continue;
            const cJSON *card;
            cJSON_ArrayForEach(card, ex) {
                cJSON *cp = cJSON_Duplicate(card, 1);
                if (!cp) continue;
                cJSON_AddBoolToObject(cp, "_local", true);
                cards.push_back({ item_key(cp, false, seen), ex->string, cp });
            }
        }

        // sezioni nell'ordine scelto
        std::vector<std::string> one = { CID_SECTIONS };
        std::vector<std::vector<size_t>> got;
        distribute(one, ov, secs, got);
        std::vector<bool> placed_s(secs.size(), false);
        std::vector<Item> kept = attach(view, "sections", ov, secs, got[0], placed_s, do_hide);

        // card fra le sezioni rimaste
        std::vector<std::string> cids;
        for (Item &s : kept) cids.push_back(s.key);
        std::vector<std::vector<size_t>> per_sec;
        distribute(cids, ov, cards, per_sec);
        std::vector<bool> placed_c(cards.size(), false);
        for (size_t c = 0; c < kept.size(); c++)
            attach(kept[c].node, "cards", ov, cards, per_sec[c], placed_c, do_hide);

        // cio' che non ha trovato posto (sezione nascosta o sparita)
        for (size_t i = 0; i < cards.size(); i++) if (!placed_c[i]) cJSON_Delete(cards[i].node);
        for (size_t i = 0; i < secs.size();  i++) if (!placed_s[i]) cJSON_Delete(secs[i].node);
    } else {
        std::vector<Item> cards;
        collect(view, "cards", CID_CARDS, false, seen, cards);
        const cJSON *ex = cJSON_GetObjectItem(extras, CID_CARDS);
        const cJSON *card;
        cJSON_ArrayForEach(card, ex) {
            cJSON *cp = cJSON_Duplicate(card, 1);
            if (!cp) continue;
            cJSON_AddBoolToObject(cp, "_local", true);
            cards.push_back({ item_key(cp, false, seen), CID_CARDS, cp });
        }
        std::vector<std::string> one = { CID_CARDS };
        std::vector<std::vector<size_t>> got;
        distribute(one, ov, cards, got);
        std::vector<bool> placed(cards.size(), false);
        attach(view, "cards", ov, cards, got[0], placed, do_hide);
        for (size_t i = 0; i < cards.size(); i++) if (!placed[i]) cJSON_Delete(cards[i].node);
    }
}

cJSON *dash_override_rebuild(void)
{
    if (!s_src) return NULL;
    cJSON *v = cJSON_Duplicate(s_src, 1);
    if (!v) return NULL;
    apply_view(v, current_ov(), true);
    cJSON *wrap = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, v);
    cJSON_AddItemToObject(wrap, "views", arr);
    return wrap;
}

cJSON *dash_override_prepare(const cJSON *config, int view, const char *dash)
{
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(config, "views"), view);
    if (!v) return NULL;
    if (s_src) cJSON_Delete(s_src);
    s_src = cJSON_Duplicate(v, 1);
    s_key = std::string(dash ? dash : "?") + "/" + std::to_string(view);
    return dash_override_rebuild();
}

bool dash_override_has_source(void) { return s_src != NULL; }

// ------------------------------------------------------------------ editor

/* Etichetta mostrata nell'editor: il nome dell'entita' se lo conosciamo gia'
   (stesso testo della dashboard), altrimenti quello che c'e' nel JSON.
   Va chiamata col lock del display preso: legge gli stati di lovelace_ui. */
static std::string item_label(const cJSON *item, bool section)
{
    if (section) {
        std::string b = section_base(item);
        return b.empty() ? "Sezione" : b;
    }
    const char *s;
    if ((s = jstr(item, "name")))    return s;
    if ((s = jstr(item, "heading"))) return s;
    if ((s = jstr(item, "title")))   return s;
    std::string b = card_base(item);
    if (!b.empty() && strchr(b.c_str(), '.')) {
        char nm[64], val[64];
        if (ll_entity_text(b.c_str(), nm, sizeof(nm), val, sizeof(val)) && nm[0]) return nm;
    }
    return b;
}

/* L'entita' di una card, ma solo quando e' una sola e non ci sono dubbi su
   quale sia: un "entity" da solo, oppure un elenco "entities" con dentro una
   voce sola. La usa il pulsante dell'editor che porta un valore nello
   slideshow: su una card con sei righe non si capirebbe quale delle sei sta
   aggiungendo, e meglio nessun pulsante che un pulsante che indovina. */
static std::string card_single_entity(const cJSON *card)
{
    const cJSON *ents = cJSON_GetObjectItem(card, "entities");
    const char *s = jstr(card, "entity");
    std::string id;
    if (s && !ents) {
        id = s;
    } else if (cJSON_IsArray(ents) && cJSON_GetArraySize(ents) == 1) {
        const cJSON *e0 = cJSON_GetArrayItem(ents, 0);
        if (cJSON_IsString(e0))       id = e0->valuestring;
        else if ((s = jstr(e0, "entity"))) id = s;
    }
    /* Un'entita' vera ha il dominio davanti: "sensor.soggiorno_temperatura".
       Senza il punto e' un titolo, e nello slideshow non ci va. */
    return id.find('.') != std::string::npos ? id : std::string();
}

static void add_items(cJSON *containers, const char *title, const std::string &cid,
                      const std::vector<Item> &items, const cJSON *hidden, bool sections)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "id", cid.c_str());
    cJSON_AddStringToObject(c, "title", title);
    cJSON *arr = cJSON_AddArrayToObject(c, "items");
    for (const Item &it : items) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "k", it.key.c_str());
        const char *t = jstr(it.node, "type");
        cJSON_AddStringToObject(o, "type", sections ? "sezione" : (t ? t : "card"));
        cJSON_AddStringToObject(o, "label", item_label(it.node, sections).c_str());
        cJSON_AddBoolToObject(o, "hidden", hidden && in_array(hidden, it.key));
        cJSON_AddBoolToObject(o, "local", cJSON_IsTrue(cJSON_GetObjectItem(it.node, "_local")));
        if (!sections) {
            cJSON_AddNumberToObject(o, "columns", ll_card_columns(it.node));
            cJSON_AddNumberToObject(o, "rows", ll_card_rows(it.node));
            std::string ent = card_single_entity(it.node);
            if (!ent.empty()) cJSON_AddStringToObject(o, "entity", ent.c_str());
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToArray(containers, c);
}

cJSON *dash_override_layout(void)
{
    if (!s_src) return NULL;
    cJSON *v = cJSON_Duplicate(s_src, 1);
    if (!v) return NULL;
    const cJSON *ov = current_ov();
    const cJSON *hidden = cJSON_GetObjectItem(ov, "hidden");

    /* Applico ordine, spostamenti e dimensioni ma non il nascondere: l'editor
       deve poter rimettere in mostra cio' che e' nascosto. */
    apply_view(v, ov, false);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "view_key", s_key.c_str());
    const char *vt = jstr(s_src, "title");
    cJSON_AddStringToObject(root, "view_title", vt ? vt : "");

    int row_h = 56, gap = 8, sw = 1024, sh = 600;
    ll_grid_metrics(&row_h, &gap, &sw, &sh);
    cJSON_AddNumberToObject(root, "row_h", row_h);
    cJSON_AddNumberToObject(root, "gap", gap);
    cJSON_AddNumberToObject(root, "screen_w", sw);
    cJSON_AddNumberToObject(root, "screen_h", sh);

    cJSON *containers = cJSON_AddArrayToObject(root, "containers");
    std::map<std::string, int> seen;

    if (cJSON_GetObjectItem(v, "sections")) {
        cJSON_AddStringToObject(root, "layout", "sections");
        std::vector<Item> secs;
        cJSON *arr = cJSON_GetObjectItem(v, "sections");
        cJSON *sec;
        cJSON_ArrayForEach(sec, arr) secs.push_back({ item_key(sec, true, seen), CID_SECTIONS, sec });
        add_items(containers, "Sezioni", CID_SECTIONS, secs, hidden, true);
        for (Item &s : secs) {
            std::vector<Item> cards;
            cJSON *card;
            cJSON_ArrayForEach(card, cJSON_GetObjectItem(s.node, "cards"))
                cards.push_back({ item_key(card, false, seen), s.key, card });
            add_items(containers, item_label(s.node, true).c_str(), s.key, cards, hidden, false);
        }
    } else {
        cJSON_AddStringToObject(root, "layout", "masonry");
        std::vector<Item> cards;
        cJSON *card;
        cJSON_ArrayForEach(card, cJSON_GetObjectItem(v, "cards"))
            cards.push_back({ item_key(card, false, seen), CID_CARDS, card });
        add_items(containers, "Dashboard", CID_CARDS, cards, hidden, false);
    }
    cJSON_Delete(v);
    return root;
}

cJSON *dash_override_current(void)
{
    const cJSON *ov = current_ov();
    return ov ? cJSON_Duplicate(ov, 1) : cJSON_CreateObject();
}

bool dash_override_save(const char *json)
{
    cJSON *ov = cJSON_Parse(json);
    if (!cJSON_IsObject(ov)) { cJSON_Delete(ov); return false; }
    if (s_key.empty()) { cJSON_Delete(ov); return false; }
    ensure_all();
    cJSON *views = cJSON_GetObjectItem(s_all, "views");
    cJSON_DeleteItemFromObject(views, s_key.c_str());
    cJSON_AddItemToObject(views, s_key.c_str(), ov);
    bool ok = save_file();
    ESP_LOGI(TAG, "ritocchi salvati per %s (%s)", s_key.c_str(), ok ? "ok" : "errore scrittura");
    return ok;
}

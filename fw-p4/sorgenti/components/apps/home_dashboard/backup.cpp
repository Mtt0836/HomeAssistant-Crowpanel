#include "backup.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include "mbedtls/base64.h"

static const char *TAG = "backup";

#define NS_BACKUP   "backup"
#define K_CHIAVE    "chiave"
#define K_PRESA     "presa"
#define CHIAVE_LEN  32          // 256 bit
#define NONCE_LEN   12          // quello che vuole AES-GCM
#define TAG_LEN     16

/* I file che stanno fuori dalla NVS. La configurazione dello slideshow puo'
   stare su SD o in SPIFFS a seconda di come e' messo il pannello: si provano
   tutti e due e si salva quello che c'e'. */
static const char *FILE_CFG[] = {
    "/spiffs/standby.json",
    "/spiffs/overrides.json",
    "/sdcard/standby.json",
};

/* Cose che in chiaro non escono.

   Non e' prudenza generica: e' che ognuna di queste, letta da un estraneo, gli
   consegna qualcosa. Il permesso apre Home Assistant, la chiave privata
   permette di spacciarsi per il pannello, le impronte si possono provare a
   forza bruta con calma su un computer potente. */
bool backup_e_segreto(const char *ns, const char *chiave)
{
    if (!strcmp(ns, "webcert")) return true;             // certificato e chiave privata
    if (!strcmp(ns, "webauth")) return true;             // impronte delle password
    if (!strcmp(ns, "pinlock")) return true;             // impronta del PIN
    if (!strcmp(ns, "hapanel") && !strcmp(chiave, "ha_refresh")) return true;
    return false;
}

/* Lo spazio del salvataggio non entra mai in un salvataggio: dentro c'e' la
   chiave che lo apre. */
static bool e_nostro(const char *ns) { return strcmp(ns, NS_BACKUP) == 0; }

// ------------------------------------------------------------------ base64

static std::string b64(const unsigned char *d, size_t n)
{
    size_t serve = 0;
    mbedtls_base64_encode(NULL, 0, &serve, d, n);
    std::string out(serve ? serve - 1 : 0, '\0');
    size_t fatti = 0;
    if (serve && mbedtls_base64_encode((unsigned char *)&out[0], serve, &fatti, d, n) != 0)
        return std::string();
    out.resize(fatti);
    return out;
}

static bool de_b64(const char *s, std::vector<unsigned char> &out)
{
    if (!s) return false;
    size_t n = strlen(s), serve = 0;
    mbedtls_base64_decode(NULL, 0, &serve, (const unsigned char *)s, n);
    out.resize(serve);
    size_t fatti = 0;
    if (serve && mbedtls_base64_decode(out.data(), serve, &fatti, (const unsigned char *)s, n) != 0)
        return false;
    out.resize(fatti);
    return true;
}

// ------------------------------------------------------------------ chiave

/* L'alfabeto di Crockford: niente I, L, O, U. Le prime tre si confondono con
   1 e 0 quando si ricopia a mano una chiave da un foglio, la U sparisce per
   non comporre parole sgradevoli per caso. */
static const char ALFA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static void in_base32(const unsigned char *d, size_t n, char *out, size_t out_sz)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bit = 0, gruppo = 0;
    for (size_t i = 0; i < n && o + 2 < out_sz; i++) {
        acc = (acc << 8) | d[i];
        bit += 8;
        while (bit >= 5 && o + 2 < out_sz) {
            bit -= 5;
            out[o++] = ALFA[(acc >> bit) & 0x1f];
            if (++gruppo == 4 && o + 2 < out_sz) { out[o++] = '-'; gruppo = 0; }
        }
    }
    /* I bit che avanzano vanno scritti lo stesso.

       256 bit non si dividono per 5: dopo l'ultimo carattere pieno ne resta
       uno solo, e senza questa riga finiva nel nulla. La chiave usciva di 51
       caratteri invece di 52, cioe' di 31 byte invece di 32 - e chi provava a
       ricopiarla per rimettere un salvataggio si sentiva rispondere "chiave
       non valida" senza capire perche'. Dentro il pannello non si notava,
       perche' li' la chiave si legge dalla memoria e non dal testo. */
    if (bit > 0 && o + 2 < out_sz)
        out[o++] = ALFA[(acc << (5 - bit)) & 0x1f];
    if (o && out[o - 1] == '-') o--;
    out[o] = 0;
}

static bool da_base32(const char *s, unsigned char *out, size_t out_sz)
{
    uint32_t acc = 0;
    int bit = 0;
    size_t o = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        /* I caratteri che l'alfabeto evita si accettano lo stesso in lettura:
           chi ricopia a mano scrive O per 0 e I per 1 senza pensarci. */
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (c == 'U') c = 'V';
        const char *p = strchr(ALFA, c);
        if (!p) return false;
        acc = (acc << 5) | (uint32_t)(p - ALFA);
        bit += 5;
        if (bit >= 8) {
            bit -= 8;
            if (o < out_sz) out[o++] = (unsigned char)((acc >> bit) & 0xff);
        }
    }
    return o == out_sz;
}

static bool chiave_bytes(unsigned char out[CHIAVE_LEN])
{
    nvs_handle_t h;
    size_t n = CHIAVE_LEN;
    if (nvs_open(NS_BACKUP, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_blob(h, K_CHIAVE, out, &n);
        nvs_close(h);
        if (e == ESP_OK && n == CHIAVE_LEN) return true;
    }
    /* Non c'e' ancora: si crea adesso e non cambiera' piu'. Se cambiasse, i
       salvataggi fatti prima diventerebbero illeggibili. */
    esp_fill_random(out, CHIAVE_LEN);
    if (nvs_open(NS_BACKUP, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, K_CHIAVE, out, CHIAVE_LEN) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "chiave di recupero creata");
    return ok;
}

bool backup_chiave_testo(char *out, size_t out_sz)
{
    unsigned char k[CHIAVE_LEN];
    if (!chiave_bytes(k)) return false;
    in_base32(k, CHIAVE_LEN, out, out_sz);
    memset(k, 0, sizeof(k));
    return true;
}

bool backup_chiave_gia_presa(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS_BACKUP, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, K_PRESA, &v);
    nvs_close(h);
    return v != 0;
}

void backup_chiave_segna_presa(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_BACKUP, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, K_PRESA, 1);
    nvs_commit(h);
    nvs_close(h);
}

// ------------------------------------------------------------------ raccolta

static cJSON *leggi_nvs(bool con_segreti, cJSON *omessi)
{
    cJSON *spazi = cJSON_CreateObject();
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    while (e == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (e_nostro(info.namespace_name)) { e = nvs_entry_next(&it); continue; }
        if (!con_segreti && backup_e_segreto(info.namespace_name, info.key)) {
            char nome[64];
            snprintf(nome, sizeof(nome), "%s/%s", info.namespace_name, info.key);
            cJSON_AddItemToArray(omessi, cJSON_CreateString(nome));
            e = nvs_entry_next(&it);
            continue;
        }

        cJSON *sp = cJSON_GetObjectItem(spazi, info.namespace_name);
        if (!sp) { sp = cJSON_CreateObject(); cJSON_AddItemToObject(spazi, info.namespace_name, sp); }

        nvs_handle_t h;
        if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
            cJSON *v = cJSON_CreateObject();
            bool preso = true;
            switch (info.type) {
                case NVS_TYPE_U8:  { uint8_t  x = 0; nvs_get_u8(h, info.key, &x);  cJSON_AddStringToObject(v, "t", "u8");  cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_I8:  { int8_t   x = 0; nvs_get_i8(h, info.key, &x);  cJSON_AddStringToObject(v, "t", "i8");  cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_U16: { uint16_t x = 0; nvs_get_u16(h, info.key, &x); cJSON_AddStringToObject(v, "t", "u16"); cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_I16: { int16_t  x = 0; nvs_get_i16(h, info.key, &x); cJSON_AddStringToObject(v, "t", "i16"); cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_U32: { uint32_t x = 0; nvs_get_u32(h, info.key, &x); cJSON_AddStringToObject(v, "t", "u32"); cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_I32: { int32_t  x = 0; nvs_get_i32(h, info.key, &x); cJSON_AddStringToObject(v, "t", "i32"); cJSON_AddNumberToObject(v, "v", x); break; }
                case NVS_TYPE_U64: { uint64_t x = 0; nvs_get_u64(h, info.key, &x); cJSON_AddStringToObject(v, "t", "u64"); cJSON_AddNumberToObject(v, "v", (double)x); break; }
                case NVS_TYPE_I64: { int64_t  x = 0; nvs_get_i64(h, info.key, &x); cJSON_AddStringToObject(v, "t", "i64"); cJSON_AddNumberToObject(v, "v", (double)x); break; }
                case NVS_TYPE_STR: {
                    size_t n = 0;
                    if (nvs_get_str(h, info.key, NULL, &n) == ESP_OK && n) {
                        std::string s(n, '\0');
                        if (nvs_get_str(h, info.key, &s[0], &n) == ESP_OK) {
                            s.resize(n ? n - 1 : 0);
                            cJSON_AddStringToObject(v, "t", "str");
                            cJSON_AddStringToObject(v, "v", s.c_str());
                        } else preso = false;
                    } else preso = false;
                    break;
                }
                case NVS_TYPE_BLOB: {
                    size_t n = 0;
                    if (nvs_get_blob(h, info.key, NULL, &n) == ESP_OK && n) {
                        std::vector<unsigned char> b(n);
                        if (nvs_get_blob(h, info.key, b.data(), &n) == ESP_OK) {
                            cJSON_AddStringToObject(v, "t", "blob");
                            cJSON_AddStringToObject(v, "v", b64(b.data(), n).c_str());
                        } else preso = false;
                    } else preso = false;
                    break;
                }
                default: preso = false; break;
            }
            nvs_close(h);
            if (preso) cJSON_AddItemToObject(sp, info.key, v);
            else       cJSON_Delete(v);
        }
        e = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    return spazi;
}

static cJSON *leggi_file(void)
{
    cJSON *o = cJSON_CreateObject();
    for (const char *p : FILE_CFG) {
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 128 * 1024) {
            std::string s(n, '\0');
            if (fread(&s[0], 1, n, f) == (size_t)n) cJSON_AddStringToObject(o, p, s.c_str());
        }
        fclose(f);
    }
    return o;
}

// ------------------------------------------------------------------ cifratura

static bool cifra(const std::string &chiaro, const unsigned char k[CHIAVE_LEN],
                  std::string &nonce_b64, std::string &dati_b64)
{
    if (psa_crypto_init() != PSA_SUCCESS) return false;

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&a, k, CHIAVE_LEN, &kid) != PSA_SUCCESS) return false;

    unsigned char nonce[NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));

    std::vector<unsigned char> out(chiaro.size() + TAG_LEN + 16);
    size_t n = 0;
    psa_status_t st = psa_aead_encrypt(kid, PSA_ALG_GCM, nonce, sizeof(nonce), NULL, 0,
                                       (const unsigned char *)chiaro.data(), chiaro.size(),
                                       out.data(), out.size(), &n);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS) { ESP_LOGE(TAG, "cifratura fallita: %d", (int)st); return false; }

    nonce_b64 = b64(nonce, sizeof(nonce));
    dati_b64  = b64(out.data(), n);
    return true;
}

static bool decifra(const std::vector<unsigned char> &nonce,
                    const std::vector<unsigned char> &dati,
                    const unsigned char k[CHIAVE_LEN], std::string &chiaro)
{
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    if (nonce.size() != NONCE_LEN || dati.size() <= TAG_LEN) return false;

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&a, k, CHIAVE_LEN, &kid) != PSA_SUCCESS) return false;

    std::vector<unsigned char> out(dati.size());
    size_t n = 0;
    psa_status_t st = psa_aead_decrypt(kid, PSA_ALG_GCM, nonce.data(), nonce.size(), NULL, 0,
                                       dati.data(), dati.size(), out.data(), out.size(), &n);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS) return false;      // chiave sbagliata o file manomesso
    chiaro.assign((const char *)out.data(), n);
    return true;
}

// ------------------------------------------------------------------ esporta

char *backup_esporta(bool cifrato)
{
    cJSON *omessi = cJSON_CreateArray();
    cJSON *dentro = cJSON_CreateObject();
    cJSON_AddItemToObject(dentro, "nvs", leggi_nvs(cifrato, omessi));
    cJSON_AddItemToObject(dentro, "file", leggi_file());

    char *corpo = cJSON_PrintUnformatted(dentro);
    cJSON_Delete(dentro);
    if (!corpo) { cJSON_Delete(omessi); return NULL; }

    cJSON *fuori = cJSON_CreateObject();
    cJSON_AddStringToObject(fuori, "crowpanel", "configurazione");
    cJSON_AddNumberToObject(fuori, "versione", 1);
    cJSON_AddBoolToObject(fuori, "cifrato", cifrato);

    time_t tt = time(NULL);
    struct tm tm;
    gmtime_r(&tt, &tm);
    char q[32];
    strftime(q, sizeof(q), "%Y-%m-%dT%H:%M:%SZ", &tm);
    cJSON_AddStringToObject(fuori, "creato", q);

    bool ok = true;
    if (cifrato) {
        unsigned char k[CHIAVE_LEN];
        std::string nb, db;
        ok = chiave_bytes(k) && cifra(std::string(corpo), k, nb, db);
        memset(k, 0, sizeof(k));
        if (ok) {
            cJSON_AddStringToObject(fuori, "nonce", nb.c_str());
            cJSON_AddStringToObject(fuori, "dati", db.c_str());
            cJSON_AddStringToObject(fuori, "come",
                "AES-256-GCM. Serve la chiave di recupero del pannello.");
        }
        cJSON_Delete(omessi);
    } else {
        cJSON *d = cJSON_Parse(corpo);
        if (d) cJSON_AddItemToObject(fuori, "dati", d); else ok = false;
        cJSON_AddItemToObject(fuori, "omessi", omessi);
        cJSON_AddStringToObject(fuori, "attenzione",
            "Salvataggio in chiaro: le voci elencate in \"omessi\" NON ci sono. "
            "Per un ripristino completo serve il salvataggio cifrato.");
    }
    free(corpo);

    char *out = ok ? cJSON_PrintUnformatted(fuori) : NULL;
    cJSON_Delete(fuori);
    return out;
}

// ------------------------------------------------------------------ importa

static void scrivi_nvs(const cJSON *spazi)
{
    const cJSON *sp;
    cJSON_ArrayForEach(sp, spazi) {
        if (!sp->string || e_nostro(sp->string)) continue;
        nvs_handle_t h;
        if (nvs_open(sp->string, NVS_READWRITE, &h) != ESP_OK) continue;
        const cJSON *v;
        cJSON_ArrayForEach(v, sp) {
            const cJSON *t = cJSON_GetObjectItem(v, "t");
            const cJSON *x = cJSON_GetObjectItem(v, "v");
            if (!cJSON_IsString(t) || !x || !v->string) continue;
            const char *ty = t->valuestring;
            if      (!strcmp(ty, "u8")  && cJSON_IsNumber(x)) nvs_set_u8 (h, v->string, (uint8_t)x->valuedouble);
            else if (!strcmp(ty, "i8")  && cJSON_IsNumber(x)) nvs_set_i8 (h, v->string, (int8_t)x->valuedouble);
            else if (!strcmp(ty, "u16") && cJSON_IsNumber(x)) nvs_set_u16(h, v->string, (uint16_t)x->valuedouble);
            else if (!strcmp(ty, "i16") && cJSON_IsNumber(x)) nvs_set_i16(h, v->string, (int16_t)x->valuedouble);
            else if (!strcmp(ty, "u32") && cJSON_IsNumber(x)) nvs_set_u32(h, v->string, (uint32_t)x->valuedouble);
            else if (!strcmp(ty, "i32") && cJSON_IsNumber(x)) nvs_set_i32(h, v->string, (int32_t)x->valuedouble);
            else if (!strcmp(ty, "u64") && cJSON_IsNumber(x)) nvs_set_u64(h, v->string, (uint64_t)x->valuedouble);
            else if (!strcmp(ty, "i64") && cJSON_IsNumber(x)) nvs_set_i64(h, v->string, (int64_t)x->valuedouble);
            else if (!strcmp(ty, "str") && cJSON_IsString(x)) nvs_set_str(h, v->string, x->valuestring);
            else if (!strcmp(ty, "blob") && cJSON_IsString(x)) {
                std::vector<unsigned char> b;
                if (de_b64(x->valuestring, b) && !b.empty())
                    nvs_set_blob(h, v->string, b.data(), b.size());
            }
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

static void scrivi_file(const cJSON *file)
{
    const cJSON *f;
    cJSON_ArrayForEach(f, file) {
        if (!f->string || !cJSON_IsString(f)) continue;
        /* Solo i percorsi che conosciamo: un salvataggio non deve poter
           scrivere dove gli pare nel pannello. */
        bool nostro = false;
        for (const char *p : FILE_CFG) if (!strcmp(p, f->string)) { nostro = true; break; }
        if (!nostro) { ESP_LOGW(TAG, "percorso non previsto, ignorato: %s", f->string); continue; }
        FILE *w = fopen(f->string, "wb");
        if (!w) continue;
        fwrite(f->valuestring, 1, strlen(f->valuestring), w);
        fclose(w);
    }
}

/* Quante voci ci sono, in tutto, dentro un albero "nvs". */
static int conta_voci(const cJSON *nvs)
{
    int n = 0;
    const cJSON *sp;
    cJSON_ArrayForEach(sp, nvs) n += cJSON_GetArraySize(sp);
    return n;
}

bool backup_autoprova(char *out, size_t out_sz)
{
    /* Nessuna scrittura: si esporta, si riapre e si confronta. Se qualcosa
       nella catena fosse rotto, un salvataggio "riuscito" si scoprirebbe
       illeggibile il giorno del ripristino, cioe' il giorno peggiore. */
    char *cif = backup_esporta(true);
    char *chi = backup_esporta(false);
    if (!cif || !chi) { free(cif); free(chi); snprintf(out, out_sz, "esportazione fallita"); return false; }

    size_t n_cif = strlen(cif), n_chi = strlen(chi);
    bool ok = true;
    int voci = 0, omessi = 0;
    const char *guaio = "";

    cJSON *f = cJSON_Parse(cif);
    if (!f) { ok = false; guaio = "il cifrato non e' JSON valido"; }

    std::vector<unsigned char> nonce, dati;
    unsigned char k[CHIAVE_LEN];
    std::string chiaro;
    if (ok) {
        const cJSON *n = cJSON_GetObjectItem(f, "nonce");
        const cJSON *d = cJSON_GetObjectItem(f, "dati");
        ok = cJSON_IsString(n) && cJSON_IsString(d) &&
             de_b64(n->valuestring, nonce) && de_b64(d->valuestring, dati) && chiave_bytes(k);
        if (!ok) guaio = "il cifrato non ha nonce o dati";
    }
    if (ok) {
        ok = decifra(nonce, dati, k, chiaro);
        if (!ok) guaio = "non si riapre con la chiave del pannello";
    }
    if (ok) {
        cJSON *d = cJSON_Parse(chiaro.c_str());
        if (!d) { ok = false; guaio = "dentro il cifrato non c'e' JSON valido"; }
        else {
            voci = conta_voci(cJSON_GetObjectItem(d, "nvs"));
            if (voci == 0) { ok = false; guaio = "il salvataggio e' vuoto"; }
            cJSON_Delete(d);
        }
    }

    /* Una chiave sbagliata deve essere rifiutata: e' il senso della
       cifratura. Se passasse, il file sarebbe aperto da chiunque. */
    if (ok) {
        unsigned char storta[CHIAVE_LEN];
        memcpy(storta, k, CHIAVE_LEN);
        storta[0] ^= 0xff;
        std::string niente;
        if (decifra(nonce, dati, storta, niente)) { ok = false; guaio = "una chiave sbagliata viene accettata!"; }
        memset(storta, 0, sizeof(storta));
    }
    memset(k, 0, sizeof(k));
    if (f) cJSON_Delete(f);

    /* La chiave scritta e riletta deve tornare la stessa.

       Questa prova mancava, e mancava proprio dove faceva male: la chiave
       dentro il pannello si legge dalla memoria, quindi tutto funzionava
       finche' nessuno la ricopiava. Chi l'avesse presa dal foglio per
       rimettere un salvataggio si sarebbe sentito dire "chiave non valida"
       nel momento peggiore, e non avrebbe avuto modo di capire perche'. */
    if (ok) {
        unsigned char vera[CHIAVE_LEN], tornata[CHIAVE_LEN];
        char testo[BACKUP_CHIAVE_MAX];
        if (!chiave_bytes(vera)) { ok = false; guaio = "chiave non leggibile"; }
        else {
            in_base32(vera, CHIAVE_LEN, testo, sizeof(testo));
            if (!da_base32(testo, tornata, CHIAVE_LEN))
                { ok = false; guaio = "la chiave scritta non si rilegge"; }
            else if (memcmp(vera, tornata, CHIAVE_LEN) != 0)
                { ok = false; guaio = "la chiave riletta non corrisponde"; }
        }
        memset(vera, 0, sizeof(vera));
        memset(tornata, 0, sizeof(tornata));
        memset(testo, 0, sizeof(testo));
    }

    /* E nel file in chiaro i segreti non ci devono essere.

       Il controllo guarda dentro i dati, non cerca parole nel file: i nomi
       delle voci escluse compaiono apposta nell'elenco "omessi", ed e' la
       prova che sono state tolte, non che sono rimaste. Un primo tentativo
       cercava la parola e basta, e gridava al guasto proprio leggendo quella
       riscontro. */
    cJSON *fc = cJSON_Parse(chi);
    if (ok && fc) {
        omessi = cJSON_GetArraySize(cJSON_GetObjectItem(fc, "omessi"));
        const cJSON *nvs = cJSON_GetObjectItem(cJSON_GetObjectItem(fc, "dati"), "nvs");
        const cJSON *sp;
        cJSON_ArrayForEach(sp, nvs) {
            if (!sp->string) continue;
            const cJSON *v;
            cJSON_ArrayForEach(v, sp) {
                if (v->string && backup_e_segreto(sp->string, v->string)) {
                    ok = false;
                    guaio = "il file in chiaro contiene dei segreti!";
                }
            }
        }
        if (ok && omessi == 0) { ok = false; guaio = "in chiaro non e' stato omesso niente"; }
    }
    if (fc) cJSON_Delete(fc);

    snprintf(out, out_sz,
             "%s  cifrato=%u byte  chiaro=%u byte  voci=%d  omessi_in_chiaro=%d%s%s",
             ok ? "OK" : "GUASTO", (unsigned)n_cif, (unsigned)n_chi, voci, omessi,
             ok ? "" : " - ", guaio);
    free(cif);
    free(chi);
    return ok;
}

bool backup_importa(const char *json, const char *chiave, char *err, size_t err_sz)
{
    auto fallisci = [&](const char *m) { snprintf(err, err_sz, "%s", m); return false; };

    cJSON *fuori = cJSON_Parse(json);
    if (!fuori) return fallisci("Il file non e' leggibile.");

    const cJSON *marchio = cJSON_GetObjectItem(fuori, "crowpanel");
    if (!cJSON_IsString(marchio) || strcmp(marchio->valuestring, "configurazione")) {
        cJSON_Delete(fuori);
        return fallisci("Questo non e' un salvataggio del pannello.");
    }

    bool cifrato = cJSON_IsTrue(cJSON_GetObjectItem(fuori, "cifrato"));
    cJSON *dentro = NULL;

    if (cifrato) {
        std::vector<unsigned char> nonce, dati;
        const cJSON *n = cJSON_GetObjectItem(fuori, "nonce");
        const cJSON *d = cJSON_GetObjectItem(fuori, "dati");
        if (!cJSON_IsString(n) || !cJSON_IsString(d) ||
            !de_b64(n->valuestring, nonce) || !de_b64(d->valuestring, dati)) {
            cJSON_Delete(fuori);
            return fallisci("Il salvataggio cifrato e' incompleto.");
        }
        unsigned char k[CHIAVE_LEN];
        bool hok;
        if (chiave && *chiave) hok = da_base32(chiave, k, CHIAVE_LEN);
        else                   hok = chiave_bytes(k);
        if (!hok) {
            cJSON_Delete(fuori);
            return fallisci("Chiave di recupero non valida.");
        }
        std::string chiaro;
        bool ok = decifra(nonce, dati, k, chiaro);
        memset(k, 0, sizeof(k));
        if (!ok) {
            cJSON_Delete(fuori);
            /* GCM non distingue fra chiave sbagliata e file ritoccato, e va
               bene cosi': in entrambi i casi non si deve scrivere niente. */
            return fallisci("Chiave sbagliata, o il file e' stato alterato.");
        }
        dentro = cJSON_Parse(chiaro.c_str());
        if (!dentro) { cJSON_Delete(fuori); return fallisci("Contenuto illeggibile."); }
    } else {
        const cJSON *d = cJSON_GetObjectItem(fuori, "dati");
        if (!cJSON_IsObject(d)) { cJSON_Delete(fuori); return fallisci("Salvataggio senza dati."); }
        dentro = cJSON_Duplicate(d, true);
    }

    scrivi_nvs(cJSON_GetObjectItem(dentro, "nvs"));
    scrivi_file(cJSON_GetObjectItem(dentro, "file"));

    int quanti = cJSON_GetArraySize(cJSON_GetObjectItem(dentro, "nvs"));
    cJSON_Delete(dentro);
    cJSON_Delete(fuori);
    ESP_LOGW(TAG, "configurazione ripristinata (%d spazi): serve un riavvio", quanti);
    snprintf(err, err_sz, "Ripristinata. Riavvia il pannello per applicarla.");
    return true;
}

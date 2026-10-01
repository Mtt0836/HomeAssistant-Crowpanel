#include "web_cert.h"
#include "net_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

/* ESP-IDF 6 porta mbedTLS 4, e qui si sente piu' che altrove.

   Quattro delle intestazioni che serviva includere prima sono diventate
   private: ecp.h, entropy.h, ctr_drbg.h e sha256.h stanno sotto
   mbedtls/private/ e non si possono piu' usare da fuori. Quello che facevano
   lo fanno adesso le PSA Crypto API, che sono il modo nuovo e unico di
   chiedere crittografia a mbedTLS.

   La parte grossa, invece, e' rimasta: pk.h, x509_crt.h e oid.h sono ancora
   pubbliche, quindi il certificato si scrive esattamente come prima. Cambiano
   solo due cose - da dove arriva la casualita' e come nasce la chiave - e
   sono proprio quelle che stavano dietro le intestazioni scomparse. */
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"

static const char *TAG = "web_cert";

#define NVS_NS   "webcert"
#define K_CERT   "cert"
#define K_KEY    "key"
#define K_IP     "ip"

#define CERT_BUF 2048
#define KEY_BUF  1024

static char *s_cert = NULL, *s_key = NULL;
static size_t s_cert_len = 0, s_key_len = 0;

static void current_ip(char *out, size_t sz)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr)
        snprintf(out, sz, IPSTR, IP2STR(&ip.ip));
    else
        snprintf(out, sz, "0.0.0.0");
}

// ------------------------------------------------------------------ generazione

/* La chiave privata del pannello: si fa nascere in PSA e poi si copia qui.

   mbedtls_pk_setup() + mbedtls_ecp_gen_key() non esistono piu'. Il giro nuovo
   e' psa_generate_key() e poi mbedtls_pk_copy_from_psa(). E' "copy" e non
   "wrap" per un motivo preciso: dopo la copia il contesto pk contiene la sua
   chiave ed e' indipendente, e da li' si puo' tirare fuori il PEM da salvare
   nella memoria del pannello. Una chiave soltanto "avvolta" resterebbe dentro
   PSA e non si potrebbe scrivere.

   Da questo viene anche PSA_KEY_USAGE_EXPORT: senza quel permesso la copia
   non e' consentita, e la chiave non uscirebbe mai. Vive lo spazio di due
   righe - il tempo di diventare PEM - e poi la si butta via da PSA.

   La curva e' la stessa di prima, secp256r1: il certificato e la chiave gia'
   salvati nella memoria continuano a valere e il pannello non rigenera
   niente. */
static bool genera_chiave(mbedtls_pk_context *key)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_generate_key(&attr, &kid);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key: %d", (int)st);
        return false;
    }

    int e = mbedtls_pk_copy_from_psa(kid, key);
    psa_destroy_key(kid);
    if (e != 0) {
        ESP_LOGE(TAG, "pk_copy_from_psa: -0x%04x", (unsigned)-e);
        return false;
    }
    return true;
}

static bool generate(const char *host, const char *ip, char **cert_out, size_t *cert_len,
                     char **key_out, size_t *key_len)
{
    int ret = -1;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    char *cert_pem = NULL, *key_pem = NULL;
    char subject[128];

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    /* Va chiamata prima di qualunque altra psa_*. E' innocua se qualcun altro
       l'ha gia' fatta: la seconda volta non fa niente e risponde bene. */
    if (psa_crypto_init() != PSA_SUCCESS) goto done;

    /* Il seme e il generatore non si preparano piu' a mano. Prima si mescolava
       una stringa fissa con l'entropia di mbedTLS per avviare un CTR-DRBG;
       adesso ci pensa PSA, che sull'ESP32-P4 pesca dal generatore casuale
       hardware. Meno righe, e una casualita' di partenza migliore. */
    if (!genera_chiave(&key)) goto done;

    snprintf(subject, sizeof(subject), "CN=%s,O=Pannello Home Assistant", host);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);          // firmato da se stesso
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) goto done;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0) goto done;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    /* L'orologio del pannello non e' ancora sincronizzato quando serve il
       certificato: date fisse e larghe, tanto non c'e' nessuno che lo revoca. */
    if (mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000") != 0) goto done;
    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    mbedtls_x509write_crt_set_authority_key_identifier(&crt);

    {   // numero di serie casuale
        unsigned char sn[16];
        if (psa_generate_random(sn, sizeof(sn)) != PSA_SUCCESS) goto done;
        sn[0] &= 0x7f;                       // deve restare positivo
        if (mbedtls_x509write_crt_set_serial_raw(&crt, sn, sizeof(sn)) != 0) goto done;
    }

    {   /* Nomi alternativi: il browser guarda questi, non il CN. Ci metto il
           nome del pannello, lo stesso con .local e l'indirizzo IP. */
        char host_local[80];
        snprintf(host_local, sizeof(host_local), "%s.local", host);
        mbedtls_x509_san_list san_ip = {};
        mbedtls_x509_san_list san_loc = {};
        mbedtls_x509_san_list san_dns = {};
        san_dns.node.type = MBEDTLS_X509_SAN_DNS_NAME;    san_dns.next = &san_loc;
        san_loc.node.type = MBEDTLS_X509_SAN_DNS_NAME;    san_loc.next = &san_ip;
        san_ip.node.type  = MBEDTLS_X509_SAN_IP_ADDRESS;  san_ip.next  = NULL;
        unsigned char ip_raw[4] = {0, 0, 0, 0};
        unsigned int a, b, c, d;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ip_raw[0] = a; ip_raw[1] = b; ip_raw[2] = c; ip_raw[3] = d;
        }
        san_dns.node.san.unstructured_name.p   = (unsigned char *)host;
        san_dns.node.san.unstructured_name.len = strlen(host);
        san_loc.node.san.unstructured_name.p   = (unsigned char *)host_local;
        san_loc.node.san.unstructured_name.len = strlen(host_local);
        san_ip.node.san.unstructured_name.p    = ip_raw;
        san_ip.node.san.unstructured_name.len  = sizeof(ip_raw);
        if (mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_dns) != 0)
            ESP_LOGW(TAG, "nomi alternativi non inseriti: il browser protestera' di piu'");

        cert_pem = (char *)calloc(1, CERT_BUF);
        key_pem  = (char *)calloc(1, KEY_BUF);
        if (!cert_pem || !key_pem) goto done;

        int64_t t0 = esp_timer_get_time();
        /* Due parametri in meno: in mbedTLS 4 questa funzione non si fa piu'
           passare il generatore casuale, se lo prende da PSA per conto suo. */
        if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, CERT_BUF) != 0) goto done;
        if (mbedtls_pk_write_key_pem(&key, (unsigned char *)key_pem, KEY_BUF) != 0) goto done;
        ESP_LOGI(TAG, "certificato generato per %s / %s in %d ms", host, ip,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }

    *cert_out = cert_pem; *cert_len = strlen(cert_pem) + 1;
    *key_out  = key_pem;  *key_len  = strlen(key_pem) + 1;
    cert_pem = key_pem = NULL;
    ret = 0;

done:
    free(cert_pem);
    free(key_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (ret) ESP_LOGE(TAG, "generazione del certificato non riuscita");
    return ret == 0;
}

// ------------------------------------------------------------------ memoria

static bool load_from_nvs(const char *ip)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = false;
    char saved_ip[16] = "";
    size_t n = sizeof(saved_ip);
    nvs_get_str(h, K_IP, saved_ip, &n);
    if (strcmp(saved_ip, ip) == 0) {              // stesso indirizzo: va bene
        size_t cl = 0, kl = 0;
        if (nvs_get_blob(h, K_CERT, NULL, &cl) == ESP_OK &&
            nvs_get_blob(h, K_KEY,  NULL, &kl) == ESP_OK && cl && kl) {
            char *c = (char *)calloc(1, cl), *k = (char *)calloc(1, kl);
            if (c && k &&
                nvs_get_blob(h, K_CERT, c, &cl) == ESP_OK &&
                nvs_get_blob(h, K_KEY,  k, &kl) == ESP_OK) {
                s_cert = c; s_cert_len = cl;
                s_key  = k; s_key_len  = kl;
                ok = true;
            } else { free(c); free(k); }
        }
    }
    nvs_close(h);
    return ok;
}

static void save_to_nvs(const char *ip)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, K_CERT, s_cert, s_cert_len);
    nvs_set_blob(h, K_KEY,  s_key,  s_key_len);
    nvs_set_str(h, K_IP, ip);
    nvs_commit(h);
    nvs_close(h);
}

bool web_cert_get(const char **cert_pem, size_t *cert_len,
                  const char **key_pem,  size_t *key_len)
{
    char ip[16];
    current_ip(ip, sizeof(ip));

    if (!s_cert) {
        if (!load_from_nvs(ip)) {
            net_config_t nc;
            net_config_load(&nc);
            const char *host = nc.hostname[0] ? nc.hostname : "pannello";
            if (!generate(host, ip, &s_cert, &s_cert_len, &s_key, &s_key_len)) return false;
            save_to_nvs(ip);
        }
    }
    *cert_pem = s_cert; *cert_len = s_cert_len;
    *key_pem  = s_key;  *key_len  = s_key_len;
    return true;
}

void web_cert_fingerprint(char *out, size_t out_sz)
{
    out[0] = 0;
    if (!s_cert) return;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)s_cert, s_cert_len) == 0) {
        /* mbedtls_sha256() stava in sha256.h, ora privata: lo stesso conto si
           chiede a PSA. L'impronta resta lo SHA-256 del certificato in forma
           grezza, quindi quella che mostra il browser non cambia. */
        unsigned char sha[32];
        size_t quanti = 0;
        if (psa_crypto_init() == PSA_SUCCESS &&
            psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len,
                             sha, sizeof(sha), &quanti) == PSA_SUCCESS &&
            quanti == sizeof(sha)) {
            size_t pos = 0;
            for (int i = 0; i < 32 && pos + 3 < out_sz; i++)
                pos += snprintf(out + pos, out_sz - pos, i ? ":%02X" : "%02X", sha[i]);
        }
    }
    mbedtls_x509_crt_free(&crt);
}

#include "batteria.h"
#include "idle_manager.h"          // idle_manager_screen_on(): il peso del tempo
#include "../../espressif__esp32_p4_function_ev_board/bsp_stc8h1kxx.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char *TAG = "batt";

#define NS              "batt"
#define LOG_PATH        "/sdcard/batteria.csv"
#define LOG_VECCHIO     "/sdcard/batteria.1.csv"
#define LOG_MAX         (4 * 1024 * 1024)
#define HEADER  "data_ora,uptime_s,mv,mv_filtrato,pct,pct_stc8,stato,stato_grezzo," \
                "led,schermo,mv_riposo,caduta_schermo,corsa_s,corsa_carica\n"

/* Ogni quanto si interroga il coprocessore. Il factory lo faceva ogni secondo,
   e ogni giro sono dodici transazioni I2C separate (il driver legge un
   registro per volta). Quel bus lo condivide il touch, che ci ha gia' dato
   1205 errori in 71 secondi quando era sotto pressione: due secondi danno la
   stessa informazione con meta' del traffico. Il registro e' al minuto, la
   risoluzione non ne soffre. */
#define PERIODO_MS      2000
#define LOG_PERIODO_S   60

/* Spegnimento di protezione. La soglia e' quella del firmware originale: una
   litio portata sotto non si riprende piu'. Quello che cambia e' come ci si
   arriva - vedi il commento su conta_basse in campiona(). */
#define MV_SPEGNI       3500

/* Fine di una scarica registrata. Deve stare SOPRA MV_SPEGNI, altrimenti il
   pannello si spegne prima di aver visto il fondo e la corsa non si chiude
   mai. Ne viene una conseguenza che vale la pena dire: per questo pannello
   lo zero per cento non e' la cella scarica, e' il punto in cui il pannello
   si spegne. Che e' proprio quello che interessa a chi guarda. */
#define MV_FINE_CORSA   3560

// Da quanto in su una cella si puo' considerare piena abbastanza per tararci sopra.
#define MV_PIENA_MIN    3900

/* Quanto consuma il pannello a schermo acceso rispetto a spento. Serve a
   pesare il tempo: con corrente costante il tempo misura la carica, ma i
   primi minuti di una scarica lo schermo e' ancora accesso e consuma di piu'.
   E' una stima. Sbagliarla sposta quei pochi minuti su molte ore di scarica,
   quindi l'errore sulla curva e' trascurabile; e il registro tiene le colonne
   grezze, cosi' sul computer si puo' rifare il conto con un peso migliore.

   Chi ha un amperometro a pinza puo' togliere la stima di mezzo: con
   "batteria ma <spento> <acceso>" il rapporto diventa misurato. */
#define PESO_ACCESO     2.8f

/* Tabella di partenza: litio a cella singola, dallo spegnimento del pannello
   al pieno. Conta solo finche' il pannello non si e' tarato da solo.

   Il 100% sta a 4120 mV e non a 4200, e la ragione vale la pena di scriverla
   perche' e' l'errore classico. I 4200 mV sono la tensione di FINE CARICA,
   quella che si misura mentre il caricatore sta ancora spingendo; appena la
   carica termina la cella si assesta un centinaio di millivolt sotto e li'
   resta. Una tabella ancorata a 4200 mostra quindi una cella piena all'87%
   per sempre - che e' esattamente il numero a cui si era fermato questo
   pannello col firmware del coprocessore, dopo giorni attaccato alla corrente.
   Ancorare al valore a riposo e' l'unica scala che corrisponde a quello che la
   cella fa davvero.

   Dopo una scarica di taratura tutto questo non conta piu': il punto al 100%
   diventa la tensione da cui la scarica e' partita, misurata su questa cella. */
static const uint16_t CURVA_STANDARD[BATT_PUNTI] = {
    3500, 3560, 3610, 3640, 3670, 3700, 3720, 3740, 3760, 3780, 3800,
    3820, 3850, 3870, 3900, 3930, 3960, 4000, 4040, 4080, 4120,
};

struct Campione { float carica; uint16_t mv; };
#define MAX_CAMPIONI 1200          // 20 ore al minuto; oltre, si dirada

static SemaphoreHandle_t s_mtx = nullptr;
static batt_info_t       s_info;
static uint16_t          s_curva[BATT_PUNTI];
static bool              s_imparata = false;
static uint32_t          s_offset_mv = 0;
static uint16_t          s_ma_spento = 0, s_ma_acceso = 0;
static uint16_t          s_capacita_mah = 0;
/* I secondi pesati dell'ultima scarica completa, tenuti in NVS a parte.
   La capacita' in mAh si ricava da questi moltiplicando per la corrente, e la
   corrente puo' arrivare molto dopo la scarica - la si misura a pinza, quando
   si ha tempo. Senza questo numero chi misurasse dopo avrebbe perso il treno
   e dovrebbe rifare una scarica di venti ore per niente. */
static uint32_t          s_carica_ultima_s = 0;

/* Il rapporto fra i due consumi: misurato se qualcuno ha passato le correnti,
   stimato altrimenti. */
static float peso_acceso(void)
{
    if (s_ma_spento && s_ma_acceso) return (float)s_ma_acceso / (float)s_ma_spento;
    return PESO_ACCESO;
}

/* I secondi pesati sono secondi "come se lo schermo fosse spento": per farne
   mAh basta la corrente a schermo spento. Senza quella non si puo' sapere. */
static uint16_t mah_da_carica(float carica_s)
{
    if (!s_ma_spento || carica_s <= 0) return 0;
    float mah = carica_s * (float)s_ma_spento / 3600.0f;
    return mah > 65000 ? 0 : (uint16_t)mah;
}

// stato della corsa di taratura
static Campione *s_camp = nullptr;
static size_t    s_n_camp = 0;
static uint32_t  s_camp_passo_s = LOG_PERIODO_S;
static bool      s_corsa = false;
static float     s_carica = 0;         // secondi pesati dall'inizio della corsa
static int64_t   s_corsa_inizio_us = 0;
static uint32_t  s_corsa_mv0 = 0;
static uint32_t  s_corsa_mv_min = 0;
static uint32_t  s_picco_mv = 0;
/* Vero solo dopo aver visto con i propri occhi la cella piena, in questa
   accensione. Serve a non tarare su una scarica cominciata prima del riavvio:
   al riavvio il picco si ricostruisce dalla tensione di quel momento, che in
   mezzo a una scarica e' gia' scesa, e la curva che ne verrebbe chiamerebbe
   "pieno" una cella a meta'. Chi la usasse poi vedrebbe percentuali gonfiate
   sul serio - un danno peggiore del non essere tarati. */
static bool      s_visto_pieno = false;

// misura dell'offset di carica: tensione al distacco, poi di nuovo dopo un po'
static uint32_t s_mv_al_distacco = 0;
static int64_t  s_distacco_us = 0;

/* Di quanto lo schermo acceso fa crollare la tensione ai morsetti.

   Non e' un dettaglio: sul registro di questo pannello sono 163 mV. La
   batteria non si e' svuotata in un istante, e' la resistenza interna della
   cella piu' i fili sotto il mezzo ampere in piu' della retroilluminazione.
   Appena lo schermo si rispegne la tensione risale.

   Senza correggerlo la stima andava a rotoli, e in un modo che si sarebbe
   notato solo dopo giorni: a batteria la percentuale puo' solo scendere - cosi'
   un numero di cui fidarsi non torna mai indietro - quindi OGNI accensione
   dello schermo la faceva crollare, e il rispegnimento non la recuperava. Dopo
   qualche tocco il pannello si sarebbe dichiarato quasi scarico con la cella
   ancora mezza piena.

   Si misura come il gonfiamento della carica: si guarda di quanto risale la
   tensione trenta secondi dopo che lo schermo si e' spento. */
static uint32_t s_caduta_schermo_mv = 0;
static uint32_t s_mv_prima_spento = 0;
static int64_t  s_spento_us = 0;

// ------------------------------------------------------------------ tabella

static uint8_t pct_da_mv(const uint16_t *tab, uint32_t mv)
{
    if (mv <= tab[0]) return 0;
    if (mv >= tab[BATT_PUNTI - 1]) return 100;
    for (int i = 1; i < BATT_PUNTI; i++) {
        if (mv < tab[i]) {
            uint32_t basso = tab[i - 1], alto = tab[i];
            if (alto <= basso) return (uint8_t)(5 * i);
            uint32_t dentro = (mv - basso) * 5 / (alto - basso);    // 0..4
            return (uint8_t)(5 * (i - 1) + dentro);
        }
    }
    return 100;
}

/* Una curva si accetta solo se e' crescente e copre un intervallo sensato.
   Una tabella non crescente darebbe percentuali che vanno a ritroso, e una
   schiacciata (tutta dentro 100 mV) vorrebbe dire che la corsa non ha
   misurato niente. */
static bool curva_sensata(const uint16_t *tab)
{
    for (int i = 1; i < BATT_PUNTI; i++)
        if (tab[i] <= tab[i - 1]) return false;
    if (tab[0] < 2800 || tab[BATT_PUNTI - 1] > 4400) return false;
    return tab[BATT_PUNTI - 1] - tab[0] >= 300;
}

// ------------------------------------------------------------------ NVS

static void curva_carica_da_nvs(void)
{
    memcpy(s_curva, CURVA_STANDARD, sizeof(s_curva));
    s_imparata = false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t tab[BATT_PUNTI];
    size_t sz = sizeof(tab);
    if (nvs_get_blob(h, "curva", tab, &sz) == ESP_OK && sz == sizeof(tab) &&
        curva_sensata(tab)) {
        memcpy(s_curva, tab, sizeof(s_curva));
        s_imparata = true;
        uint32_t q = 0, d = 0;
        nvs_get_u32(h, "quando", &q);
        nvs_get_u32(h, "durata", &d);
        s_info.curva_quando = q;
        s_info.curva_durata_s = d;
        ESP_LOGI(TAG, "curva imparata in uso: %u mV allo 0%%, %u mV al 100%%",
                 (unsigned)s_curva[0], (unsigned)s_curva[BATT_PUNTI - 1]);
    }
    uint32_t off = 0;
    if (nvs_get_u32(h, "offset", &off) == ESP_OK && off <= 400) s_offset_mv = off;
    nvs_get_u16(h, "ma_off", &s_ma_spento);
    nvs_get_u16(h, "ma_on",  &s_ma_acceso);
    nvs_get_u16(h, "mah",    &s_capacita_mah);
    nvs_get_u32(h, "carica", &s_carica_ultima_s);
    uint32_t cs = 0;
    if (nvs_get_u32(h, "cadschermo", &cs) == ESP_OK && cs <= 400) s_caduta_schermo_mv = cs;
    nvs_close(h);
}

void batteria_imposta_correnti(uint16_t ma_spento, uint16_t ma_acceso)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "ma_off", ma_spento);
    nvs_set_u16(h, "ma_on",  ma_acceso);
    s_ma_spento = ma_spento;
    s_ma_acceso = ma_acceso;
    /* Se una scarica completa c'e' gia' stata, la capacita' si ricava adesso:
       mancava solo la corrente. */
    uint16_t mah = mah_da_carica((float)s_carica_ultima_s);
    if (mah) {
        s_capacita_mah = mah;
        nvs_set_u16(h, "mah", mah);
        ESP_LOGI(TAG, "capacita' ricavata dalla scarica gia' registrata: %u mAh", mah);
    }
    nvs_commit(h);
    nvs_close(h);
    if (ma_spento && ma_acceso)
        ESP_LOGI(TAG, "consumi misurati: %u mA a schermo spento, %u acceso "
                      "(rapporto %.2f)", ma_spento, ma_acceso, peso_acceso());
    else
        ESP_LOGI(TAG, "consumi dimenticati: torno al rapporto stimato %.2f", peso_acceso());
}

static void curva_salva(const uint16_t *tab, uint32_t durata_s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t quando = (tm.tm_year + 1900 >= 2024) ? (uint32_t)now : 0;
    nvs_set_blob(h, "curva", tab, sizeof(uint16_t) * BATT_PUNTI);
    nvs_set_u32(h, "quando", quando);
    nvs_set_u32(h, "durata", durata_s);
    nvs_commit(h);
    nvs_close(h);
    memcpy(s_curva, tab, sizeof(uint16_t) * BATT_PUNTI);
    s_imparata = true;
    s_info.curva_quando = quando;
    s_info.curva_durata_s = durata_s;
}

static void offset_salva(uint32_t mv)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "offset", mv);
    nvs_commit(h);
    nvs_close(h);
    s_offset_mv = mv;
}

void batteria_azzera_curva(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "curva");
        nvs_erase_key(h, "quando");
        nvs_erase_key(h, "durata");
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_curva, CURVA_STANDARD, sizeof(s_curva));
    s_imparata = false;
    s_info.curva_imparata = false;
    s_info.curva_quando = 0;
    s_info.curva_durata_s = 0;
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "curva imparata cancellata: torno alla tabella standard");
}

// ------------------------------------------------------------------ registro

/* Il file corrente diventa la generazione di riserva. Una sola: la
   precedente si butta, perche' la storia di mesi fa non vale una scheda
   piena. */
static void metti_da_parte(void)
{
    remove(LOG_VECCHIO);
    rename(LOG_PATH, LOG_VECCHIO);
}

static void log_ruota(void)
{
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    if (n <= LOG_MAX) return;
    /* Una generazione di riserva e poi si ricomincia: il file lo si va a
       leggere sul computer, e perdere la storia di mesi fa non costa niente.
       Riempire la scheda invece fermerebbe lo slideshow. */
    metti_da_parte();
    ESP_LOGI(TAG, "registro ruotato (%ld byte)", n);
}

/* L'intestazione descrive le colonne delle righe che seguono, e dopo un
   aggiornamento del firmware puo' non descriverle piu'. Succede davvero: la
   riga e' passata da dodici a quattordici colonne, e il file sulla scheda
   cominciava ancora con l'intestazione a dodici - appesa una volta sola, a
   file vuoto, mesi prima.

   Appendere righe nuove sotto un'intestazione vecchia sarebbe lo stesso
   difetto di prima, spostato dal codice al file: un registro che si rilegge
   male senza dirlo. Quindi al primo uso dopo l'accensione si controlla, e se
   non combacia il file si mette da parte come nella rotazione. Non si perde
   niente - resta sotto LOG_VECCHIO - e il file nuovo nasce con la sua
   intestazione giusta.

   Una volta per accensione: leggere la prima riga a ogni append costerebbe
   un'apertura in piu' al minuto per nulla. */
static void intestazione_controlla(void)
{
    static bool fatto = false;
    if (fatto) return;
    fatto = true;

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;                       // non c'e': nascera' giusto
    char prima[sizeof(HEADER) + 8];
    char *letto = fgets(prima, sizeof(prima), f);
    fclose(f);
    if (!letto) return;                   // vuoto: nascera' giusto
    if (strcmp(prima, HEADER) == 0) return;

    ESP_LOGW(TAG, "il registro ha un'intestazione di un firmware precedente: "
                  "lo metto da parte in %s e ricomincio", LOG_VECCHIO);
    metti_da_parte();
}

static void log_riga(const char *riga)
{
    intestazione_controlla();
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        /* Senza SD si perde il registro, non la stima: non e' un guasto da
           fermare tutto. Ma va detto una volta, altrimenti domani si va a
           cercare la curva di una scarica che nessuno ha scritto. Una volta
           sola: al minuto per venti ore sarebbero mille righe identiche. */
        static bool detto = false;
        if (!detto) {
            detto = true;
            ESP_LOGW(TAG, "non riesco a scrivere %s (%s): niente registro ne' "
                          "autotaratura, la stima resta sulla tabella standard",
                     LOG_PATH, strerror(errno));
        }
        return;
    }
    if (ftell(f) == 0) fputs(HEADER, f);
    fputs(riga, f);
    fclose(f);
    log_ruota();
}

static void log_campione(const batt_info_t *i, bool schermo)
{
    char quando[24] = "-";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) strftime(quando, sizeof(quando), "%Y-%m-%d %H:%M:%S", &tm);

    /* Quattordici colonne, quante ne dichiara HEADER. Le ultime due sono
       quelle che rendono il registro verificabile: senza i secondi della
       corsa e la carica consumata il file racconta la scarica ma non la
       taratura, e la curva non si puo' ricalcolare sul computer.

       Qui si contano i segnaposti a mano una volta sola, perche' fin qui
       nessuno li contava: erano dodici per quattordici argomenti, i due
       della corsa cadevano nel vuoto e la caduta dello schermo - un
       uint32_t - finiva in un %.0f, che e' comportamento indefinito. Il
       compilatore lo avrebbe detto subito, ma -Wno-format lo zittiva. */
    char riga[220];
    snprintf(riga, sizeof(riga), "%s,%u,%u,%u,%u,%u,%s,%u,%u,%s,%u,%u,%u,%.0f\n",
             quando, (unsigned)(esp_timer_get_time() / 1000000),
             (unsigned)i->mv, (unsigned)i->mv_filtrato,
             (unsigned)i->pct, (unsigned)i->pct_stc8,
             batteria_stato_parole(i->stato),
             (unsigned)i->stato_grezzo, (unsigned)i->led_grezzo,
             schermo ? "acceso" : "spento",
             (unsigned)i->mv_riposo, (unsigned)i->caduta_schermo_mv,
             s_corsa ? (unsigned)((esp_timer_get_time() - s_corsa_inizio_us) / 1000000) : 0u,
             (double)(s_corsa ? s_carica : 0.0f));
    log_riga(riga);
}

// ------------------------------------------------------------------ taratura

static uint16_t mv_a_carica(const Campione *c, size_t n, float q)
{
    if (n == 0) return 0;
    if (q <= c[0].carica) return c[0].mv;
    if (q >= c[n - 1].carica) return c[n - 1].mv;
    for (size_t i = 1; i < n; i++) {
        if (q <= c[i].carica) {
            float d = c[i].carica - c[i - 1].carica;
            if (d <= 0) return c[i].mv;
            float f = (q - c[i - 1].carica) / d;
            return (uint16_t)(c[i - 1].mv + f * ((float)c[i].mv - (float)c[i - 1].mv));
        }
    }
    return c[n - 1].mv;
}

static void corsa_libera(void)
{
    if (s_camp) heap_caps_free(s_camp);
    s_camp = nullptr;
    s_n_camp = 0;
    s_camp_passo_s = LOG_PERIODO_S;
    s_corsa = false;
    s_carica = 0;
}

static void corsa_inizia(uint32_t mv)
{
    if (s_camp) corsa_libera();
    s_camp = (Campione *)heap_caps_malloc(sizeof(Campione) * MAX_CAMPIONI, MALLOC_CAP_SPIRAM);
    if (!s_camp) {
        ESP_LOGW(TAG, "niente memoria per registrare la scarica: non mi taro");
        return;
    }
    s_n_camp = 0;
    s_carica = 0;
    s_corsa_inizio_us = esp_timer_get_time();
    s_corsa_mv0 = mv;
    s_corsa_mv_min = mv;
    s_corsa = true;
    /* Il permesso si consuma. Una corsa interrotta a meta' - carica ripartita,
       carico cambiato - non si riprende da dove era: per tararsi di nuovo
       bisogna rifare il pieno. Una scarica raccolta a pezzi non misura
       niente. */
    s_visto_pieno = false;
    ESP_LOGI(TAG, "scarica in corso da %u mV: registro per tararmi", (unsigned)mv);
    log_riga("# inizio scarica\n");
}

static void corsa_annulla(const char *perche)
{
    if (!s_corsa) return;
    ESP_LOGI(TAG, "scarica scartata: %s", perche);
    char riga[96];
    snprintf(riga, sizeof(riga), "# scarica scartata: %s\n", perche);
    log_riga(riga);
    corsa_libera();
}

/* Un campione in piu'. Quando lo spazio finisce si tiene un campione su due e
   si raddoppia il passo: la corsa continua senza limite di durata, perdendo
   solo risoluzione dove non serve. */
static void corsa_campiona(uint32_t mv)
{
    if (!s_corsa || !s_camp) return;
    if (s_n_camp >= MAX_CAMPIONI) {
        for (size_t i = 0; i * 2 < s_n_camp; i++) s_camp[i] = s_camp[i * 2];
        s_n_camp = (s_n_camp + 1) / 2;
        s_camp_passo_s *= 2;
    }
    s_camp[s_n_camp].carica = s_carica;
    s_camp[s_n_camp].mv = (uint16_t)mv;
    s_n_camp++;
}

/* La corsa e' arrivata al fondo: se e' credibile, se ne ricava la curva.

   Il conto e' quello spiegato in batteria.h: carica zero = cella piena,
   carica totale = pannello che si spegne. Il punto al 5*i per cento sta dove
   la carica consumata e' (100 - 5*i)/100 del totale. */
static void corsa_concludi(void)
{
    if (!s_corsa || !s_camp) { corsa_libera(); return; }

    uint32_t durata_s = (uint32_t)((esp_timer_get_time() - s_corsa_inizio_us) / 1000000);
    uint32_t caduta = s_corsa_mv0 > s_camp[s_n_camp - 1].mv
                    ? s_corsa_mv0 - s_camp[s_n_camp - 1].mv : 0;

    /* Tre modi di non essere credibile: troppo breve (un calo passeggero,
       non una scarica), troppo pochi punti per disegnare una curva, troppa
       poca caduta di tensione. */
    if (durata_s < 30 * 60 || s_n_camp < 10 || caduta < 300) {
        char perche[120];
        snprintf(perche, sizeof(perche), "durata %u s, %u punti, caduta %u mV: non basta",
                 (unsigned)durata_s, (unsigned)s_n_camp, (unsigned)caduta);
        corsa_annulla(perche);
        return;
    }

    float tot = s_camp[s_n_camp - 1].carica;
    if (tot <= 0) { corsa_annulla("carica totale nulla"); return; }

    uint16_t tab[BATT_PUNTI];
    for (int i = 0; i < BATT_PUNTI; i++) {
        float q = tot * (float)(100 - 5 * i) / 100.0f;
        tab[i] = mv_a_carica(s_camp, s_n_camp, q);
    }
    /* Il rumore di misura puo' produrre due punti vicini invertiti. Li si
       raddrizza di un millivolt invece di buttare via tutta la corsa: la
       curva resta quella misurata, e resta usabile. */
    for (int i = 1; i < BATT_PUNTI; i++)
        if (tab[i] <= tab[i - 1]) tab[i] = tab[i - 1] + 1;

    if (!curva_sensata(tab)) { corsa_annulla("curva non sensata"); return; }

    curva_salva(tab, durata_s);
    ESP_LOGW(TAG, "TARATA su una scarica di %u s (%u punti): 0%%=%u mV, 50%%=%u mV, 100%%=%u mV",
             (unsigned)durata_s, (unsigned)s_n_camp,
             (unsigned)tab[0], (unsigned)tab[10], (unsigned)tab[BATT_PUNTI - 1]);

    /* La capacita' vera del pacco, se le correnti ci sono. Questo e' il numero
       che dice se i mAh stampati sull'etichetta sono quelli. */
    uint16_t mah = mah_da_carica(tot);
    s_carica_ultima_s = (uint32_t)tot;
    if (mah) s_capacita_mah = mah;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "carica", s_carica_ultima_s);
        if (mah) nvs_set_u16(h, "mah", mah);
        nvs_commit(h);
        nvs_close(h);
    }
    if (mah)
        ESP_LOGW(TAG, "capacita' misurata: %u mAh", mah);
    else
        ESP_LOGW(TAG, "scarica da %u secondi pesati: per i mAh mancano le correnti "
                      "(batteria ma <spento> <acceso>, anche fra un mese)",
                 (unsigned)s_carica_ultima_s);

    /* La curva finisce anche nel registro, in chiaro: il file e' il posto dove
       si va a vedere se la taratura ha senso, e doverla ricostruire dalla NVS
       per controllarla sarebbe assurdo. Una riga sola, non ventuno aperture
       del file. */
    char riga[220];
    int k = snprintf(riga, sizeof(riga), "# TARATA durata=%u s punti=%u mah=%u curva=",
                     (unsigned)durata_s, (unsigned)s_n_camp, mah);
    for (int i = 0; i < BATT_PUNTI && k > 0 && k < (int)sizeof(riga) - 8; i++)
        k += snprintf(riga + k, sizeof(riga) - k, "%u ", (unsigned)tab[i]);
    if (k > 0 && k < (int)sizeof(riga) - 1) snprintf(riga + k, sizeof(riga) - k, "\n");
    log_riga(riga);
    corsa_libera();
}

// ------------------------------------------------------------------ lettura

/* Senza spazi, e non e' una scelta di stile: questi valori arrivano a Home
   Assistant come stato di un sensore a scelta fissa, e li' le voci devono
   essere chiavi macchina ([a-z0-9-_]) perche' il nome visibile lo danno le
   traduzioni - altrimenti chi ha Home Assistant in inglese si ritrova
   l'italiano. hassfest rifiuta l'integrazione se non lo sono.

   Restano una lista sola anche se finiscono in tre posti diversi (Home
   Assistant, la console, il registro su SD): due liste che devono dire la
   stessa cosa prima o poi dicono cose diverse. Nel registro e sulla console
   "in_carica" si legge benissimo. */
const char *batteria_stato_parole(batt_stato_t s)
{
    switch (s) {
    case BATT_IN_CARICA:   return "in_carica";
    case BATT_CARICA:      return "carica";
    case BATT_A_BATTERIA:  return "a_batteria";
    case BATT_ERRORE:      return "errore";
    default:               return "sconosciuto";
    }
}

static batt_stato_t stato_da_grezzo(uint8_t g)
{
    switch (g) {
    case BAT_CHARGE_CHARGING:      return BATT_IN_CARICA;
    case BAT_CHARGE_FULLY_CHARGED: return BATT_CARICA;
    case BAT_CHARGE_NO_CHARGE:     return BATT_A_BATTERIA;
    case BAT_CHARGE_ERROR:         return BATT_ERRORE;
    default:                       return BATT_SCONOSCIUTO;   // BAT_CHARGE_IDLE
    }
}

static void campiona(void)
{
    static uint32_t filtro = 0;
    static int      conta_basse = 0;
    static uint32_t ultimo_log_s = 0;
    static batt_stato_t stato_prec = BATT_SCONOSCIUTO;
    static bool     primo = true;

    Battery_info_t b = {};
    bool ok = stc8_battery_info_get(&b) == ESP_OK && b.bat_voltage > 0;

    /* Spegnimento di protezione, fatto in modo che non possa scattare per
       sbaglio. Il firmware originale spegneva alla prima lettura sotto soglia,
       e la struttura la azzerava a ogni giro: stc8_battery_info_get esce in
       anticipo se l'I2C da' errore, quindi bat_voltage restava a zero, zero e'
       minore di 3500, e il pannello se ne andava in deep sleep. Un solo errore
       su un bus condiviso col touch e il pannello sembrava morto, da staccare
       dalla corrente per farlo tornare.

       Ora una lettura fallita non conclude niente, la tensione deve essere
       davvero sotto soglia per dieci letture di fila (venti secondi), e in
       carica non si spegne mai - sotto carica una tensione bassa vuol dire che
       la ricarica e' appena cominciata, non che la cella e' finita. */
    if (!ok) {
        conta_basse = 0;
    } else {
        batt_stato_t st = stato_da_grezzo(b.bat_state);
        if (st == BATT_IN_CARICA || b.bat_voltage > MV_SPEGNI) conta_basse = 0;
        else conta_basse++;
        if (conta_basse >= 10) {
            ESP_LOGE(TAG, "%u mV per venti secondi: spengo per non rovinare la cella",
                     (unsigned)b.bat_voltage);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_deep_sleep_start();
        }
    }
    if (!ok) return;

    if (primo) { filtro = b.bat_voltage; primo = false; }
    else       filtro += ((int32_t)b.bat_voltage - (int32_t)filtro) / 8;

    batt_stato_t st = stato_da_grezzo(b.bat_state);
    bool in_carica = (st == BATT_IN_CARICA);
    bool schermo = idle_manager_screen_on();

    // ---- offset di carica: quanto la carica gonfia la tensione
    if ((stato_prec == BATT_IN_CARICA || stato_prec == BATT_CARICA) &&
        st == BATT_A_BATTERIA) {
        s_mv_al_distacco = filtro;
        s_distacco_us = esp_timer_get_time();
    }
    if (s_distacco_us && st == BATT_A_BATTERIA &&
        esp_timer_get_time() - s_distacco_us > 120ll * 1000000) {
        /* Due minuti: il rilassamento della cella e' quasi tutto qui dentro,
           e in due minuti una batteria da qualche migliaio di mAh si scarica
           di cosi' poco che non sporca la misura. */
        uint32_t o = s_mv_al_distacco > filtro ? s_mv_al_distacco - filtro : 0;
        if (o <= 400) {
            uint32_t nuovo = s_offset_mv ? (s_offset_mv + o) / 2 : o;   // media con il passato
            if (nuovo != s_offset_mv) {
                offset_salva(nuovo);
                ESP_LOGI(TAG, "la carica gonfia la tensione di %u mV", (unsigned)nuovo);
            }
        }
        s_distacco_us = 0;
    }
    if (st == BATT_IN_CARICA || st == BATT_CARICA) s_distacco_us = 0;

    /* ---- quanto pesa lo schermo acceso, misurato quando si spegne

       Si misura la RIPRESA della tensione dopo lo spegnimento, e si aspetta
       perche' la cella non si riprende all'istante. Due minuti, gli stessi
       dell'offset di carica qui sopra e per lo stesso motivo: il rilassamento
       sta quasi tutto dentro quella finestra, e in due minuti una cella da
       qualche migliaio di mAh perde cosi' poca carica che non sporca la
       misura (a 250 mA sono 8 mAh su 4500, lo 0,2%).

       Prima si aspettavano trenta secondi, e sottostimava per due motivi che
       si sommavano: la cella non aveva finito di riprendersi, e il filtro -
       un passa-basso a un ottavo su periodo di due secondi, costante di tempo
       sui sedici secondi - a trenta secondi era arrivato all'85% del gradino.
       Risultato misurato sul ciclo del 6-7 ottobre 2026: 22 mV salvati contro
       i ~40 che le transizioni nello stesso registro mostrano.

       Serve anche che lo schermo sia stato acceso per un minuto almeno: su
       un'accensione breve il filtro non ha avuto il tempo di scendere alla
       tensione sotto carico, quindi il punto di partenza sarebbe troppo alto
       e la ripresa misurata troppo piccola - lo stesso errore dall'altro
       capo.

       Con una finestra piu' larga le misure sono piu' rare, perche' basta che
       lo schermo si riaccenda entro due minuti per annullarla. E' il verso
       giusto in cui sbagliare: meglio poche misure buone che molte storte,
       tanto il valore si media con quelle di prima e non serve in fretta. */
    {
        static bool schermo_prec = true;
        static int64_t s_acceso_us = 0;
        if (!schermo_prec && schermo) s_acceso_us = esp_timer_get_time();
        if (schermo_prec && !schermo) {        // appena spento
            bool abbastanza = s_acceso_us &&
                (esp_timer_get_time() - s_acceso_us) > 60ll * 1000000;
            s_mv_prima_spento = filtro;
            s_spento_us = abbastanza ? esp_timer_get_time() : 0;
        }
        /* Si misura solo a batteria: sotto carica la tensione la tiene il
           caricatore e la caduta non si vede. E si annulla se lo schermo
           torna acceso prima della misura. */
        if (schermo || st != BATT_A_BATTERIA) s_spento_us = 0;
        if (s_spento_us && esp_timer_get_time() - s_spento_us > 120ll * 1000000) {
            uint32_t c = filtro > s_mv_prima_spento ? filtro - s_mv_prima_spento : 0;
            if (c <= 400) {
                uint32_t nuovo = s_caduta_schermo_mv ? (s_caduta_schermo_mv + c) / 2 : c;
                if (nuovo != s_caduta_schermo_mv) {
                    s_caduta_schermo_mv = nuovo;
                    nvs_handle_t h;
                    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u32(h, "cadschermo", nuovo);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    ESP_LOGI(TAG, "lo schermo acceso costa %u mV di tensione", (unsigned)nuovo);
                }
            }
            s_spento_us = 0;
        }
        schermo_prec = schermo;
    }

    // ---- la stima
    /* Si lavora sempre sulla tensione "a riposo": quella misurata corretta per
       cio' che in questo momento la falsa. Sotto carica il caricatore la alza,
       a schermo acceso il consumo la abbassa. La curva - standard o imparata -
       e' una curva di tensioni a riposo, e confrontarci una tensione sotto
       carico vuol dire leggere il punto sbagliato. */
    uint32_t mv_vero = filtro;
    if (in_carica && s_offset_mv < filtro)
        mv_vero = filtro - s_offset_mv;
    else if (schermo && st == BATT_A_BATTERIA)
        mv_vero = filtro + s_caduta_schermo_mv;
    uint8_t grezza = pct_da_mv(s_curva, mv_vero);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_info.pct == 0 && s_info.stato == BATT_SCONOSCIUTO) {
        s_info.pct = grezza;                       // primo valore: la verita' nuda
    } else if (in_carica) {
        /* In carica la percentuale puo' solo salire, e il 100% lo decide il
           coprocessore dicendo "piena" - non la tensione, che al 4,2 V arriva
           con la cella ancora a tre quarti. Un numero che non torna indietro
           e' un numero di cui si puo' fidare. */
        if (grezza > s_info.pct) s_info.pct = grezza;
        if (s_info.pct > 99) s_info.pct = 99;
    } else if (st == BATT_CARICA) {
        s_info.pct = 100;
    } else if (st == BATT_A_BATTERIA) {
        if (grezza < s_info.pct) s_info.pct = grezza;
    } else {
        s_info.pct = grezza;
    }
    s_info.letta = true;
    s_info.mv = b.bat_voltage;
    s_info.mv_filtrato = filtro;
    s_info.mv_riposo = mv_vero;
    s_info.caduta_schermo_mv = s_caduta_schermo_mv;
    s_info.pct_stc8 = b.bat_level;
    s_info.stato = st;
    s_info.stato_grezzo = b.bat_state;
    s_info.led_grezzo = b.led_state;
    s_info.curva_imparata = s_imparata;
    s_info.offset_carica_mv = s_offset_mv;
    s_info.corsa_attiva = s_corsa;
    s_info.ma_spento = s_ma_spento;
    s_info.ma_acceso = s_ma_acceso;
    s_info.capacita_mah = s_capacita_mah;
    /* L'autonomia: la carica che resta divisa per quello che sta assorbendo
       adesso. Solo a batteria - sotto carica non vuol dire niente - e solo
       se qualcuno ha misurato i consumi. */
    s_info.autonomia_min = 0;
    if (s_capacita_mah && s_ma_spento && st == BATT_A_BATTERIA) {
        uint16_t ma = schermo && s_ma_acceso ? s_ma_acceso : s_ma_spento;
        float min = (float)s_capacita_mah * (float)s_info.pct / 100.0f * 60.0f / (float)ma;
        if (min > 0 && min < 65000) s_info.autonomia_min = (uint16_t)min;
    }
    batt_info_t copia = s_info;
    xSemaphoreGive(s_mtx);

    // ---- la corsa di taratura
    if (filtro > s_picco_mv && !s_corsa) s_picco_mv = filtro;
    if ((st == BATT_CARICA || in_carica) && filtro >= MV_PIENA_MIN) s_visto_pieno = true;

    if (s_corsa) {
        s_carica += (PERIODO_MS / 1000.0f) * (schermo ? peso_acceso() : 1.0f);
        if (filtro < s_corsa_mv_min) s_corsa_mv_min = filtro;
        if (in_carica) {
            corsa_annulla("e' ripartita la carica");
            s_picco_mv = filtro;
        } else if (filtro > s_corsa_mv_min + 40) {
            /* La tensione risale: o e' tornata la corrente, o il carico e'
               cambiato di brutto. In nessuno dei due casi il tempo misura
               ancora la carica consumata, e una curva ricavata da qui
               sarebbe peggio di nessuna curva. */
            corsa_annulla("la tensione e' risalita");
            s_picco_mv = filtro;
        } else if (filtro <= MV_FINE_CORSA) {
            corsa_concludi();
        }
    } else if (!in_carica && st != BATT_CARICA && s_visto_pieno &&
               s_picco_mv >= MV_PIENA_MIN && filtro + 30 < s_picco_mv) {
        /* L'inizio di una scarica lo riconosco dalla tensione che scende da un
           valore pieno, non dallo stato del coprocessore: con la batteria
           staccata quello dice "piena" (2) esattamente come quando la batteria
           c'e' ed e' piena, quindi su di lui non si puo' contare per sapere
           se una scarica e' cominciata. La tensione che cala di trenta
           millivolt dal picco invece lo dice senza ambiguita'. */
        corsa_inizia(filtro);
    }

    // ---- registro
    uint32_t ora_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (st != stato_prec || ora_s - ultimo_log_s >= LOG_PERIODO_S ||
        (s_corsa && ora_s - ultimo_log_s >= s_camp_passo_s)) {
        if (st != stato_prec)
            ESP_LOGI(TAG, "alimentazione: %s (%u mV, %u%%)",
                     batteria_stato_parole(st), (unsigned)filtro, copia.pct);
        if (s_corsa && ora_s - ultimo_log_s >= s_camp_passo_s) corsa_campiona(mv_vero);
        log_campione(&copia, schermo);
        ultimo_log_s = ora_s;
    }
    stato_prec = st;
}

static void batteria_task(void *arg)
{
    log_riga("# avvio\n");
    while (true) {
        campiona();
        vTaskDelay(pdMS_TO_TICKS(PERIODO_MS));
    }
}

void batteria_avvia(void)
{
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    memset(&s_info, 0, sizeof(s_info));
    curva_carica_da_nvs();
    /* Stack generoso: qui dentro si scrive sulla SD, e il driver FAT non e'
       parco. Il task gira ogni due secondi e non fa altro. */
    xTaskCreate(batteria_task, "batteria", 5120, nullptr, 3, nullptr);
}

void batteria_leggi(batt_info_t *out)
{
    if (!out) return;
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_mtx);
}

/* Accoda a out senza mai uscire dal buffer. La forma diretta
   "n += snprintf(out + n, n < sz ? sz - n : 0, ...)" sembra sicura - con
   dimensione zero snprintf non scrive - ma out + n viene calcolato comunque, e
   un puntatore oltre la fine del buffer e' comportamento indefinito per conto
   suo. Qui out + n si calcola solo quando n e' ancora dentro. */
#define AGG(...) do { if (n < sz) n += snprintf(out + n, sz - n, __VA_ARGS__); } while (0)

void batteria_diagnostica(char *out, size_t sz)
{
    batt_info_t i;
    batteria_leggi(&i);
    size_t n = 0;

    AGG("BATTERIA %s  %u mV (filtrata %u)  stima %u%%  coprocessore %u%%\n",
        batteria_stato_parole(i.stato), (unsigned)i.mv,
        (unsigned)i.mv_filtrato, i.pct, i.pct_stc8);
    AGG("BATTERIA a riposo %u mV  (gonfiamento in carica %u mV, "
        "schermo acceso %u mV)\n",
        (unsigned)i.mv_riposo, (unsigned)i.offset_carica_mv,
        (unsigned)i.caduta_schermo_mv);
    AGG("BATTERIA grezzi: stato=%u led=%u\n", i.stato_grezzo, i.led_grezzo);

    if (i.curva_imparata) {
        char quando[24] = "data sconosciuta";
        if (i.curva_quando) {
            time_t t = (time_t)i.curva_quando;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(quando, sizeof(quando), "%Y-%m-%d %H:%M", &tm);
        }
        AGG("BATTERIA tarata il %s su una scarica di %u s\n",
            quando, (unsigned)i.curva_durata_s);
    } else {
        AGG("BATTERIA non tarata: uso la tabella standard. Serve una scarica "
            "completa da piena a spenta, senza ricariche in mezzo.\n");
    }

    if (i.ma_spento || i.ma_acceso)
        AGG("BATTERIA consumi misurati: %u mA spento, %u mA acceso (rapporto %.2f)\n",
            i.ma_spento, i.ma_acceso, peso_acceso());
    else
        AGG("BATTERIA consumi non misurati: rapporto stimato %.2f, niente mAh ne' "
            "autonomia. Per darli: batteria ma <spento> <acceso>%s\n",
            peso_acceso(),
            s_carica_ultima_s ? " (la scarica c'e' gia': i mAh li ricavo subito)" : "");
    if (i.capacita_mah)
        AGG("BATTERIA capacita' misurata %u mAh, autonomia %u min\n",
            i.capacita_mah, i.autonomia_min);

    AGG("BATTERIA curva:");
    for (int k = 0; k < BATT_PUNTI; k++)
        AGG(" %d%%=%u", k * 5, (unsigned)s_curva[k]);
    AGG("\n");

    if (i.corsa_attiva)
        AGG("BATTERIA scarica in registrazione: %u punti, da %u mV a %u mV\n",
            (unsigned)s_n_camp, (unsigned)s_corsa_mv0, (unsigned)i.mv_filtrato);

    long dim = 0;
    FILE *f = fopen(LOG_PATH, "r");
    if (f) { fseek(f, 0, SEEK_END); dim = ftell(f); fclose(f); }
    AGG("BATTERIA registro %s: %ld byte\n", LOG_PATH, dim);
}

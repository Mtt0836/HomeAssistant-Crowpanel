#include "ram_monitor.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "hosted_recovery.h"
#include "ha_ws.h"

static const char *TAG = "ram_mon";

#define LOG_PATH   "/spiffs/ramlog.csv"
#define LOG_MAX    (16 * 1024)       // oltre, si tiene solo la meta' piu' recente
#define FIRST_S    (5 * 60)
#define PERIOD_S   (60 * 60)
#define HEADER     "data_ora,uptime_s,int_libera,int_minima,int_blocco_max,psram_libera,psram_minima,recuperi_sdio,ha\n"

static SemaphoreHandle_t s_mtx = nullptr;

/* Il file cresce di ~100 byte l'ora: quando supera LOG_MAX ne butto la meta'
   piu' vecchia, cosi' restano sempre circa una settimana di campioni. */
static void trim_if_needed(void)
{
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= LOG_MAX) { fclose(f); return; }
    // in PSRAM: la RAM interna e' proprio quella che stiamo sorvegliando
    char *buf = (char *)heap_caps_malloc(LOG_MAX / 2, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return; }
    fseek(f, n - LOG_MAX / 2, SEEK_SET);
    size_t got = fread(buf, 1, LOG_MAX / 2, f);
    fclose(f);
    const char *start = (const char *)memchr(buf, '\n', got);   // riparto da una riga intera
    if (start) {
        start++;
        f = fopen(LOG_PATH, "w");
        if (f) {
            fputs(HEADER, f);
            fwrite(start, 1, got - (start - buf), f);
            fclose(f);
        }
    }
    heap_caps_free(buf);
}

static void write_line(const char *line)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        if (ftell(f) == 0) fputs(HEADER, f);
        fputs(line, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "impossibile scrivere %s", LOG_PATH);
    }
    trim_if_needed();
    xSemaphoreGive(s_mtx);
}

void ram_monitor_sample_now(void)
{
    char when[24] = "-";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);

    const unsigned up  = (unsigned)(esp_timer_get_time() / 1000000);
    const size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t i_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t p_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    char line[160];
    snprintf(line, sizeof(line), "%s,%u,%u,%u,%u,%u,%u,%u,%s\n", when, up,
             (unsigned)i_free, (unsigned)i_min, (unsigned)i_big,
             (unsigned)p_free, (unsigned)p_min, hosted_recovery_count(),
             ha_ws_connected() ? "si" : "no");
    ESP_LOGI(TAG, "RAM interna libera %u (minima %u, blocco max %u), PSRAM %u",
             (unsigned)i_free, (unsigned)i_min, (unsigned)i_big, (unsigned)p_free);
    write_line(line);
}

static void monitor_task(void *arg)
{
    write_line("# avvio\n");     // separa le sessioni nel file
    vTaskDelay(pdMS_TO_TICKS(FIRST_S * 1000));
    while (true) {
        ram_monitor_sample_now();
        vTaskDelay(pdMS_TO_TICKS((uint32_t)PERIOD_S * 1000));
    }
}

size_t ram_monitor_read(char *buf, size_t sz)
{
    if (sz == 0) return 0;
    buf[0] = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = fopen(LOG_PATH, "r");
    size_t got = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        long from = n > (long)(sz - 1) ? n - (long)(sz - 1) : 0;
        fseek(f, from, SEEK_SET);
        got = fread(buf, 1, sz - 1, f);
        buf[got] = 0;
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    return got;
}

void ram_monitor_start(void)
{
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    xTaskCreate(monitor_task, "ram_mon", 4096, nullptr, 2, nullptr);
}

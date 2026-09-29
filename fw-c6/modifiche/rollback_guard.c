#include "rollback_guard.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs.h"

static const char *TAG = "rb_guard";

/* Conferma: canale dati aperto da almeno 2 minuti di fila, con traffico vero. */
#define RB_CONFIRM_MS        (120 * 1000)
#define RB_CONFIRM_MIN_PKTS  10
/* Un avvio che non si conferma entro 5 minuti conta come fallito. */
#define RB_BOOT_TIMEOUT_MS   (300 * 1000)
/* Dopo 6 avvii falliti di fila si torna all'altro slot. */
#define RB_MAX_BOOTS         6

#define NS          "rbguard"
#define KEY_OK      "ok"       /* SHA dell'immagine confermata */
#define KEY_TRIAL   "trial"    /* SHA dell'immagine in prova   */
#define KEY_BOOTS   "boots"    /* avvii in prova senza conferma */

static volatile uint32_t s_open_ms  = 0;   /* 0 = canale chiuso */
static volatile uint32_t s_pkts     = 0;   /* pacchetti dal P4 dall'apertura */
static bool s_armed = false;               /* true solo per un'immagine in prova */

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void rollback_guard_datapath(bool open)
{
	s_pkts = 0;
	s_open_ms = open ? (now_ms() | 1) : 0;   /* |1: mai zero se aperto */
}

void rollback_guard_host_pkt(void)
{
	s_pkts++;
}

static bool nvs_get_sha(nvs_handle_t h, const char *key, uint8_t out[32])
{
	size_t len = 32;
	return nvs_get_blob(h, key, out, &len) == ESP_OK && len == 32;
}

static void confirm(const uint8_t sha[32])
{
	nvs_handle_t h;
	if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
		ESP_LOGE(TAG, "NVS non apribile: conferma non salvata");
		return;
	}
	nvs_set_blob(h, KEY_OK, sha, 32);
	nvs_erase_key(h, KEY_TRIAL);
	nvs_erase_key(h, KEY_BOOTS);
	nvs_commit(h);
	nvs_close(h);
	ESP_LOGW(TAG, "immagine confermata: il ripristino automatico resta spento");
}

static void guard_task(void *arg)
{
	const uint8_t *sha = (const uint8_t *)arg;
	while (true) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		uint32_t open = s_open_ms;
		uint32_t t = now_ms();
		if (open && (t - open) >= RB_CONFIRM_MS && s_pkts >= RB_CONFIRM_MIN_PKTS) {
			confirm(sha);
			s_armed = false;
			vTaskDelete(NULL);
		}
		if (t >= RB_BOOT_TIMEOUT_MS) {
			ESP_LOGW(TAG, "nessun collegamento stabile col P4 in %d s: riavvio (avvio fallito)",
					RB_BOOT_TIMEOUT_MS / 1000);
			vTaskDelay(pdMS_TO_TICKS(100));
			esp_restart();
		}
	}
}

void rollback_guard_init(void)
{
	static uint8_t sha[32];
	const esp_app_desc_t *d = esp_app_get_description();
	memcpy(sha, d->app_elf_sha256, sizeof(sha));

	nvs_handle_t h;
	if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
		ESP_LOGE(TAG, "NVS non apribile: protezione disattivata");
		return;
	}

	uint8_t ok[32], trial[32];
	if (nvs_get_sha(h, KEY_OK, ok) && memcmp(ok, sha, 32) == 0) {
		nvs_close(h);
		ESP_LOGI(TAG, "immagine confermata (build %d)", RB_FW_BUILD);
		return;
	}

	uint8_t boots = 0;
	if (nvs_get_sha(h, KEY_TRIAL, trial) && memcmp(trial, sha, 32) == 0) {
		nvs_get_u8(h, KEY_BOOTS, &boots);
	} else {
		nvs_set_blob(h, KEY_TRIAL, sha, 32);   /* nuova immagine: si riparte da zero */
	}
	boots++;
	nvs_set_u8(h, KEY_BOOTS, boots);
	nvs_commit(h);

	const esp_partition_t *running = esp_ota_get_running_partition();
	ESP_LOGW(TAG, "immagine IN PROVA su %s: avvio %u di %d senza conferma",
			running ? running->label : "?", boots, RB_MAX_BOOTS);

	if (boots > RB_MAX_BOOTS) {
		const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
		esp_app_desc_t od;
		if (other && esp_ota_get_partition_description(other, &od) == ESP_OK &&
		    esp_ota_set_boot_partition(other) == ESP_OK) {   /* verifica anche l'immagine */
			nvs_erase_key(h, KEY_TRIAL);
			nvs_erase_key(h, KEY_BOOTS);
			nvs_commit(h);
			nvs_close(h);
			ESP_LOGE(TAG, "RIPRISTINO: torno a %s (versione %s)", other->label, od.version);
			vTaskDelay(pdMS_TO_TICKS(100));
			esp_restart();
		}
		/* Nessuna immagine valida nell'altro slot: meglio restare dove siamo. */
		ESP_LOGE(TAG, "ripristino impossibile: l'altro slot non contiene un'immagine valida");
		nvs_set_u8(h, KEY_BOOTS, 0);
		nvs_commit(h);
		nvs_close(h);
		return;
	}
	nvs_close(h);

	s_armed = true;
	xTaskCreate(guard_task, "rb_guard", 3072, sha, 2, NULL);
}

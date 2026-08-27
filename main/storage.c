#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";
static const char *NAMESPACE = "tracker";

static nvs_handle_t s_handle;
static bool s_ready = false;

// Maps a reset reason to its NVS counter key and its short display label.
// Everything that isn't a clean power-on, software restart, or brownout
// gets bucketed as PANIC/OTHER rather than growing this switch forever --
// the point is "how many times has this thing crashed", not a full taxonomy.
static const char *reset_reason_key(esp_reset_reason_t reason, const char **out_label)
{
    switch (reason) {
        case ESP_RST_BROWNOUT:
            *out_label = "BOR";
            return "rst_bor";
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_CPU_LOCKUP:
            *out_label = "PANIC";
            return "rst_panic";
        case ESP_RST_SW:
            *out_label = "SW";
            return "rst_sw";
        default:
            *out_label = "OTHER";
            return "rst_other";
    }
}

static void record_reset_reason(void)
{
    const char *label = "OTHER";
    const char *key = reset_reason_key(esp_reset_reason(), &label);

    uint32_t count = 0;
    nvs_get_u32(s_handle, key, &count); // leaves count at 0 if key not found yet
    count++;
    nvs_set_u32(s_handle, key, count);
    nvs_commit(s_handle);

    ESP_LOGI(TAG, "Boot reset reason: %s (count now %lu)", label, (unsigned long)count);
}

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (err=0x%x), reinitializing", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x, persistence disabled this boot", err);
        s_ready = false;
        return;
    }

    err = nvs_open(NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: 0x%x, persistence disabled this boot", err);
        s_ready = false;
        return;
    }

    s_ready = true;
    record_reset_reason();
}

void storage_save_last_fix(float lat, float lon)
{
    if (!s_ready) return;
    nvs_set_blob(s_handle, "fix_lat", &lat, sizeof(lat));
    nvs_set_blob(s_handle, "fix_lon", &lon, sizeof(lon));
    nvs_commit(s_handle);
}

bool storage_load_last_fix(float *lat, float *lon)
{
    if (!s_ready) return false;
    size_t sz = sizeof(*lat);
    if (nvs_get_blob(s_handle, "fix_lat", lat, &sz) != ESP_OK) return false;
    sz = sizeof(*lon);
    if (nvs_get_blob(s_handle, "fix_lon", lon, &sz) != ESP_OK) return false;
    return true;
}

void storage_format_reset_summary(char *buf, size_t buf_size)
{
    uint32_t bor = 0, panic = 0, sw = 0, other = 0;
    if (s_ready) {
        nvs_get_u32(s_handle, "rst_bor", &bor);
        nvs_get_u32(s_handle, "rst_panic", &panic);
        nvs_get_u32(s_handle, "rst_sw", &sw);
        nvs_get_u32(s_handle, "rst_other", &other);
    }
    snprintf(buf, buf_size, "BOR:%lu PANIC:%lu SW:%lu OTHER:%lu",
             (unsigned long)bor, (unsigned long)panic, (unsigned long)sw, (unsigned long)other);
}

#include "ota.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "ota";

//--------------------------------------------------------------------+
// Manifest parsing
//--------------------------------------------------------------------+

// Download and parse JSON manifest. Fills version fields for "p4" or "c5" key.
static esp_err_t fetch_manifest_version(const char *manifest_url,
                                         const char *target_key,
                                         ota_version_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_http_client_config_t cfg = {
        .url         = manifest_url,
        .timeout_ms  = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || content_len > 8192) {
        ESP_LOGE(TAG, "Manifest too large or empty (%lld bytes)", content_len);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *json_buf = malloc((size_t)content_len + 1);
    if (!json_buf) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read = esp_http_client_read(client, json_buf, (int)content_len);
    json_buf[read > 0 ? read : 0] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Parse JSON
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) {
        ESP_LOGE(TAG, "Manifest JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *target = cJSON_GetObjectItem(root, target_key);
    if (!target) {
        ESP_LOGE(TAG, "Manifest has no '%s' key", target_key);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *ver = cJSON_GetObjectItem(target, "version");
    cJSON *url = cJSON_GetObjectItem(target, "url");
    cJSON *sha = cJSON_GetObjectItem(target, "sha256");
    cJSON *chg = cJSON_GetObjectItem(target, "changelog");

    if (ver && cJSON_IsString(ver))
        strncpy(out->version, ver->valuestring, sizeof(out->version) - 1);
    if (url && cJSON_IsString(url))
        strncpy(out->url, url->valuestring, sizeof(out->url) - 1);
    if (sha && cJSON_IsString(sha))
        strncpy(out->sha256, sha->valuestring, sizeof(out->sha256) - 1);
    if (chg && cJSON_IsString(chg))
        strncpy(out->changelog, chg->valuestring, sizeof(out->changelog) - 1);

    cJSON_Delete(root);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// P4 OTA
//--------------------------------------------------------------------+

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware marked valid, rollback cancelled");
        }
    }
}

esp_err_t ota_get_running_version(char *buf, size_t len)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) { buf[0] = '\0'; return ESP_FAIL; }
    strncpy(buf, desc->version, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t ota_check_update(const char *manifest_url, ota_version_t *out_available)
{
    esp_err_t err = fetch_manifest_version(manifest_url, "p4", out_available);
    if (err != ESP_OK) return err;

    // Compare with running version
    char running_ver[32] = {0};
    ota_get_running_version(running_ver, sizeof(running_ver));

    if (strcmp(running_ver, out_available->version) == 0) {
        ESP_LOGI(TAG, "Already running latest P4 firmware (%s)", running_ver);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Update available: %s -> %s", running_ver, out_available->version);
    return ESP_OK;
}

typedef struct {
    ota_progress_cb_t cb;
    int64_t content_len;
    int64_t downloaded;
} ota_progress_ctx_t;

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    ota_progress_ctx_t *ctx = (ota_progress_ctx_t *)evt->user_data;
    if (!ctx || !ctx->cb) return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Content-Length header
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                ctx->content_len = atoll(evt->header_value);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            ctx->downloaded += evt->data_len;
            if (ctx->content_len > 0) {
                uint8_t pct = (uint8_t)((ctx->downloaded * 90) / ctx->content_len);
                ctx->cb(pct);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t ota_start_update(const char *firmware_url, ota_progress_cb_t progress_cb)
{
    ESP_LOGI(TAG, "Starting P4 OTA from: %s", firmware_url);

    if (progress_cb) progress_cb(0);

    ota_progress_ctx_t ctx = {.cb = progress_cb};

    esp_http_client_config_t http_cfg = {
        .url            = firmware_url,
        .timeout_ms     = 60000,
        .buffer_size    = 16384,
        .event_handler  = ota_http_event_handler,
        .user_data      = &ctx,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    if (progress_cb) progress_cb(100);

    ESP_LOGI(TAG, "OTA successful — rebooting in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;  // Never reached
}

esp_err_t ota_rollback(void)
{
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
    }
    return err;
}

//--------------------------------------------------------------------+
// C5 companion OTA
//--------------------------------------------------------------------+

esp_err_t ota_c5_check_update(const char *manifest_url, ota_version_t *out_available)
{
    return fetch_manifest_version(manifest_url, "c5", out_available);
}

esp_err_t ota_c5_start_update(const char *firmware_url, ota_progress_cb_t progress_cb)
{
    // TODO: Download firmware to PSRAM, transmit via esp_hosted SDIO OTA API
    // esp_hosted_ota_update(buf, len, progress_cb) — API TBD when implementing F8-C fully
    ESP_LOGW(TAG, "C5 OTA: download+transmit via SDIO not yet implemented");
    if (progress_cb) progress_cb(0);
    (void)firmware_url;
    return ESP_ERR_NOT_SUPPORTED;
}

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

// Manifest URL (configurable via NVS in the future; hardcoded for now)
#define DEFAULT_MANIFEST_URL "https://updates.lyra-player.io/manifest.json"

void ota_handle_cdc_command(const char *subcommand, ota_print_fn_t print_fn)
{
    while (*subcommand == ' ') subcommand++;

    if (strcmp(subcommand, "version") == 0) {
        char ver[32] = {0};
        ota_get_running_version(ver, sizeof(ver));
        print_fn("P4 firmware: %s\r\n", ver[0] ? ver : "(unknown)");
        print_fn("C5 firmware: (query via esp_hosted — TODO)\r\n");

    } else if (strcmp(subcommand, "check") == 0) {
        print_fn("Checking for updates at %s ...\r\n", DEFAULT_MANIFEST_URL);
        ota_version_t avail = {0};
        esp_err_t err = ota_check_update(DEFAULT_MANIFEST_URL, &avail);
        if (err == ESP_OK) {
            print_fn("Update available: %s\r\n", avail.version);
            if (avail.changelog[0]) print_fn("  %s\r\n", avail.changelog);
            print_fn("Run 'ota update' to apply\r\n");
        } else if (err == ESP_ERR_NOT_FOUND) {
            char ver[32] = {0};
            ota_get_running_version(ver, sizeof(ver));
            print_fn("Already up to date (%s)\r\n", ver);
        } else {
            print_fn("Check failed: %s\r\n", esp_err_to_name(err));
        }

    } else if (strcmp(subcommand, "update") == 0) {
        print_fn("Fetching manifest...\r\n");
        ota_version_t avail = {0};
        esp_err_t err = ota_check_update(DEFAULT_MANIFEST_URL, &avail);
        if (err == ESP_ERR_NOT_FOUND) {
            print_fn("Already up to date\r\n");
            return;
        }
        if (err != ESP_OK) {
            print_fn("Manifest fetch failed: %s\r\n", esp_err_to_name(err));
            return;
        }
        print_fn("Downloading P4 firmware %s...\r\n", avail.version);

        ota_progress_cb_t pcb = NULL;  // TODO: wire to print_fn progress
        err = ota_start_update(avail.url, pcb);
        if (err != ESP_OK) {
            print_fn("OTA failed: %s\r\n", esp_err_to_name(err));
        }
        // On success, device reboots — this line is never reached

    } else if (strcmp(subcommand, "c5 update") == 0 ||
               strcmp(subcommand, "c5update") == 0) {
        print_fn("C5 OTA not yet implemented\r\n");

    } else if (strcmp(subcommand, "rollback") == 0) {
        print_fn("Scheduling rollback and rebooting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_err_t err = ota_rollback();
        if (err != ESP_OK) {
            print_fn("Rollback failed: %s\r\n", esp_err_to_name(err));
        }

    } else {
        print_fn("OTA commands:\r\n");
        print_fn("  ota version    - show running firmware versions\r\n");
        print_fn("  ota check      - check for available updates\r\n");
        print_fn("  ota update     - download and apply P4 update\r\n");
        print_fn("  ota c5 update  - update C5 companion (TODO)\r\n");
        print_fn("  ota rollback   - revert to previous firmware\r\n");
    }
}

#include "settings_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "settings";
static bool s_initialized = false;

//--------------------------------------------------------------------+
// Init
//--------------------------------------------------------------------+

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init_partition(SETTINGS_NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' corrupt, erasing...",
                 SETTINGS_NVS_PARTITION);
        nvs_flash_erase_partition(SETTINGS_NVS_PARTITION);
        err = nvs_flash_init_partition(SETTINGS_NVS_PARTITION);
    }
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "Settings store initialized (partition: %s)",
                 SETTINGS_NVS_PARTITION);
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
    return err;
}

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+

static esp_err_t save_blob(const char *ns, const char *key,
                            const void *data, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(SETTINGS_NVS_PARTITION, ns,
                                             NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_blob(const char *ns, const char *key,
                            void *data, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(SETTINGS_NVS_PARTITION, ns,
                                             NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t stored_len = len;
    err = nvs_get_blob(h, key, data, &stored_len);
    nvs_close(h);
    return err;
}

static esp_err_t erase_ns(const char *ns)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(SETTINGS_NVS_PARTITION, ns,
                                             NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

//--------------------------------------------------------------------+
// Audio
//--------------------------------------------------------------------+

esp_err_t settings_save_audio(const settings_audio_t *cfg)
{
    esp_err_t err = save_blob("audio", "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Audio saved: preset=%d limiter=%d enabled=%d vol=%d",
                 cfg->preset, cfg->limiter_mode, cfg->dsp_enabled, cfg->volume);
    }
    return err;
}

esp_err_t settings_load_audio(settings_audio_t *cfg)
{
    esp_err_t err = load_blob("audio", "cfg", cfg, sizeof(*cfg));
    if (err != ESP_OK) {
        // Defaults
        memset(cfg, 0, sizeof(*cfg));
        cfg->preset = 0;       // PRESET_FLAT
        cfg->dsp_enabled = 1;
        cfg->volume = 80;
    }
    return err;
}

//--------------------------------------------------------------------+
// WiFi
//--------------------------------------------------------------------+

esp_err_t settings_save_wifi(const settings_wifi_t *cfg)
{
    esp_err_t err = save_blob("wifi", "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi saved: ssid=%s auto=%d", cfg->ssid, cfg->auto_connect);
    }
    return err;
}

esp_err_t settings_load_wifi(settings_wifi_t *cfg)
{
    esp_err_t err = load_blob("wifi", "cfg", cfg, sizeof(*cfg));
    if (err != ESP_OK) {
        memset(cfg, 0, sizeof(*cfg));
    }
    return err;
}

//--------------------------------------------------------------------+
// Subsonic profiles
//--------------------------------------------------------------------+

static void sub_ns(uint8_t index, char *ns, size_t len)
{
    snprintf(ns, len, "sub_%d", index);
}

esp_err_t settings_save_subsonic_profile(uint8_t index,
                                          const settings_subsonic_profile_t *profile)
{
    if (index >= SETTINGS_MAX_SUBSONIC_PROFILES) return ESP_ERR_INVALID_ARG;

    char ns[8];
    sub_ns(index, ns, sizeof(ns));

    esp_err_t err = save_blob(ns, "prof", profile, sizeof(*profile));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Subsonic[%d] saved: %s@%s active=%d",
                 index, profile->username, profile->server_url, profile->active);
    }
    return err;
}

esp_err_t settings_load_subsonic_profile(uint8_t index,
                                          settings_subsonic_profile_t *profile)
{
    if (index >= SETTINGS_MAX_SUBSONIC_PROFILES) return ESP_ERR_INVALID_ARG;

    char ns[8];
    sub_ns(index, ns, sizeof(ns));

    esp_err_t err = load_blob(ns, "prof", profile, sizeof(*profile));
    if (err != ESP_OK) {
        memset(profile, 0, sizeof(*profile));
    }
    return err;
}

esp_err_t settings_delete_subsonic_profile(uint8_t index)
{
    if (index >= SETTINGS_MAX_SUBSONIC_PROFILES) return ESP_ERR_INVALID_ARG;

    char ns[8];
    sub_ns(index, ns, sizeof(ns));

    esp_err_t err = erase_ns(ns);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Subsonic[%d] deleted", index);
    }
    return err;
}

int settings_get_active_subsonic_profile(void)
{
    settings_subsonic_profile_t prof;
    for (int i = 0; i < SETTINGS_MAX_SUBSONIC_PROFILES; i++) {
        if (settings_load_subsonic_profile(i, &prof) == ESP_OK && prof.active) {
            return i;
        }
    }
    return -1;
}

//--------------------------------------------------------------------+
// User EQ presets
//--------------------------------------------------------------------+

static void ueq_ns(uint8_t index, char *ns, size_t len)
{
    snprintf(ns, len, "ueq_%d", index);
}

esp_err_t settings_save_user_eq(uint8_t index, const settings_user_eq_t *eq)
{
    if (index >= SETTINGS_MAX_USER_EQ_SLOTS) return ESP_ERR_INVALID_ARG;

    char ns[8];
    ueq_ns(index, ns, sizeof(ns));

    esp_err_t err = save_blob(ns, "eq", eq, sizeof(*eq));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UserEQ[%d] saved: '%s' (%d bands)",
                 index, eq->name, eq->num_bands);
    }
    return err;
}

esp_err_t settings_load_user_eq(uint8_t index, settings_user_eq_t *eq)
{
    if (index >= SETTINGS_MAX_USER_EQ_SLOTS) return ESP_ERR_INVALID_ARG;

    char ns[8];
    ueq_ns(index, ns, sizeof(ns));

    esp_err_t err = load_blob(ns, "eq", eq, sizeof(*eq));
    if (err != ESP_OK) {
        memset(eq, 0, sizeof(*eq));
    }
    return err;
}

//--------------------------------------------------------------------+
// Last.fm
//--------------------------------------------------------------------+

esp_err_t settings_save_lastfm(const settings_lastfm_t *cfg)
{
    esp_err_t err = save_blob("lastfm", "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Last.fm saved: user=%s has_session=%d",
                 cfg->username, cfg->session_key[0] ? 1 : 0);
    }
    return err;
}

esp_err_t settings_load_lastfm(settings_lastfm_t *cfg)
{
    esp_err_t err = load_blob("lastfm", "cfg", cfg, sizeof(*cfg));
    if (err != ESP_OK) {
        memset(cfg, 0, sizeof(*cfg));
    }
    return err;
}

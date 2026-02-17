/**
 * @file    wireless.c
 * @brief   WiFi connectivity via companion chip (esp_hosted over SDIO)
 *
 * Architecture:
 *   P4 (host) ──SDIO Slot 1──> companion (slave, esp_hosted firmware)
 *   esp_wifi_remote wraps standard esp_wifi_* APIs transparently.
 *   SDIO pin mapping & reset GPIO configured via Kconfig (menuconfig).
 *
 * Companion chips:
 *   Dev board:  ESP32-C6-MINI-1
 *   Final Lyra: ESP32-C5-WROOM-1U-N8R8
 *
 * GPIOs:
 *   SDIO: DAT0=14, DAT1=15, DAT2=16, DAT3=17, CLK=18, CMD=19
 *   Reset: GPIO 54 → companion EN  (configured in esp_hosted Kconfig)
 *   Boot:  GPIO 53 → companion GPIO9  (final board only, normal = HIGH)
 *
 * Phase: F7 (Wireless)
 */

#include "wireless.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"

#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wireless";

/* ── GPIO ──────────────────────────────────────────────────────── */

/* Reset GPIO always available (dev + final board) */
#define C5_RESET_GPIO   GPIO_NUM_54

/* Boot mode GPIO only on final board (on dev board GPIO 53 = AMP_EN) */
#if LYRA_FINAL_BOARD
#define C5_BOOT_GPIO    GPIO_NUM_53
#endif

/* ── Internal state ────────────────────────────────────────────── */

static wireless_state_t  s_state       = WIRELESS_STATE_OFF;
static esp_netif_t      *s_sta_netif   = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;
static char              s_ip_str[16]  = {0};

/* ── Event handlers ────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGW(TAG, ">>> STA_CONNECTED event received <<<");
            /* Check netif status to verify DHCP will start */
            if (s_sta_netif) {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(s_sta_netif, &ip_info);
                ESP_LOGI(TAG, "netif flags: UP=%d, link_UP=%d",
                         esp_netif_is_netif_up(s_sta_netif),
                         esp_netif_is_netif_up(s_sta_netif));
                ESP_LOGI(TAG, "netif IP (pre-DHCP): " IPSTR, IP2STR(&ip_info.ip));
                esp_netif_dhcp_status_t dhcp_status;
                esp_err_t dret = esp_netif_dhcpc_get_status(s_sta_netif, &dhcp_status);
                ESP_LOGI(TAG, "DHCP client status: %d (0=INIT,1=STARTED,2=STOPPED), ret=%s",
                         dhcp_status, esp_err_to_name(dret));
            } else {
                ESP_LOGE(TAG, "s_sta_netif is NULL!");
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "WiFi disconnected (reason: %d)", disc ? disc->reason : -1);
            s_ip_str[0] = '\0';
            wireless_state_t prev = s_state;
            s_state = WIRELESS_STATE_DISCONNECTED;
            /* Unblock connect() if waiting */
            if (prev == WIRELESS_STATE_CONNECTING && s_connect_sem) {
                xSemaphoreGive(s_connect_sem);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_state = WIRELESS_STATE_CONNECTED;
        if (s_connect_sem) {
            xSemaphoreGive(s_connect_sem);
        }
    }
}

/* ── Init ──────────────────────────────────────────────────────── */

esp_err_t wireless_init(void)
{
    if (s_state != WIRELESS_STATE_OFF) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

#if LYRA_FINAL_BOARD
    /* Boot mode pin: HIGH = normal run (must be set BEFORE esp_hosted init) */
    gpio_config_t boot_io = {
        .pin_bit_mask  = (1ULL << C5_BOOT_GPIO),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&boot_io);
    gpio_set_level(C5_BOOT_GPIO, 1);
    ESP_LOGI(TAG, "C5 boot GPIO %d = HIGH (normal boot)", C5_BOOT_GPIO);
#endif

    /*
     * Following the pattern from esp_hosted's host_sdcard_with_hosted example:
     *   1. esp_netif_init() + event loop
     *   2. esp_wifi_init() — this triggers esp_hosted_init() internally
     *      (esp_hosted handles C5 reset via Kconfig GPIO 54, SDIO Slot 1 setup)
     *   3. Register event handlers
     *   4. esp_wifi_start()
     */

    /* TCP/IP stack + event loop */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
        s_state = WIRELESS_STATE_ERROR;
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));
        s_state = WIRELESS_STATE_ERROR;
        return ret;
    }

    /* WiFi STA netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create WiFi STA netif");
        s_state = WIRELESS_STATE_ERROR;
        return ESP_FAIL;
    }

    /*
     * esp_wifi_init() triggers esp_hosted transport initialization:
     *   - Resets C5 via GPIO 54 (Kconfig: ESP_HOSTED_GPIO_RESET)
     *   - Initializes SDIO Slot 1 with GPIOs 14-19 (Kconfig)
     *   - Establishes RPC channel with C5 slave firmware
     * All WiFi calls are forwarded to C5 via esp_wifi_remote.
     */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (is C5 powered & flashed?)",
                 esp_err_to_name(ret));
        s_state = WIRELESS_STATE_ERROR;
        return ret;
    }

    /* Log co-processor firmware info */
    esp_hosted_coprocessor_fwver_t fwver;
    if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
        ESP_LOGI(TAG, "C5 firmware: %lu.%lu.%lu",
                 (unsigned long)fwver.major1,
                 (unsigned long)fwver.minor1,
                 (unsigned long)fwver.patch1);
    } else {
        ESP_LOGW(TAG, "C5 firmware version unavailable (RPC timeout - slave may need update)");
    }

    esp_hosted_app_desc_t app_desc;
    if (esp_hosted_get_coprocessor_app_desc(&app_desc) == ESP_OK) {
        ESP_LOGI(TAG, "C5 project: %.32s, ver: %.32s, IDF: %.32s",
                 app_desc.project_name, app_desc.version, app_desc.idf_ver);
        ESP_LOGI(TAG, "C5 built: %.16s %.16s", app_desc.date, app_desc.time);
    } else {
        ESP_LOGW(TAG, "C5 app descriptor unavailable");
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /*
     * Prevent C5 auto-connect on esp_wifi_start().
     * The slave stores last-used credentials in NVS and auto-connects,
     * but the host hasn't registered netif/events yet at that point,
     * so the CONNECTED event is lost and DHCP never starts.
     * Clear the config so the slave starts clean (no auto-connect).
     */
    wifi_config_t empty_cfg = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);

    ESP_ERROR_CHECK(esp_wifi_start());

    s_connect_sem = xSemaphoreCreateBinary();
    s_state = WIRELESS_STATE_DISCONNECTED;

    ESP_LOGI(TAG, "Wireless ready (C5 via SDIO, WiFi STA mode)");
    return ESP_OK;
}

/* ── WiFi Scan ─────────────────────────────────────────────────── */

static const char *auth_mode_str(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    default:                        return "???";
    }
}

#define SCAN_MAX_AP 20

esp_err_t wireless_wifi_scan(wireless_print_fn_t print_fn)
{
    if (s_state == WIRELESS_STATE_OFF || s_state == WIRELESS_STATE_ERROR) {
        print_fn("Error: wireless not initialized\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    print_fn("Scanning...\r\n");

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (ret != ESP_OK) {
        print_fn("Scan failed: %s\r\n", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        print_fn("No networks found\r\n");
        esp_wifi_scan_get_ap_records(NULL, NULL); /* free scan memory */
        return ESP_OK;
    }

    uint16_t fetch = (ap_count > SCAN_MAX_AP) ? SCAN_MAX_AP : ap_count;
    wifi_ap_record_t *ap_list = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        print_fn("Out of memory\r\n");
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&fetch, ap_list);

    print_fn("Found %d networks", ap_count);
    if (ap_count > SCAN_MAX_AP) print_fn(" (showing %d)", SCAN_MAX_AP);
    print_fn(":\r\n");
    print_fn("  #  SSID                             RSSI  CH  Auth\r\n");
    print_fn("  -- -------------------------------- ----- --- ------\r\n");

    for (int i = 0; i < fetch; i++) {
        char ssid_buf[33] = {0};
        memcpy(ssid_buf, ap_list[i].ssid, 32);
        if (ssid_buf[0] == '\0') {
            strcpy(ssid_buf, "(hidden)");
        }
        print_fn("  %2d %-32s %4d  %2d  %s\r\n",
                 i + 1, ssid_buf, ap_list[i].rssi,
                 ap_list[i].primary, auth_mode_str(ap_list[i].authmode));
    }

    print_fn("\r\nUse: wifi connect <ssid> <password>\r\n");

    free(ap_list);
    return ESP_OK;
}

/* ── WiFi STA Connect ─────────────────────────────────────────── */

esp_err_t wireless_wifi_connect(const char *ssid, const char *password,
                                wireless_print_fn_t print_fn)
{
    if (s_state != WIRELESS_STATE_DISCONNECTED &&
        s_state != WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("Error: invalid state (%d)\r\n", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 1: Disconnect if needed */
    if (s_state == WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("[1/4] Disconnecting from current AP...\r\n");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    } else {
        if (print_fn) print_fn("[1/4] Preparing connection...\r\n");
    }

    /* Step 2: Configure */
    if (print_fn) print_fn("[2/4] Setting WiFi config (SSID: %s)...\r\n", ssid);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        if (print_fn) print_fn("       (open network, no password)\r\n");
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_wifi_set_config: %s\r\n",
                               esp_err_to_name(ret));
        return ret;
    }

    /* Step 3: Connect */
    if (print_fn) print_fn("[3/4] Connecting to AP...\r\n");

    s_state = WIRELESS_STATE_CONNECTING;
    xSemaphoreTake(s_connect_sem, 0);

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_wifi_connect: %s\r\n",
                               esp_err_to_name(ret));
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ret;
    }

    /* Step 4: Wait for IP */
    if (print_fn) print_fn("[4/4] Waiting for IP (max 15s)...\r\n");

    if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        if (print_fn) print_fn("FAIL: timeout waiting for IP\r\n");
        ESP_LOGW(TAG, "WiFi connect timeout");
        esp_wifi_disconnect();
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ESP_ERR_TIMEOUT;
    }

    if (s_state == WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("OK! Connected. IP: %s\r\n", s_ip_str);
        return ESP_OK;
    }

    if (print_fn) print_fn("FAIL: connection rejected (wrong password?)\r\n");
    return ESP_FAIL;
}

esp_err_t wireless_wifi_connect_static(const char *ssid, const char *password,
                                       const char *ip, const char *gw,
                                       const char *mask,
                                       wireless_print_fn_t print_fn)
{
    if (s_state != WIRELESS_STATE_DISCONNECTED &&
        s_state != WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("Error: invalid state (%d)\r\n", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("[1/5] Disconnecting...\r\n");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        if (print_fn) print_fn("[1/5] Preparing...\r\n");
    }

    /* Step 2: Configure WiFi */
    if (print_fn) print_fn("[2/5] WiFi config (SSID: %s)...\r\n", ssid);
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Step 3: Connect WiFi L2 */
    if (print_fn) print_fn("[3/5] Connecting WiFi L2...\r\n");
    s_state = WIRELESS_STATE_CONNECTING;
    xSemaphoreTake(s_connect_sem, 0);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_wifi_connect: %s\r\n", esp_err_to_name(ret));
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ret;
    }

    /* Wait for STA_CONNECTED (not IP — we'll set static IP) */
    if (print_fn) print_fn("       Waiting for L2 association (max 10s)...\r\n");
    /* We wait on the semaphore but check for CONNECTING state (not CONNECTED,
       since no DHCP will run). The semaphore fires on DISCONNECTED too. */
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        /* Check if STA_CONNECTED was received by checking netif UP */
        if (s_sta_netif && esp_netif_is_netif_up(s_sta_netif)) {
            break;
        }
        if (s_state == WIRELESS_STATE_DISCONNECTED) {
            if (print_fn) print_fn("FAIL: WiFi disconnected during association\r\n");
            return ESP_FAIL;
        }
    }
    if (!s_sta_netif || !esp_netif_is_netif_up(s_sta_netif)) {
        if (print_fn) print_fn("FAIL: L2 association timeout\r\n");
        esp_wifi_disconnect();
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ESP_ERR_TIMEOUT;
    }
    if (print_fn) print_fn("       L2 associated OK\r\n");

    /* Step 4: Stop DHCP client, set static IP */
    if (print_fn) print_fn("[4/5] Setting static IP: %s (gw: %s)...\r\n", ip, gw);
    esp_netif_dhcpc_stop(s_sta_netif);

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr      = ipaddr_addr(ip);
    ip_info.gw.addr      = ipaddr_addr(gw);
    ip_info.netmask.addr = ipaddr_addr(mask);

    ret = esp_netif_set_ip_info(s_sta_netif, &ip_info);
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_netif_set_ip_info: %s\r\n", esp_err_to_name(ret));
        esp_wifi_disconnect();
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ret;
    }

    snprintf(s_ip_str, sizeof(s_ip_str), "%s", ip);
    s_state = WIRELESS_STATE_CONNECTED;

    if (print_fn) print_fn("[5/5] Static IP set. Ready for ping test.\r\n");
    if (print_fn) print_fn("OK! IP: %s, GW: %s\r\n", ip, gw);
    return ESP_OK;
}

esp_err_t wireless_wifi_disconnect(void)
{
    if (s_state == WIRELESS_STATE_OFF || s_state == WIRELESS_STATE_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_wifi_disconnect();
    s_state = WIRELESS_STATE_DISCONNECTED;
    s_ip_str[0] = '\0';
    return ESP_OK;
}

bool wireless_wifi_is_connected(void)
{
    return s_state == WIRELESS_STATE_CONNECTED;
}

esp_err_t wireless_wifi_get_ip(char *buf, size_t len)
{
    if (s_state != WIRELESS_STATE_CONNECTED || s_ip_str[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(buf, s_ip_str, len);
    buf[len - 1] = '\0';
    return ESP_OK;
}

/* ── Ping ──────────────────────────────────────────────────────── */

typedef struct {
    wireless_print_fn_t print_fn;
    SemaphoreHandle_t   done_sem;
    uint32_t            total_time_ms;
} ping_ctx_t;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint8_t  ttl;
    uint16_t seqno;
    uint32_t elapsed, recv_len;
    ip_addr_t target;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,   &seqno,   sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,      &ttl,     sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP,  &elapsed, sizeof(elapsed));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,     &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,   &target,  sizeof(target));

    ctx->print_fn("%lu bytes from %s: seq=%d ttl=%d time=%lums\r\n",
                  (unsigned long)recv_len, ipaddr_ntoa(&target),
                  seqno, ttl, (unsigned long)elapsed);
    ctx->total_time_ms += elapsed;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ctx->print_fn("Request timed out (seq=%d)\r\n", seqno);
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint32_t transmitted, received;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,   &received,   sizeof(received));

    uint32_t loss = transmitted > 0
                    ? ((transmitted - received) * 100 / transmitted) : 0;
    uint32_t avg  = received > 0 ? (ctx->total_time_ms / received) : 0;

    ctx->print_fn("--- ping statistics ---\r\n");
    ctx->print_fn("%lu sent, %lu received, %lu%% loss",
                  (unsigned long)transmitted, (unsigned long)received,
                  (unsigned long)loss);
    if (received > 0) {
        ctx->print_fn(", avg %lums", (unsigned long)avg);
    }
    ctx->print_fn("\r\n");

    xSemaphoreGive(ctx->done_sem);
}

esp_err_t wireless_ping(const char *host, int count, wireless_print_fn_t print_fn)
{
    if (s_state != WIRELESS_STATE_CONNECTED) {
        print_fn("Error: WiFi not connected\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!host || !host[0]) return ESP_ERR_INVALID_ARG;
    if (count < 1)  count = 1;
    if (count > 20) count = 20;

    /* Resolve target address */
    ip_addr_t target_addr = {0};

    if (ipaddr_aton(host, &target_addr)) {
        /* Already a numeric IP */
    } else {
        /* DNS lookup */
        struct addrinfo hints = { .ai_family = AF_INET };
        struct addrinfo *res  = NULL;
        int err = getaddrinfo(host, NULL, &hints, &res);
        if (err != 0 || !res) {
            print_fn("DNS failed for '%s'\r\n", host);
            if (res) freeaddrinfo(res);
            return ESP_ERR_NOT_FOUND;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &sa->sin_addr);
        target_addr.type = IPADDR_TYPE_V4;
        print_fn("Resolved %s -> %s\r\n", host, ipaddr_ntoa(&target_addr));
        freeaddrinfo(res);
    }

    /* Ping context (lives on caller's stack, outlives the session) */
    ping_ctx_t ctx = {
        .print_fn      = print_fn,
        .done_sem      = xSemaphoreCreateBinary(),
        .total_time_ms = 0,
    };
    if (!ctx.done_sem) return ESP_ERR_NO_MEM;

    esp_ping_config_t pcfg = ESP_PING_DEFAULT_CONFIG();
    pcfg.target_addr  = target_addr;
    pcfg.count        = count;
    pcfg.interval_ms  = 1000;
    pcfg.timeout_ms   = 5000;
    pcfg.task_stack_size = 4096;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end     = ping_end_cb,
        .cb_args         = &ctx,
    };

    esp_ping_handle_t ping_hdl;
    esp_err_t ret = esp_ping_new_session(&pcfg, &cbs, &ping_hdl);
    if (ret != ESP_OK) {
        print_fn("Ping session failed: %s\r\n", esp_err_to_name(ret));
        vSemaphoreDelete(ctx.done_sem);
        return ret;
    }

    esp_ping_start(ping_hdl);

    /* Block until end callback fires (generous timeout) */
    TickType_t max_wait = pdMS_TO_TICKS((uint32_t)count * 6000 + 3000);
    xSemaphoreTake(ctx.done_sem, max_wait);

    esp_ping_stop(ping_hdl);
    esp_ping_delete_session(ping_hdl);
    vSemaphoreDelete(ctx.done_sem);

    return ESP_OK;
}

/* ── Status / Info ─────────────────────────────────────────────── */

wireless_state_t wireless_get_state(void)
{
    return s_state;
}

void wireless_print_info(wireless_print_fn_t print_fn)
{
    static const char *state_names[] = {
        "OFF", "DISCONNECTED", "CONNECTING", "CONNECTED", "ERROR"
    };
    print_fn("=== Wireless Info ===\r\n");
    print_fn("State: %s\r\n", state_names[s_state]);

    if (s_state == WIRELESS_STATE_CONNECTED) {
        print_fn("IP: %s\r\n", s_ip_str);
    }

    if (s_state == WIRELESS_STATE_OFF) {
        print_fn("(not initialized)\r\n");
        return;
    }

    /* Query C5 firmware version (live RPC) */
    esp_hosted_coprocessor_fwver_t fwver;
    if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
        print_fn("C5 FW version: %lu.%lu.%lu\r\n",
                 (unsigned long)fwver.major1,
                 (unsigned long)fwver.minor1,
                 (unsigned long)fwver.patch1);
    } else {
        print_fn("C5 FW version: unavailable (RPC timeout)\r\n");
    }

    /* Query C5 app descriptor (live RPC) */
    esp_hosted_app_desc_t app_desc;
    if (esp_hosted_get_coprocessor_app_desc(&app_desc) == ESP_OK) {
        print_fn("C5 project:    %.32s\r\n", app_desc.project_name);
        print_fn("C5 app ver:    %.32s\r\n", app_desc.version);
        print_fn("C5 IDF ver:    %.32s\r\n", app_desc.idf_ver);
        print_fn("C5 built:      %.16s %.16s\r\n", app_desc.date, app_desc.time);
    } else {
        print_fn("C5 app desc:   unavailable (RPC timeout)\r\n");
    }

    /* WiFi MAC */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        print_fn("STA MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

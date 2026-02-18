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
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"

#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

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

static volatile bool     s_speed_abort = false;

/* Forward declarations */
static bool diag_ping_one(ip_addr_t *target, uint32_t *out_ms);

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
                struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
                ESP_LOGI(TAG, "netif flags: UP=%d, link_UP=%d",
                         esp_netif_is_netif_up(s_sta_netif),
                         nif ? netif_is_link_up(nif) : 0);
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
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ev->ip_info.gw));
        ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ev->ip_info.netmask));

        /* DNS fallback — priority: Google (8.8.8.8) > Cloudflare (1.1.1.1)
         *
         * ESP-IDF lwIP (ESP_DNS=1) reserves DNS slot 1 as "fallback"
         * and DHCP skips it (dhcp.c:805). So only DNS0 comes from DHCP.
         *
         * Cases:
         *   DHCP gave DNS0 + DNS1  → keep both (nothing to do)
         *   DHCP gave DNS0 only    → DNS1 = Google
         *   DHCP gave nothing      → DNS0 = Google, DNS1 = Cloudflare
         */
        ip_addr_t dns0 = *dns_getserver(0);
        ip_addr_t dns1 = *dns_getserver(1);
        ESP_LOGI(TAG, "DNS0 (from DHCP): %s", ipaddr_ntoa(&dns0));
        ESP_LOGI(TAG, "DNS1 (fallback slot): %s", ipaddr_ntoa(&dns1));

        ip_addr_t google_dns, cf_dns;
        ipaddr_aton("8.8.8.8", &google_dns);
        ipaddr_aton("1.1.1.1", &cf_dns);

        if (ip_addr_isany(&dns0) && ip_addr_isany(&dns1)) {
            dns_setserver(0, &google_dns);
            dns_setserver(1, &cf_dns);
            ESP_LOGW(TAG, "No DNS from DHCP — set DNS0=8.8.8.8, DNS1=1.1.1.1");
        } else if (ip_addr_isany(&dns1)) {
            dns_setserver(1, &google_dns);
            ESP_LOGI(TAG, "Set DNS1 = 8.8.8.8 (fallback)");
        } else if (ip_addr_isany(&dns0)) {
            dns_setserver(0, &google_dns);
            ESP_LOGW(TAG, "DNS0 empty — set DNS0 = 8.8.8.8");
        }

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

    /*
     * Disable WiFi modem sleep on the companion chip.
     * With power save enabled the C6 radio goes dormant after a few
     * seconds, killing the data path while lwIP still shows CONNECTED.
     * esp_wifi_set_ps() is forwarded to the slave via esp_wifi_remote.
     */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_connect_sem = xSemaphoreCreateBinary();
    s_state = WIRELESS_STATE_DISCONNECTED;

    /*
     * NOTE: After cold boot the C6 companion may need a few extra seconds
     * before SDIO data path is fully stable. The first wifi connect attempt
     * can fail with "DHCP: not initialized". A second attempt succeeds.
     * TODO: add automatic retry or readiness check if this becomes a UX issue.
     */

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

/* ── DHCP diagnostics ──────────────────────────────────────────── */

/**
 * Log lwIP internal DHCP state — helps identify where DHCP gets stuck:
 *   SELECTING(6) = DISCOVER sent, no OFFER received
 *   REQUESTING(1) = OFFER received, REQUEST sent, no ACK
 *   CHECKING(8) = ACK received, ARP probe in progress
 *   BOUND(10) = IP assigned (success)
 */
static void log_dhcp_state(wireless_print_fn_t print_fn)
{
    if (!s_sta_netif) return;

    struct netif *n = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
    if (!n) return;

    struct dhcp *d = netif_dhcp_data(n);
    if (!d) {
        ESP_LOGW(TAG, "DHCP data NULL (never started?)");
        if (print_fn) print_fn("  DHCP: not initialized\r\n");
        return;
    }

    static const char *state_names[] = {
        [0] = "OFF", [1] = "REQUESTING", [2] = "INIT",
        [3] = "REBOOTING", [4] = "REBINDING", [5] = "RENEWING",
        [6] = "SELECTING", [7] = "INFORMING", [8] = "CHECKING",
        [9] = "PERMANENT", [10] = "BOUND", [11] = "RELEASING",
        [12] = "BACKING_OFF",
    };
    const char *sn = (d->state <= 12) ? state_names[d->state] : "?";

    ESP_LOGW(TAG, "DHCP: state=%d(%s) tries=%d  netif: up=%d link=%d ip=" IPSTR,
             d->state, sn, d->tries,
             netif_is_up(n), netif_is_link_up(n),
             IP2STR(netif_ip4_addr(n)));

    if (print_fn) {
        print_fn("  DHCP: state=%d(%s) tries=%d, netif up=%d link=%d\r\n",
                 d->state, sn, d->tries,
                 netif_is_up(n), netif_is_link_up(n));
    }
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
        if (print_fn) print_fn("[1/3] Disconnecting...\r\n");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        if (print_fn) print_fn("[1/3] Preparing...\r\n");
    }

    /* Step 2: Configure WiFi */
    if (print_fn) print_fn("[2/3] Connecting to '%s'...\r\n", ssid);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_wifi_set_config: %s\r\n",
                               esp_err_to_name(ret));
        return ret;
    }

    /* Step 3: Connect and wait for DHCP
     *
     * The default handler in esp_wifi_remote_net2.c automatically:
     *   1. Sets RX callback (s_rx_fn) at STA_CONNECTED
     *   2. Calls esp_netif_action_connected() which starts DHCP
     *
     * We do NOT manually stop/start DHCP — that interferes with the
     * default handler and causes "invalid static ip" errors.
     */
    s_state = WIRELESS_STATE_CONNECTING;
    xSemaphoreTake(s_connect_sem, 0);

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        if (print_fn) print_fn("FAIL: esp_wifi_connect: %s\r\n",
                               esp_err_to_name(ret));
        s_state = WIRELESS_STATE_DISCONNECTED;
        return ret;
    }

    if (print_fn) print_fn("[3/3] Waiting for IP (DHCP)...\r\n");

    /* Wait with periodic DHCP state logging (8 × 2s = 16s max) */
    bool got_sem = false;
    for (int i = 0; i < 8; i++) {
        if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            got_sem = true;
            break;
        }
        /* Disconnected while waiting? */
        if (s_state == WIRELESS_STATE_DISCONNECTED) {
            if (print_fn) print_fn("FAIL: disconnected\r\n");
            return ESP_FAIL;
        }
        /* Log DHCP progress every 2s */
        log_dhcp_state(print_fn);
    }

    if (got_sem && s_state == WIRELESS_STATE_CONNECTED) {
        if (print_fn) print_fn("OK! IP: %s\r\n", s_ip_str);
        return ESP_OK;
    }

    /* Timeout — final DHCP state dump */
    log_dhcp_state(print_fn);
    if (print_fn) print_fn("FAIL: DHCP timeout (no IP in ~16s)\r\n");
    ESP_LOGW(TAG, "WiFi DHCP timeout");
    esp_wifi_disconnect();
    s_state = WIRELESS_STATE_DISCONNECTED;
    return ESP_ERR_TIMEOUT;
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
        int err = lwip_getaddrinfo(host, NULL, &hints, &res);
        if (err != 0 || !res) {
            print_fn("DNS failed for '%s'\r\n", host);
            if (res) lwip_freeaddrinfo(res);
            return ESP_ERR_NOT_FOUND;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &sa->sin_addr);
        target_addr.type = IPADDR_TYPE_V4;
        print_fn("Resolved %s -> %s\r\n", host, ipaddr_ntoa(&target_addr));
        lwip_freeaddrinfo(res);
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

/* ── Network diagnostics ──────────────────────────────────────── */

/** Single-ping helper for diagnostics. Returns true if reply received. */
typedef struct {
    SemaphoreHandle_t sem;
    bool              ok;
    uint32_t          time_ms;
} diag_ping_ctx_t;

static void diag_ping_ok(esp_ping_handle_t hdl, void *args)
{
    diag_ping_ctx_t *c = (diag_ping_ctx_t *)args;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &c->time_ms, sizeof(c->time_ms));
    c->ok = true;
    /* Don't signal here — wait for on_ping_end (last callback) */
}

static void diag_ping_tmo(esp_ping_handle_t hdl, void *args)
{
    diag_ping_ctx_t *c = (diag_ping_ctx_t *)args;
    c->ok = false;
    /* Don't signal here — wait for on_ping_end (last callback) */
}

static void diag_ping_end(esp_ping_handle_t hdl, void *args)
{
    diag_ping_ctx_t *c = (diag_ping_ctx_t *)args;
    xSemaphoreGive(c->sem);
}

static bool diag_ping_one(ip_addr_t *target, uint32_t *out_ms)
{
    diag_ping_ctx_t ctx = {
        .sem = xSemaphoreCreateBinary(),
        .ok  = false,
        .time_ms = 0,
    };
    if (!ctx.sem) return false;

    esp_ping_config_t pcfg = ESP_PING_DEFAULT_CONFIG();
    pcfg.target_addr     = *target;
    pcfg.count           = 1;
    pcfg.interval_ms     = 1000;
    pcfg.timeout_ms      = 3000;
    pcfg.task_stack_size  = 4096;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = diag_ping_ok,
        .on_ping_timeout = diag_ping_tmo,
        .on_ping_end     = diag_ping_end,
        .cb_args         = &ctx,
    };

    esp_ping_handle_t hdl;
    if (esp_ping_new_session(&pcfg, &cbs, &hdl) != ESP_OK) {
        vSemaphoreDelete(ctx.sem);
        return false;
    }

    esp_ping_start(hdl);
    xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(5000));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    vSemaphoreDelete(ctx.sem);

    if (out_ms) *out_ms = ctx.time_ms;
    return ctx.ok;
}

esp_err_t wireless_wifi_diag(wireless_print_fn_t print_fn)
{
    if (s_state != WIRELESS_STATE_CONNECTED) {
        print_fn("Error: WiFi not connected\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    print_fn("=== Network Diagnostics ===\r\n");

    /* L2: WiFi state */
    print_fn("[L2] WiFi: CONNECTED\r\n");

    /* L3: IP / Gateway / Netmask */
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_sta_netif, &ip_info);
    print_fn("[L3] IP: " IPSTR "  GW: " IPSTR "  Mask: " IPSTR "\r\n",
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));

    /* L3: netif flags */
    struct netif *n = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
    if (n) {
        print_fn("[L3] netif: UP=%d, LINK_UP=%d\r\n",
                 netif_is_up(n), netif_is_link_up(n));
    }

    /* DHCP state */
    if (n) {
        struct dhcp *d = netif_dhcp_data(n);
        if (d) {
            static const char *snames[] = {
                [0]="OFF", [1]="REQUESTING", [2]="INIT", [3]="REBOOTING",
                [4]="REBINDING", [5]="RENEWING", [6]="SELECTING",
                [7]="INFORMING", [8]="CHECKING", [9]="PERMANENT",
                [10]="BOUND", [11]="RELEASING", [12]="BACKING_OFF",
            };
            const char *sn = (d->state <= 12) ? snames[d->state] : "?";
            print_fn("[L3] DHCP: state=%d(%s) tries=%d\r\n",
                     d->state, sn, d->tries);
        }
    }

    /* DNS servers */
    ip_addr_t dns0 = *dns_getserver(0);
    ip_addr_t dns1 = *dns_getserver(1);
    /* ipaddr_ntoa uses static buffer — print separately */
    print_fn("[DNS] DNS0: %s", ipaddr_ntoa(&dns0));
    print_fn("   DNS1: %s\r\n", ipaddr_ntoa(&dns1));

    /* Ping gateway */
    if (ip_info.gw.addr != 0) {
        ip_addr_t gw_addr;
        ip_addr_set_ip4_u32_val(gw_addr, ip_info.gw.addr);
        uint32_t ms = 0;
        print_fn("[L3] Ping gateway " IPSTR " ... ", IP2STR(&ip_info.gw));
        if (diag_ping_one(&gw_addr, &ms)) {
            print_fn("OK (%lums)\r\n", (unsigned long)ms);
        } else {
            print_fn("FAIL\r\n");
        }
    } else {
        print_fn("[L3] Gateway: not set!\r\n");
    }

    /* Ping 8.8.8.8 */
    {
        ip_addr_t inet_addr;
        ipaddr_aton("8.8.8.8", &inet_addr);
        uint32_t ms = 0;
        print_fn("[L3] Ping 8.8.8.8 ... ");
        if (diag_ping_one(&inet_addr, &ms)) {
            print_fn("OK (%lums)\r\n", (unsigned long)ms);
        } else {
            print_fn("FAIL\r\n");
        }
    }

    /* DNS resolution test */
    {
        struct addrinfo hints = { .ai_family = AF_INET };
        struct addrinfo *res = NULL;
        print_fn("[DNS] Resolve google.com ... ");
        int err = lwip_getaddrinfo("google.com", NULL, &hints, &res);
        if (err == 0 && res) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            char ip_str[16];
            inet_ntoa_r(sa->sin_addr, ip_str, sizeof(ip_str));
            print_fn("OK (%s)\r\n", ip_str);
            lwip_freeaddrinfo(res);
        } else {
            print_fn("FAIL (err=%d)\r\n", err);
            if (res) lwip_freeaddrinfo(res);
        }
    }

    print_fn("=== End ===\r\n");
    return ESP_OK;
}

/* ── Speed test (HTTP download) ───────────────────────────────── */

#include "lwip/sockets.h"

#define SPEED_BUF_SIZE      16384
#define SPEED_DEFAULT_URL   "http://speedtest.tele2.net/10MB.zip"
#define SPEED_MAX_REDIRECTS 3

/**
 * Parse "http://host[:port]/path" into components.
 * Returns false if scheme is not http:// .
 */
static bool parse_http_url(const char *url, char *host, size_t host_len,
                           char *path, size_t path_len, uint16_t *port)
{
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    const char *colon = NULL;

    /* Find ':' only within the host part (before first '/') */
    for (const char *p = h; p < (slash ? slash : h + strlen(h)); p++) {
        if (*p == ':') { colon = p; break; }
    }

    if (colon) {
        size_t hlen = colon - h;
        if (hlen == 0 || hlen >= host_len) return false;
        memcpy(host, h, hlen);
        host[hlen] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        *port = 80;
        size_t hlen = slash ? (size_t)(slash - h) : strlen(h);
        if (hlen == 0 || hlen >= host_len) return false;
        memcpy(host, h, hlen);
        host[hlen] = '\0';
    }

    if (slash) {
        strncpy(path, slash, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        strncpy(path, "/", path_len - 1);
    }
    return true;
}

/**
 * HTTP GET one attempt: connect, send request, read response headers.
 * Returns the connected socket with headers consumed, or -1 on error.
 * On redirect (301/302), fills redirect_url and returns -2.
 */
static int speed_http_get(const char *host, uint16_t port, const char *path,
                          wireless_print_fn_t print_fn,
                          uint8_t *buf, int *out_content_length,
                          size_t *out_body_in_buf,
                          char *redirect_url, size_t redirect_url_len)
{
    *out_content_length = -1;
    *out_body_in_buf = 0;

    /* DNS resolve */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int dns_err = lwip_getaddrinfo(host, NULL, &hints, &res);
    if (dns_err != 0 || !res) {
        print_fn("DNS failed for '%s' (err=%d)\r\n", host, dns_err);
        if (res) lwip_freeaddrinfo(res);
        return -1;
    }
    struct sockaddr_in dest;
    memcpy(&dest, res->ai_addr, sizeof(dest));
    dest.sin_port = htons(port);
    char ip_str[16];
    inet_ntoa_r(dest.sin_addr, ip_str, sizeof(ip_str));
    print_fn("Resolved %s -> %s\r\n", host, ip_str);
    lwip_freeaddrinfo(res);

    /* Connect with send timeout (affects connect on lwIP) */
    print_fn("Connecting to %s:%d...\r\n", ip_str, port);
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        print_fn("Socket error (errno=%d)\r\n", errno);
        return -1;
    }

    struct timeval tv = { .tv_sec = 10 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        print_fn("Connect failed (errno=%d)\r\n", errno);
        close(sock);
        return -1;
    }
    print_fn("Connected.\r\n");

    /* Send HTTP GET */
    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: Lyra/1.0\r\n"
        "\r\n", path, host);

    if (send(sock, req, req_len, 0) != req_len) {
        print_fn("Send failed (errno=%d)\r\n", errno);
        close(sock);
        return -1;
    }

    /* Read headers — find \r\n\r\n */
    size_t hdr_used = 0;
    bool headers_done = false;
    size_t hdr_end_offset = 0;

    while (!headers_done && hdr_used < SPEED_BUF_SIZE - 1) {
        int r = recv(sock, buf + hdr_used, SPEED_BUF_SIZE - 1 - hdr_used, 0);
        if (r <= 0) break;
        hdr_used += r;
        buf[hdr_used] = '\0';

        char *end = strstr((char *)buf, "\r\n\r\n");
        if (end) {
            hdr_end_offset = (end - (char *)buf) + 4;
            headers_done = true;
        }
    }

    if (!headers_done) {
        print_fn("No HTTP response (errno=%d)\r\n", errno);
        close(sock);
        return -1;
    }

    /* Parse HTTP status */
    int http_status = 0;
    sscanf((char *)buf, "HTTP/%*d.%*d %d", &http_status);

    /* Handle redirects */
    if (http_status == 301 || http_status == 302 || http_status == 307) {
        char *loc = strstr((char *)buf, "Location:");
        if (!loc) loc = strstr((char *)buf, "location:");
        if (loc) {
            loc += 9;
            while (*loc == ' ') loc++;
            char *eol = strchr(loc, '\r');
            if (eol) *eol = '\0';
            strncpy(redirect_url, loc, redirect_url_len - 1);
            redirect_url[redirect_url_len - 1] = '\0';
        }
        close(sock);
        return -2;  /* redirect */
    }

    if (http_status < 200 || http_status >= 300) {
        char *eol = strchr((char *)buf, '\r');
        if (eol) *eol = '\0';
        print_fn("HTTP %d: %s\r\n", http_status, (char *)buf);
        close(sock);
        return -1;
    }

    /* Parse Content-Length */
    char *cl = strstr((char *)buf, "Content-Length:");
    if (!cl) cl = strstr((char *)buf, "content-length:");
    if (cl) *out_content_length = atoi(cl + 15);

    *out_body_in_buf = hdr_used - hdr_end_offset;
    return sock;  /* success — caller owns the socket */
}

esp_err_t wireless_speed_test(const char *url, wireless_print_fn_t print_fn)
{
    if (s_state != WIRELESS_STATE_CONNECTED) {
        print_fn("Error: WiFi not connected\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || !url[0]) url = SPEED_DEFAULT_URL;

    uint8_t *buf = heap_caps_malloc(SPEED_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(SPEED_BUF_SIZE);  /* fallback to internal */
    if (!buf) return ESP_ERR_NO_MEM;

    /* Follow redirects (HTTP→HTTP only) */
    char current_url[256];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    int sock = -1;
    int content_length = -1;
    size_t body_in_buf = 0;

    for (int redir = 0; redir <= SPEED_MAX_REDIRECTS; redir++) {
        char host[64], path[128];
        uint16_t port;

        if (!parse_http_url(current_url, host, sizeof(host),
                            path, sizeof(path), &port)) {
            if (strncmp(current_url, "https://", 8) == 0) {
                print_fn("HTTPS not supported (no TLS). Cannot follow redirect.\r\n");
            } else {
                print_fn("Invalid URL: %s\r\n", current_url);
            }
            free(buf);
            return ESP_ERR_INVALID_ARG;
        }

        print_fn("%sDownload: %s\r\n", redir ? "\r\n" : "", current_url);

        char redirect_url[256] = {0};
        sock = speed_http_get(host, port, path, print_fn, buf,
                              &content_length, &body_in_buf,
                              redirect_url, sizeof(redirect_url));

        if (sock == -2) {
            /* Redirect */
            print_fn("Redirect -> %s\r\n", redirect_url);
            strncpy(current_url, redirect_url, sizeof(current_url) - 1);
            current_url[sizeof(current_url) - 1] = '\0';
            continue;
        }
        break;  /* success or error */
    }

    if (sock < 0) {
        free(buf);
        return ESP_FAIL;
    }

    /* Download body and measure speed */
    uint32_t total_bytes = (uint32_t)body_in_buf;

    if (content_length > 0) {
        print_fn("File: %d bytes (%.1f KB)\r\n",
                 content_length, (float)content_length / 1024.0f);
    }
    print_fn("Downloading...\r\n");

    int64_t t_start = esp_timer_get_time();
    int64_t last_progress = t_start;

    /* 1s recv timeout — short so "wifi stop" abort is responsive */
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_speed_abort = false;

    int stall_count = 0;
    while (!s_speed_abort) {
        int r = recv(sock, buf, SPEED_BUF_SIZE, 0);
        if (r < 0 && errno == EAGAIN) {
            if (++stall_count >= 5) break;  /* 5s with no data → done */
            continue;
        }
        if (r <= 0) break;
        stall_count = 0;
        total_bytes += r;

        /* Progress every ~2 seconds */
        int64_t now = esp_timer_get_time();
        if (now - last_progress > 2000000) {
            float elapsed = (float)(now - t_start) / 1000000.0f;
            float cur_kBs = (elapsed > 0) ? ((float)total_bytes / elapsed / 1024.0f) : 0;
            if (content_length > 0) {
                int pct = (int)((uint64_t)total_bytes * 100 / content_length);
                print_fn("  %lu/%d (%d%%) %.0f KB/s\r\n",
                         (unsigned long)total_bytes, content_length, pct, cur_kBs);
            } else {
                print_fn("  %lu bytes %.0f KB/s\r\n",
                         (unsigned long)total_bytes, cur_kBs);
            }
            last_progress = now;
        }
    }

    bool aborted = s_speed_abort;
    s_speed_abort = false;

    int64_t t_end = esp_timer_get_time();
    float elapsed_s = (float)(t_end - t_start) / 1000000.0f;

    close(sock);
    free(buf);

    if (total_bytes == 0 || elapsed_s <= 0) {
        print_fn("No data received\r\n");
        return ESP_FAIL;
    }

    float kBs  = (float)total_bytes / elapsed_s / 1024.0f;
    float mbps = (float)total_bytes * 8.0f / elapsed_s / 1000000.0f;

    print_fn("\r\n%sResult: %lu bytes in %.1fs\r\n",
             aborted ? "[Aborted] " : "",
             (unsigned long)total_bytes, elapsed_s);
    print_fn("Speed: %.2f Mbps (%.0f KB/s)\r\n", mbps, kBs);
    return aborted ? ESP_ERR_TIMEOUT : ESP_OK;
}

void wireless_speed_test_abort(void)
{
    s_speed_abort = true;
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

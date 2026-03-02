/*
 * ui_wifi.c — WiFi sub-screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  WiFi                        │  Header (48px)
 *   ├──────────────────────────────────┤
 *   │  Connected to "HomeNetwork"      │  Connection card (if connected)
 *   │  -52 dBm      [Disconnect]      │
 *   ├──────────────────────────────────┤
 *   │        [ Scan Networks ]         │  Scan button
 *   ├──────────────────────────────────┤
 *   │  HomeNetwork    -52 dBm  *   >  │  Scrollable scan results
 *   │  ─────────────────────────────   │
 *   │  CafeWiFi       -71 dBm     >  │
 *   │          ... (scrollable) ...   │
 *   └──────────────────────────────────┘
 *
 * No bottom nav bar (sub-screen — back returns to Settings).
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_wifi;

/* Header */
static lv_obj_t *btn_back;

/* Connection card */
static lv_obj_t *card_connected;
static lv_obj_t *lbl_conn_ssid;
static lv_obj_t *lbl_conn_rssi;

/* Scan button + spinner */
static lv_obj_t *btn_scan;
static lv_obj_t *spinner_scan;

/* Scan results */
static lv_obj_t *list_container;
static lv_obj_t *lbl_scan_status;

/* Password dialog */
static lv_obj_t *dlg_password;
static lv_obj_t *lbl_dlg_ssid;
static lv_obj_t *ta_password;
static lv_obj_t *sw_show_pwd;
static lv_obj_t *kb_password;

/* AP click context */
typedef struct {
    char           ssid[33];
    ui_wifi_auth_t auth;
} wifi_ap_ctx_t;

static wifi_ap_ctx_t s_ap_ctx[UI_WIFI_MAX_APS];
static char s_connecting_ssid[33];

/* Dirty-check */
static int             s_last_ap_count = -1;
static ui_wifi_state_t s_last_state = UI_WIFI_IDLE;

/* Toast state tracking (s_toast_seeded=false → first update seeds without firing) */
static bool            s_toast_seeded;
static ui_wifi_state_t s_prev_toast_state = UI_WIFI_IDLE;
static char s_prev_toast_ssid[33];

/* -----------------------------------------------------------------------
 * Password dialog helpers
 * ----------------------------------------------------------------------- */

static void show_password_dialog(const char *ssid)
{
    char buf[64];
    lv_snprintf(buf, sizeof(buf), "Connect to \"%s\"", ssid);
    lv_label_set_text(lbl_dlg_ssid, buf);
    lv_textarea_set_text(ta_password, "");
    lv_textarea_set_password_mode(ta_password, true);
    lv_obj_clear_state(sw_show_pwd, LV_STATE_CHECKED);
    lv_obj_clear_flag(dlg_password, LV_OBJ_FLAG_HIDDEN);
}

static void hide_password_dialog(void)
{
    lv_obj_add_flag(dlg_password, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ta_password, "");
}

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    hide_password_dialog();
    ui_navigate_to(UI_SCREEN_SETTINGS);
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    ui_cmd_wifi_scan();
    s_last_ap_count = -1;  /* Force list rebuild */
}

static void on_disconnect(lv_event_t *e)
{
    (void)e;
    ui_cmd_wifi_disconnect();
    s_last_ap_count = -1;
    ui_toast_show(UI_TOAST_INFO, "WiFi disconnected", 2500);
}

static void on_ap_click(lv_event_t *e)
{
    wifi_ap_ctx_t *ctx = (wifi_ap_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->auth == UI_WIFI_AUTH_OPEN) {
        ui_cmd_wifi_connect(ctx->ssid, "");
        s_last_ap_count = -1;
    } else {
        strncpy(s_connecting_ssid, ctx->ssid, sizeof(s_connecting_ssid) - 1);
        s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
        show_password_dialog(ctx->ssid);
    }
}

static void on_dlg_connect(lv_event_t *e)
{
    (void)e;
    const char *pwd = lv_textarea_get_text(ta_password);
    ui_cmd_wifi_connect(s_connecting_ssid, pwd);
    hide_password_dialog();
    s_last_ap_count = -1;
}

static void on_dlg_cancel(lv_event_t *e)
{
    (void)e;
    hide_password_dialog();
}

static void on_show_pwd_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool show = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(ta_password, !show);
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_dlg_connect(e);
    } else if (code == LV_EVENT_CANCEL) {
        hide_password_dialog();
    }
}

/* -----------------------------------------------------------------------
 * Helper: rebuild the AP list
 * ----------------------------------------------------------------------- */

static void rebuild_ap_list(const ui_wifi_scan_data_t *data)
{
    lv_obj_clean(list_container);
    lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);

    for (int i = 0; i < data->ap_count; i++) {
        const ui_wifi_ap_t *ap = &data->aps[i];

        /* Copy context for event handler */
        strncpy(s_ap_ctx[i].ssid, ap->ssid, sizeof(s_ap_ctx[i].ssid) - 1);
        s_ap_ctx[i].ssid[sizeof(s_ap_ctx[i].ssid) - 1] = '\0';
        s_ap_ctx[i].auth = ap->auth;

        bool is_connected = (data->connected_ssid[0] != '\0' &&
                             strcmp(ap->ssid, data->connected_ssid) == 0);

        /* Row container */
        lv_obj_t *row = lv_obj_create(list_container);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 10, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

        /* Press feedback */
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        /* WiFi icon */
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_add_style(icon, ui_theme_style_info(), 0);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        if (is_connected)
            lv_obj_set_style_text_color(icon, ui_theme_color_accent(), 0);
        else if (ap->rssi >= -60)
            lv_obj_set_style_text_color(icon, ui_theme_color_text_primary(), 0);
        else
            lv_obj_set_style_text_color(icon, ui_theme_color_text_secondary(), 0);

        /* SSID label */
        lv_obj_t *name = lv_label_create(row);
        lv_obj_add_style(name, ui_theme_style_list_item(), 0);
        lv_label_set_text(name, ap->ssid);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        if (is_connected)
            lv_obj_set_style_text_color(name, ui_theme_color_accent(), 0);

        /* RSSI value */
        char rssi_buf[16];
        lv_snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", ap->rssi);
        lv_obj_t *rssi_lbl = lv_label_create(row);
        lv_obj_add_style(rssi_lbl, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(rssi_lbl,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(rssi_lbl, rssi_buf);

        /* Security indicator */
        if (ap->auth == UI_WIFI_AUTH_OPEN) {
            /* Open networks: green "Open" chip — stands out as the exception */
            lv_obj_t *chip = lv_obj_create(row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(chip, lv_color_hex(0x1B3D1B), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(chip, 10, 0);
            lv_obj_set_style_pad_hor(chip, 8, 0);
            lv_obj_set_style_pad_ver(chip, 2, 0);
            lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
            lv_obj_t *chip_lbl = lv_label_create(chip);
            lv_obj_set_style_text_color(chip_lbl, lv_color_hex(0x4CAF50), 0);
            lv_obj_set_style_text_font(chip_lbl, &lv_font_montserrat_14, 0);
            lv_label_set_text(chip_lbl, "Open");
        } else {
            /* Secured networks: dim lock icon */
            lv_obj_t *lock = lv_label_create(row);
            lv_obj_add_style(lock, ui_theme_style_info(), 0);
            lv_obj_set_style_text_color(lock,
                                        ui_theme_color_text_secondary(), 0);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
        }

        /* Connected checkmark or chevron */
        if (is_connected) {
            lv_obj_t *check = lv_label_create(row);
            lv_obj_add_style(check, ui_theme_style_info(), 0);
            lv_obj_set_style_text_color(check, ui_theme_color_accent(), 0);
            lv_label_set_text(check, LV_SYMBOL_OK);
        }

        /* Click event (only for non-connected APs) */
        if (!is_connected) {
            lv_obj_add_event_cb(row, on_ap_click, LV_EVENT_CLICKED,
                                &s_ap_ctx[i]);
        }

        /* Divider */
        if (i < data->ap_count - 1) {
            lv_obj_t *div = lv_obj_create(list_container);
            lv_obj_remove_style_all(div);
            lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
            lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
            lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
        }
    }
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_wifi_create(void)
{
    scr_wifi = lv_obj_create(NULL);
    lv_obj_add_style(scr_wifi, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_wifi, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_wifi);
    lv_obj_remove_style_all(main);
    lv_obj_set_size(main, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_gap(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(header, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 8, 0);

    /* Back button */
    btn_back = lv_button_create(header);
    lv_obj_add_style(btn_back, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_back, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_back, 8, 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(header);
    lv_obj_add_style(lbl_title, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_title, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_title, "WiFi");

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    lv_obj_t *content = lv_obj_create(main);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, UI_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    /* Themed scrollbar: thin, dark, rounded */
    lv_obj_set_style_bg_color(content, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, (lv_opa_t)120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);

    /* ==================================================================
     * CONNECTION CARD (hidden when not connected)
     * ================================================================== */
    card_connected = lv_obj_create(content);
    lv_obj_remove_style_all(card_connected);
    lv_obj_add_style(card_connected, ui_theme_style_card(), 0);
    lv_obj_set_size(card_connected, UI_HOR_RES - 2 * PAD_SIDE,
                    LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(card_connected, 16, 0);
    lv_obj_set_style_pad_all(card_connected, 16, 0);
    lv_obj_set_flex_flow(card_connected, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_connected, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card_connected, 8, 0);
    lv_obj_set_scrollbar_mode(card_connected, LV_SCROLLBAR_MODE_OFF);

    /* "Connected to" label */
    lv_obj_t *lbl_conn_title = lv_label_create(card_connected);
    lv_obj_add_style(lbl_conn_title, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_conn_title,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_conn_title, "Connected to");

    /* SSID */
    lbl_conn_ssid = lv_label_create(card_connected);
    lv_obj_add_style(lbl_conn_ssid, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_conn_ssid, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_conn_ssid, "");

    /* RSSI + Disconnect row */
    lv_obj_t *conn_row = lv_obj_create(card_connected);
    lv_obj_remove_style_all(conn_row);
    lv_obj_set_size(conn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(conn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(conn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(conn_row, LV_SCROLLBAR_MODE_OFF);

    lbl_conn_rssi = lv_label_create(conn_row);
    lv_obj_add_style(lbl_conn_rssi, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_conn_rssi,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_conn_rssi, "");

    lv_obj_t *btn_disc = lv_button_create(conn_row);
    lv_obj_add_style(btn_disc, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_disc, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn_disc, 16, 0);
    lv_obj_set_style_pad_ver(btn_disc, 6, 0);
    lv_obj_t *lbl_disc = lv_label_create(btn_disc);
    lv_obj_add_style(lbl_disc, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_disc, lv_color_hex(0xFF6666), 0);
    lv_label_set_text(lbl_disc, "Disconnect");
    lv_obj_add_event_cb(btn_disc, on_disconnect, LV_EVENT_CLICKED, NULL);

    /* Initially hidden */
    lv_obj_add_flag(card_connected, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * SCAN BUTTON + SPINNER
     * ================================================================== */
    lv_obj_t *scan_row = lv_obj_create(content);
    lv_obj_remove_style_all(scan_row);
    lv_obj_set_size(scan_row, UI_HOR_RES - 2 * PAD_SIDE, 56);
    lv_obj_set_style_margin_top(scan_row, 16, 0);
    lv_obj_set_flex_flow(scan_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scan_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(scan_row, 12, 0);
    lv_obj_set_scrollbar_mode(scan_row, LV_SCROLLBAR_MODE_OFF);

    btn_scan = lv_button_create(scan_row);
    lv_obj_set_style_bg_opa(btn_scan, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_scan, ui_theme_color_accent(), 0);
    lv_obj_set_style_border_width(btn_scan, 2, 0);
    lv_obj_set_style_border_opa(btn_scan, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_scan, 24, 0);
    lv_obj_set_style_pad_hor(btn_scan, 28, 0);
    lv_obj_set_style_pad_ver(btn_scan, 12, 0);
    lv_obj_set_style_shadow_width(btn_scan, 0, 0);
    /* Pressed: fill gold, dark text */
    lv_obj_set_style_bg_color(btn_scan, ui_theme_color_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_scan, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn_scan, ui_theme_color_bg(), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn_scan, LV_OPA_TRANSP, LV_STATE_PRESSED);

    lv_obj_t *lbl_scan = lv_label_create(btn_scan);
    lv_obj_set_style_text_color(lbl_scan, ui_theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_scan, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_scan, LV_SYMBOL_REFRESH "  Scan Networks");
    lv_obj_add_event_cb(btn_scan, on_scan, LV_EVENT_CLICKED, NULL);

    spinner_scan = lv_spinner_create(scan_row);
    lv_obj_set_size(spinner_scan, 32, 32);
    lv_obj_set_style_arc_color(spinner_scan, ui_theme_color_accent(),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner_scan, ui_theme_color_slider_track(),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner_scan, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner_scan, 4, LV_PART_MAIN);
    lv_obj_add_flag(spinner_scan, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * SCAN STATUS LABEL
     * ================================================================== */
    lbl_scan_status = lv_label_create(content);
    lv_obj_add_style(lbl_scan_status, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_scan_status,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_align(lbl_scan_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_scan_status, UI_HOR_RES - 2 * PAD_SIDE);
    lv_obj_set_style_margin_top(lbl_scan_status, 24, 0);
    lv_label_set_text(lbl_scan_status, "Tap Scan to find networks");

    /* ==================================================================
     * SCAN RESULTS LIST (scrollable)
     * ================================================================== */
    list_container = lv_obj_create(content);
    lv_obj_remove_style_all(list_container);
    lv_obj_set_width(list_container, UI_HOR_RES);
    lv_obj_set_style_margin_top(list_container, 8, 0);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_TRANSP, 0);
    /* Let list grow to content, parent scrolls */
    lv_obj_set_height(list_container, LV_SIZE_CONTENT);

    /* ==================================================================
     * PASSWORD DIALOG (overlay, hidden by default)
     * ================================================================== */
    dlg_password = lv_obj_create(scr_wifi);
    lv_obj_remove_style_all(dlg_password);
    lv_obj_set_size(dlg_password, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(dlg_password, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg_password, LV_OPA_80, 0);
    lv_obj_set_flex_flow(dlg_password, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg_password, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dlg_password, 0, 0);
    lv_obj_add_flag(dlg_password, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(dlg_password, LV_SCROLLBAR_MODE_OFF);

    /* Dialog card */
    lv_obj_t *dlg_card = lv_obj_create(dlg_password);
    lv_obj_remove_style_all(dlg_card);
    lv_obj_add_style(dlg_card, ui_theme_style_card(), 0);
    lv_obj_set_size(dlg_card, UI_HOR_RES - 48, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(dlg_card, 24, 0);
    lv_obj_set_flex_flow(dlg_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dlg_card, 16, 0);
    lv_obj_set_scrollbar_mode(dlg_card, LV_SCROLLBAR_MODE_OFF);

    /* SSID title */
    lbl_dlg_ssid = lv_label_create(dlg_card);
    lv_obj_add_style(lbl_dlg_ssid, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_dlg_ssid,
                                ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_dlg_ssid, "");
    lv_obj_set_width(lbl_dlg_ssid, lv_pct(100));
    lv_obj_set_style_text_align(lbl_dlg_ssid, LV_TEXT_ALIGN_CENTER, 0);

    /* Password textarea */
    ta_password = lv_textarea_create(dlg_card);
    lv_obj_set_size(ta_password, lv_pct(100), LV_SIZE_CONTENT);
    lv_textarea_set_placeholder_text(ta_password, "Password");
    lv_textarea_set_password_mode(ta_password, true);
    lv_textarea_set_one_line(ta_password, true);
    lv_obj_set_style_bg_color(ta_password, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(ta_password, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta_password,
                                ui_theme_color_text_primary(), 0);
    lv_obj_set_style_border_color(ta_password, ui_theme_color_accent(), 0);
    lv_obj_set_style_border_width(ta_password, 1, 0);
    lv_obj_set_style_radius(ta_password, 6, 0);
    lv_obj_set_style_pad_all(ta_password, 12, 0);

    /* Show password toggle row */
    lv_obj_t *pwd_toggle_row = lv_obj_create(dlg_card);
    lv_obj_remove_style_all(pwd_toggle_row);
    lv_obj_set_size(pwd_toggle_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pwd_toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pwd_toggle_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pwd_toggle_row, 10, 0);
    lv_obj_set_scrollbar_mode(pwd_toggle_row, LV_SCROLLBAR_MODE_OFF);

    sw_show_pwd = lv_switch_create(pwd_toggle_row);
    lv_obj_t *sw_show = sw_show_pwd;
    lv_obj_set_size(sw_show, 44, 24);
    lv_obj_set_style_bg_color(sw_show, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(sw_show, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sw_show, ui_theme_color_accent(), (uint32_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw_show, LV_OPA_COVER, (uint32_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_show, lv_color_hex(0x444444), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw_show, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw_show, ui_theme_color_text_primary(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw_show, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw_show, -3, LV_PART_KNOB);
    lv_obj_add_event_cb(sw_show, on_show_pwd_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lbl_show = lv_label_create(pwd_toggle_row);
    lv_obj_set_style_text_color(lbl_show, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_font(lbl_show, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_show, "Show password");

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(dlg_card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(btn_row, LV_SCROLLBAR_MODE_OFF);

    /* Cancel button — outlined pill */
    lv_obj_t *btn_cancel = lv_button_create(btn_row);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_border_opa(btn_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_cancel, 24, 0);
    lv_obj_set_style_pad_hor(btn_cancel, 32, 0);
    lv_obj_set_style_pad_ver(btn_cancel, 10, 0);
    lv_obj_set_style_shadow_width(btn_cancel, 0, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_obj_set_style_text_color(lbl_cancel, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_add_event_cb(btn_cancel, on_dlg_cancel, LV_EVENT_CLICKED, NULL);

    /* Connect button — filled gold pill */
    lv_obj_t *btn_connect = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(btn_connect, ui_theme_color_accent(), 0);
    lv_obj_set_style_bg_opa(btn_connect, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_connect, 24, 0);
    lv_obj_set_style_pad_hor(btn_connect, 32, 0);
    lv_obj_set_style_pad_ver(btn_connect, 10, 0);
    lv_obj_set_style_border_width(btn_connect, 0, 0);
    lv_obj_set_style_shadow_width(btn_connect, 0, 0);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0xC48E1F), LV_STATE_PRESSED);
    lv_obj_t *lbl_connect = lv_label_create(btn_connect);
    lv_obj_set_style_text_color(lbl_connect, ui_theme_color_bg(), 0);
    lv_obj_set_style_text_font(lbl_connect, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_connect, "Connect");
    lv_obj_add_event_cb(btn_connect, on_dlg_connect, LV_EVENT_CLICKED, NULL);

    /* Keyboard — dark audiophile style with visible key separation */
    kb_password = lv_keyboard_create(dlg_password);
    lv_obj_set_size(kb_password, UI_HOR_RES, 400);
    lv_keyboard_set_textarea(kb_password, ta_password);

    /* Keyboard background */
    lv_obj_set_style_bg_color(kb_password, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(kb_password, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb_password, 0, 0);
    lv_obj_set_style_pad_all(kb_password, 6, 0);
    lv_obj_set_style_pad_gap(kb_password, 4, 0);

    /* Individual keys (LV_PART_ITEMS) */
    lv_obj_set_style_bg_color(kb_password, lv_color_hex(0x252525), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb_password, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb_password, lv_color_hex(0x3A3A3A), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb_password, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(kb_password, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb_password, 8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb_password, ui_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb_password, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb_password, 0, LV_PART_ITEMS);

    /* Key pressed state */
    lv_obj_set_style_bg_color(kb_password, lv_color_hex(0x3A3A3A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(kb_password, lv_color_hex(0x555555),
                                  (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);

    /* Special keys (checked = Shift, Enter, Backspace, etc.) */
    lv_obj_set_style_bg_color(kb_password, lv_color_hex(0x1A1A1A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb_password, ui_theme_color_accent(),
                                (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(kb_password, lv_color_hex(0x333333),
                                  (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(kb_password, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb_password, on_kb_event, LV_EVENT_CANCEL, NULL);

    return scr_wifi;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh WiFi scan data)
 * ----------------------------------------------------------------------- */

void ui_wifi_update(const ui_wifi_scan_data_t *data)
{
    if (!scr_wifi) return;

    /* -- Connection card -- */
    if (data->connected_ssid[0] != '\0' &&
        (data->state == UI_WIFI_CONNECTED || data->state == UI_WIFI_IDLE)) {
        lv_obj_clear_flag(card_connected, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_conn_ssid, data->connected_ssid);
        char rssi_buf[24];
        lv_snprintf(rssi_buf, sizeof(rssi_buf), LV_SYMBOL_WIFI " %d dBm",
                    data->connected_rssi);
        lv_label_set_text(lbl_conn_rssi, rssi_buf);
    } else {
        lv_obj_add_flag(card_connected, LV_OBJ_FLAG_HIDDEN);
    }

    /* -- Spinner + status -- */
    if (data->state == UI_WIFI_SCANNING) {
        lv_obj_clear_flag(spinner_scan, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_scan_status, "Scanning...");
        lv_obj_set_style_text_color(lbl_scan_status,
                                    ui_theme_color_text_secondary(), 0);
        lv_obj_clear_flag(lbl_scan_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(spinner_scan, LV_OBJ_FLAG_HIDDEN);
        if (data->ap_count == 0) {
            lv_label_set_text(lbl_scan_status,
                              "No networks found. Tap Scan.");
            lv_obj_set_style_text_color(lbl_scan_status,
                                        ui_theme_color_text_secondary(), 0);
            lv_obj_clear_flag(lbl_scan_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_scan_status, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* -- Error state -- */
    if (data->state == UI_WIFI_ERROR && data->error_msg[0]) {
        lv_label_set_text(lbl_scan_status, data->error_msg);
        lv_obj_set_style_text_color(lbl_scan_status,
                                    lv_color_hex(0xFF4444), 0);
        lv_obj_clear_flag(lbl_scan_status, LV_OBJ_FLAG_HIDDEN);
    }

    /* -- Toast on state transitions or SSID change -- */
    if (!s_toast_seeded) {
        /* First update: seed tracking state without firing toasts */
        s_toast_seeded = true;
        s_prev_toast_state = data->state;
        strncpy(s_prev_toast_ssid, data->connected_ssid,
                sizeof(s_prev_toast_ssid) - 1);
        s_prev_toast_ssid[sizeof(s_prev_toast_ssid) - 1] = '\0';
    } else {
        bool state_changed = (data->state != s_prev_toast_state);
        bool ssid_changed  = (data->state == UI_WIFI_CONNECTED &&
                              strcmp(data->connected_ssid, s_prev_toast_ssid) != 0);

        if (state_changed || ssid_changed) {
            if (data->state == UI_WIFI_CONNECTED &&
                (s_prev_toast_state != UI_WIFI_CONNECTED || ssid_changed)) {
                char tbuf[64];
                lv_snprintf(tbuf, sizeof(tbuf), "Connected to %s",
                            data->connected_ssid);
                ui_toast_show(UI_TOAST_SUCCESS, tbuf, 3000);
            }
            if (data->state == UI_WIFI_ERROR &&
                s_prev_toast_state != UI_WIFI_ERROR) {
                ui_toast_show(UI_TOAST_ERROR,
                              data->error_msg[0] ? data->error_msg
                                                 : "Connection failed",
                              4000);
            }
            s_prev_toast_state = data->state;
            strncpy(s_prev_toast_ssid, data->connected_ssid,
                    sizeof(s_prev_toast_ssid) - 1);
            s_prev_toast_ssid[sizeof(s_prev_toast_ssid) - 1] = '\0';
        }
    }

    /* -- Rebuild AP list if changed -- */
    if (s_last_ap_count != data->ap_count || s_last_state != data->state) {
        s_last_ap_count = data->ap_count;
        s_last_state = data->state;
        rebuild_ap_list(data);
    }
}

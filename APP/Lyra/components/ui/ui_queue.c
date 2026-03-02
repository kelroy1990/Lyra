/*
 * ui_queue.c — Playback queue sub-screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  Queue        🔀 Shuffle  🗑  │  Header (48px)
 *   ├──────────────────────────────────┤
 *   │                                  │
 *   │  NOW PLAYING                     │  Section (gold current track)
 *   │  ─────────────────────────────   │
 *   │  ▶ 3. Wish You Were Here  5:34  │  Gold highlight
 *   │  ─────────────────────────────   │
 *   │                                  │
 *   │  UP NEXT                         │  Section
 *   │  ─────────────────────────────   │
 *   │    4. Welcome to the Machine 7:26│
 *   │  ─────────────────────────────   │
 *   │    5. Have a Cigar         5:08  │
 *   │           ... (scrollable) ...   │
 *   │                                  │
 *   │  PLAYED                          │  Section (dimmed text)
 *   │  ─────────────────────────────   │
 *   │    1. Shine On You...     13:38  │  Gray text
 *   │  ─────────────────────────────   │
 *   │    2. Welcome to...        7:26  │
 *   │                                  │
 *   │  8 tracks · 45:23 total          │  Footer
 *   └──────────────────────────────────┘
 *
 * No bottom nav bar (sub-screen — back returns to Now Playing).
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_queue;

/* Header icons */
static lv_obj_t *btn_shuffle;
static lv_obj_t *lbl_shuffle;

/* Content sections */
static lv_obj_t *list_container;
static lv_obj_t *lbl_footer;

/* Empty state */
static lv_obj_t *empty_container;

/* Jump index stored as user_data for track click events */
static int s_jump_idx[UI_QUEUE_MAX_ITEMS];

/* Dirty check */
static int s_last_count = -1;
static int s_last_current = -1;

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_navigate_to(UI_SCREEN_NOW_PLAYING);
}

static void on_shuffle_toggle(lv_event_t *e)
{
    (void)e;
    ui_cmd_queue_shuffle_toggle();
}

static void on_clear(lv_event_t *e)
{
    (void)e;
    ui_cmd_queue_clear();
    s_last_count = -1;  /* Force rebuild */
    ui_toast_show(UI_TOAST_INFO, "Queue cleared", 2000);
}

static void on_track_click(lv_event_t *e)
{
    int *idx = (int *)lv_event_get_user_data(e);
    if (idx) {
        ui_cmd_queue_jump(*idx);
        s_last_count = -1;  /* Force rebuild */
    }
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static lv_obj_t *create_section(lv_obj_t *parent, const char *text,
                                int32_t margin_top)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_add_style(lbl, ui_theme_style_section_header(), 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, UI_HOR_RES);
    lv_obj_set_style_margin_top(lbl, margin_top, 0);
    return lbl;
}

static void create_track_row(lv_obj_t *parent, int index,
                             const ui_queue_item_t *item,
                             bool is_current, bool is_played)
{
    lv_obj_t *row = lv_obj_create(parent);
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

    /* Determine text color */
    lv_color_t text_col;
    if (is_current)
        text_col = ui_theme_color_accent();
    else if (is_played)
        text_col = ui_theme_color_text_secondary();
    else
        text_col = ui_theme_color_text_primary();

    /* Play indicator or track number */
    lv_obj_t *lbl_num = lv_label_create(row);
    lv_obj_add_style(lbl_num, ui_theme_style_info(), 0);
    lv_obj_set_width(lbl_num, 36);
    lv_obj_set_style_text_align(lbl_num, LV_TEXT_ALIGN_RIGHT, 0);
    if (is_current) {
        lv_label_set_text(lbl_num, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(lbl_num, ui_theme_color_accent(), 0);
    } else {
        char num_buf[8];
        lv_snprintf(num_buf, sizeof(num_buf), "%d", index + 1);
        lv_label_set_text(lbl_num, num_buf);
        lv_obj_set_style_text_color(lbl_num, text_col, 0);
    }

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(row);
    lv_obj_add_style(lbl_title, ui_theme_style_list_item(), 0);
    lv_label_set_text(lbl_title, item->title);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl_title, 1);
    lv_obj_set_style_text_color(lbl_title, text_col, 0);

    /* Duration */
    if (item->duration_ms > 0) {
        char dur_buf[16];
        ui_fmt_time(dur_buf, sizeof(dur_buf), item->duration_ms);
        lv_obj_t *lbl_dur = lv_label_create(row);
        lv_obj_add_style(lbl_dur, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_dur,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(lbl_dur, dur_buf);
    }

    /* Store index for jump event */
    s_jump_idx[index] = index;
    lv_obj_add_event_cb(row, on_track_click, LV_EVENT_CLICKED,
                        &s_jump_idx[index]);
}

/* -----------------------------------------------------------------------
 * Rebuild queue list
 * ----------------------------------------------------------------------- */

static void rebuild_queue(const ui_queue_data_t *queue)
{
    lv_obj_clean(list_container);
    lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);

    if (queue->count == 0) {
        /* Show empty state, hide list */
        lv_obj_clear_flag(empty_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_footer, "");
        return;
    }

    lv_obj_add_flag(empty_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);

    int cur = queue->current_index;

    /* NOW PLAYING section (current track) */
    if (cur >= 0 && cur < queue->count) {
        create_section(list_container, "NOW PLAYING", 8);
        create_track_row(list_container, cur, &queue->items[cur],
                         true, false);
    }

    /* UP NEXT section (tracks after current) */
    bool has_up_next = false;
    for (int i = cur + 1; i < queue->count; i++) {
        if (!has_up_next) {
            create_section(list_container, "UP NEXT", 16);
            has_up_next = true;
        }
        create_track_row(list_container, i, &queue->items[i],
                         false, false);

        /* Divider between up-next items */
        if (i < queue->count - 1) {
            lv_obj_t *div = lv_obj_create(list_container);
            lv_obj_remove_style_all(div);
            lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
            lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
            lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
        }
    }

    /* PLAYED section (tracks before current, dimmed) */
    bool has_played = false;
    for (int i = 0; i < cur; i++) {
        if (!has_played) {
            create_section(list_container, "PLAYED", 16);
            has_played = true;
        }
        create_track_row(list_container, i, &queue->items[i],
                         false, true);

        /* Divider between played items */
        if (i < cur - 1) {
            lv_obj_t *div = lv_obj_create(list_container);
            lv_obj_remove_style_all(div);
            lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
            lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
            lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
        }
    }

    /* Footer: track count + total duration */
    uint32_t total_ms = 0;
    for (int i = 0; i < queue->count; i++)
        total_ms += queue->items[i].duration_ms;

    char footer_buf[48];
    char time_buf[16];
    ui_fmt_time(time_buf, sizeof(time_buf), total_ms);
    lv_snprintf(footer_buf, sizeof(footer_buf), "%d tracks  %s total",
                queue->count, time_buf);
    lv_label_set_text(lbl_footer, footer_buf);
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_queue_create(void)
{
    scr_queue = lv_obj_create(NULL);
    lv_obj_add_style(scr_queue, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_queue, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main_cont = lv_obj_create(scr_queue);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_set_style_pad_gap(main_cont, 0, 0);
    lv_obj_set_scrollbar_mode(main_cont, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main_cont);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(header, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 10, 0);

    /* Back button */
    lv_obj_t *btn_back = lv_button_create(header);
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
    lv_label_set_text(lbl_title, "Queue");
    lv_obj_set_flex_grow(lbl_title, 1);

    /* Shuffle toggle button */
    btn_shuffle = lv_button_create(header);
    lv_obj_add_style(btn_shuffle, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_shuffle, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_shuffle, 8, 0);
    lbl_shuffle = lv_label_create(btn_shuffle);
    lv_label_set_text(lbl_shuffle, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(lbl_shuffle,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_add_event_cb(btn_shuffle, on_shuffle_toggle,
                        LV_EVENT_CLICKED, NULL);

    /* Clear queue button (trash icon) */
    lv_obj_t *btn_clear = lv_button_create(header);
    lv_obj_add_style(btn_clear, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_clear, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_clear, 8, 0);
    lv_obj_t *lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(lbl_clear,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_add_event_cb(btn_clear, on_clear, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    lv_obj_t *content = lv_obj_create(main_cont);
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

    /* Themed scrollbar */
    lv_obj_set_style_bg_color(content, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, (lv_opa_t)120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);

    /* ==================================================================
     * EMPTY STATE (centered, hidden when queue has items)
     * ================================================================== */
    empty_container = lv_obj_create(content);
    lv_obj_remove_style_all(empty_container);
    lv_obj_set_size(empty_container, UI_HOR_RES, 300);
    lv_obj_set_flex_flow(empty_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(empty_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(empty_container, 16, 0);
    lv_obj_set_scrollbar_mode(empty_container, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_empty_icon = lv_label_create(empty_container);
    lv_obj_add_style(lbl_empty_icon, ui_theme_style_title(), 0);
    lv_obj_set_style_text_color(lbl_empty_icon,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_empty_icon, LV_SYMBOL_LIST);

    lv_obj_t *lbl_empty_text = lv_label_create(empty_container);
    lv_obj_add_style(lbl_empty_text, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_empty_text,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_empty_text, "Queue is empty");

    lv_obj_t *lbl_empty_hint = lv_label_create(empty_container);
    lv_obj_add_style(lbl_empty_hint, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_empty_hint,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_align(lbl_empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_empty_hint, UI_HOR_RES - 80);
    lv_label_set_text(lbl_empty_hint,
                      "Play a file from the browser to\nstart building your queue");

    /* ==================================================================
     * TRACK LIST (dynamically populated)
     * ================================================================== */
    list_container = lv_obj_create(content);
    lv_obj_remove_style_all(list_container);
    lv_obj_set_width(list_container, UI_HOR_RES);
    lv_obj_set_height(list_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_TRANSP, 0);

    /* ==================================================================
     * FOOTER
     * ================================================================== */
    lbl_footer = lv_label_create(content);
    lv_obj_add_style(lbl_footer, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_footer,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_align(lbl_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_footer, UI_HOR_RES);
    lv_obj_set_style_margin_top(lbl_footer, 16, 0);
    lv_obj_set_style_margin_bottom(lbl_footer, 16, 0);
    lv_label_set_text(lbl_footer, "");

    /* Init dirty check */
    s_last_count = -1;
    s_last_current = -1;

    return scr_queue;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_queue_update(const ui_queue_data_t *queue,
                     const ui_now_playing_t *np)
{
    if (!scr_queue) return;
    (void)np;  /* Reserved for future use (e.g. highlight based on playing) */

    /* Shuffle icon color */
    if (queue->shuffle_enabled)
        lv_obj_set_style_text_color(lbl_shuffle, ui_theme_color_accent(), 0);
    else
        lv_obj_set_style_text_color(lbl_shuffle,
                                    ui_theme_color_text_secondary(), 0);

    /* Rebuild list if dirty */
    if (s_last_count != queue->count ||
        s_last_current != queue->current_index) {
        s_last_count = queue->count;
        s_last_current = queue->current_index;
        rebuild_queue(queue);
    }
}

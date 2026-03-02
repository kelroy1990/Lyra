/*
 * ui_toast.c — Toast notification manager for Lyra UI.
 *
 * Renders ephemeral notifications on lv_layer_top() so they persist
 * across screen transitions.  Single toast visible at a time; a new
 * toast replaces the current one.
 *
 * Animation: slide down from top + fade in on show,
 *            slide up   + fade out on dismiss.
 */

#include "ui_internal.h"
#include <string.h>

/* Layout */
#define TOAST_WIDTH      (UI_HOR_RES - 48)   /* 672px */
#define TOAST_X          24                   /* centered: (720-672)/2 */
#define TOAST_Y_HIDDEN   (-60)               /* off-screen above */
#define TOAST_Y_VISIBLE  64                  /* below status bar */
#define TOAST_RADIUS     12
#define TOAST_PAD_H      16
#define TOAST_PAD_V      12
#define TOAST_GAP        14

/* Animation timing (ms) */
#define ANIM_SHOW_MS     300
#define ANIM_HIDE_MS     250

/* Colors */
#define COL_TOAST_BG     lv_color_hex(0x1E1E1E)
#define COL_TOAST_BORDER lv_color_hex(0x333333)
#define COL_INFO         lv_color_hex(0xE0A526)   /* gold    */
#define COL_SUCCESS      lv_color_hex(0x4CAF50)   /* green   */
#define COL_WARNING      lv_color_hex(0xFFA726)   /* amber   */
#define COL_ERROR        lv_color_hex(0xFF5252)   /* red     */

/* -----------------------------------------------------------------------
 * Statics
 * ----------------------------------------------------------------------- */

static lv_obj_t   *s_container;      /* card on lv_layer_top() */
static lv_obj_t   *s_icon;           /* symbol label           */
static lv_obj_t   *s_label;          /* message label          */
static lv_timer_t  *s_dismiss_timer; /* auto-dismiss timer     */
static bool         s_visible;       /* currently showing?     */

/* -----------------------------------------------------------------------
 * Helpers: icon & color per type
 * ----------------------------------------------------------------------- */

static const char *toast_icon(ui_toast_type_t type)
{
    switch (type) {
        case UI_TOAST_SUCCESS: return LV_SYMBOL_OK;
        case UI_TOAST_WARNING: return LV_SYMBOL_WARNING;
        case UI_TOAST_ERROR:   return LV_SYMBOL_CLOSE;
        default:               return LV_SYMBOL_BELL;
    }
}

static lv_color_t toast_color(ui_toast_type_t type)
{
    switch (type) {
        case UI_TOAST_SUCCESS: return COL_SUCCESS;
        case UI_TOAST_WARNING: return COL_WARNING;
        case UI_TOAST_ERROR:   return COL_ERROR;
        default:               return COL_INFO;
    }
}

/* -----------------------------------------------------------------------
 * Lazy creation (first call builds the widget tree)
 * ----------------------------------------------------------------------- */

static void ensure_created(void)
{
    if (s_container) return;

    lv_obj_t *layer = lv_layer_top();

    s_container = lv_obj_create(layer);
    lv_obj_remove_style_all(s_container);

    /* Card appearance */
    lv_obj_set_size(s_container, TOAST_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_container, COL_TOAST_BG, 0);
    lv_obj_set_style_bg_opa(s_container, (lv_opa_t)242, 0);  /* ~95% */
    lv_obj_set_style_radius(s_container, TOAST_RADIUS, 0);
    lv_obj_set_style_border_color(s_container, COL_TOAST_BORDER, 0);
    lv_obj_set_style_border_width(s_container, 1, 0);
    lv_obj_set_style_border_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_container, 16, 0);
    lv_obj_set_style_shadow_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_container, (lv_opa_t)80, 0);

    /* Positioning */
    lv_obj_set_pos(s_container, TOAST_X, TOAST_Y_HIDDEN);

    /* Flex row for icon + message */
    lv_obj_set_flex_flow(s_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_container, TOAST_PAD_H, 0);
    lv_obj_set_style_pad_ver(s_container, TOAST_PAD_V, 0);
    lv_obj_set_style_pad_gap(s_container, TOAST_GAP, 0);
    lv_obj_set_scrollbar_mode(s_container, LV_SCROLLBAR_MODE_OFF);

    /* Icon */
    s_icon = lv_label_create(s_container);
    lv_obj_set_style_text_font(s_icon, &lv_font_montserrat_16, 0);

    /* Message */
    s_label = lv_label_create(s_container);
    lv_obj_set_style_text_color(s_label, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_label, 1);

    /* Start hidden */
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/* -----------------------------------------------------------------------
 * Animation callbacks
 * ----------------------------------------------------------------------- */

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void on_hide_complete(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

static void animate_show(void)
{
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;

    /* Slide Y: hidden → visible */
    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, s_container);
    lv_anim_set_values(&a_y, TOAST_Y_HIDDEN, TOAST_Y_VISIBLE);
    lv_anim_set_duration(&a_y, ANIM_SHOW_MS);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_y, anim_y_cb);
    lv_anim_start(&a_y);

    /* Fade in: 0 → 255 */
    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, s_container);
    lv_anim_set_values(&a_opa, 0, 255);
    lv_anim_set_duration(&a_opa, ANIM_SHOW_MS);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_start(&a_opa);
}

static void animate_hide(void)
{
    if (!s_visible) return;

    /* Slide Y: visible → hidden */
    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, s_container);
    lv_anim_set_values(&a_y, TOAST_Y_VISIBLE, TOAST_Y_HIDDEN);
    lv_anim_set_duration(&a_y, ANIM_HIDE_MS);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a_y, anim_y_cb);
    lv_anim_set_ready_cb(&a_y, on_hide_complete);
    lv_anim_start(&a_y);

    /* Fade out: 255 → 0 */
    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, s_container);
    lv_anim_set_values(&a_opa, 255, 0);
    lv_anim_set_duration(&a_opa, ANIM_HIDE_MS);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_start(&a_opa);
}

/* -----------------------------------------------------------------------
 * Timer callback (auto-dismiss)
 * ----------------------------------------------------------------------- */

static void dismiss_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_dismiss_timer = NULL;
    animate_hide();
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void ui_toast_show(ui_toast_type_t type, const char *message,
                   uint32_t duration_ms)
{
    ensure_created();

    /* Cancel any pending dismiss */
    if (s_dismiss_timer) {
        lv_timer_delete(s_dismiss_timer);
        s_dismiss_timer = NULL;
    }

    /* Cancel running animations on the container */
    lv_anim_delete(s_container, anim_y_cb);
    lv_anim_delete(s_container, anim_opa_cb);

    /* Update icon */
    lv_color_t col = toast_color(type);
    lv_label_set_text(s_icon, toast_icon(type));
    lv_obj_set_style_text_color(s_icon, col, 0);

    /* Update message */
    lv_label_set_text(s_label, message);

    /* Tint left border with type color */
    lv_obj_set_style_border_color(s_container, col, 0);
    lv_obj_set_style_border_side(s_container, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_container, 3, 0);

    /* Animate in */
    animate_show();

    /* Schedule auto-dismiss */
    if (duration_ms > 0) {
        s_dismiss_timer = lv_timer_create(dismiss_timer_cb, duration_ms, NULL);
        lv_timer_set_repeat_count(s_dismiss_timer, 1);
    }
}

void ui_toast_dismiss(void)
{
    if (!s_container || !s_visible) return;

    if (s_dismiss_timer) {
        lv_timer_delete(s_dismiss_timer);
        s_dismiss_timer = NULL;
    }

    animate_hide();
}

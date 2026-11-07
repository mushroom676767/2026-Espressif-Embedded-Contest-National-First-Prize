/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_theme.h"
#include "esp_log.h"

#include <stdbool.h>

static const char *TAG = "ui_theme";

LV_FONT_DECLARE(register_cjk_font_20);
LV_FONT_DECLARE(ui_cjk_font_22);
LV_FONT_DECLARE(ui_cjk_font_26);
LV_FONT_DECLARE(ui_cjk_font_32);

// ============================================================================
// Global Styles (reusable across all screens)
// ============================================================================
static lv_style_t style_bg;
static lv_style_t style_panel;
static lv_style_t style_panel_pressed;
static lv_style_t style_btn_primary;
static lv_style_t style_btn_primary_pressed;
static lv_style_t style_btn_primary_large;
static lv_style_t style_btn_primary_large_pressed;
static lv_style_t style_btn_secondary;
static lv_style_t style_btn_secondary_pressed;
static lv_style_t style_btn_secondary_large;
static lv_style_t style_btn_secondary_large_pressed;
static lv_style_t style_btn_danger;
static lv_style_t style_btn_danger_pressed;
static lv_style_t style_btn_danger_large;
static lv_style_t style_btn_danger_large_pressed;
static lv_style_t style_title;
static lv_style_t style_subtitle;
static lv_style_t style_body;
static lv_style_t style_body_bold;
static lv_style_t style_caption;
static lv_style_t style_card;
static lv_style_t style_status_badge;
static lv_style_t style_divider;

static lv_font_t font_title;
static lv_font_t font_subtitle;
static lv_font_t font_body;
static lv_font_t font_body_bold;
static lv_font_t font_caption;
static lv_font_t font_status;
static lv_font_t font_time;
static lv_font_t font_count;
static bool fonts_ready;

// ============================================================================
// Helper Functions
// ============================================================================

static void init_font_fallbacks(void)
{
    if (fonts_ready) {
        return;
    }

    font_title = lv_font_montserrat_34;
    font_subtitle = lv_font_montserrat_28;
    font_body = lv_font_montserrat_22;
    font_body_bold = lv_font_montserrat_22;
    font_caption = lv_font_montserrat_20;
    font_status = lv_font_montserrat_20;
    font_time = lv_font_montserrat_48;
    font_count = lv_font_montserrat_40;

    font_title.fallback = &ui_cjk_font_32;
    font_subtitle.fallback = &ui_cjk_font_26;
    font_body.fallback = &ui_cjk_font_22;
    font_body_bold.fallback = &ui_cjk_font_22;
    font_caption.fallback = &register_cjk_font_20;
    font_status.fallback = &register_cjk_font_20;
    font_time.fallback = &ui_cjk_font_32;
    font_count.fallback = &ui_cjk_font_32;

    fonts_ready = true;
}

const lv_font_t *ui_font_title(void)
{
    init_font_fallbacks();
    return &font_title;
}

const lv_font_t *ui_font_subtitle(void)
{
    init_font_fallbacks();
    return &font_subtitle;
}

const lv_font_t *ui_font_body(void)
{
    init_font_fallbacks();
    return &font_body;
}

const lv_font_t *ui_font_body_bold(void)
{
    init_font_fallbacks();
    return &font_body_bold;
}

const lv_font_t *ui_font_caption(void)
{
    init_font_fallbacks();
    return &font_caption;
}

const lv_font_t *ui_font_status(void)
{
    init_font_fallbacks();
    return &font_status;
}

const lv_font_t *ui_font_time(void)
{
    init_font_fallbacks();
    return &font_time;
}

const lv_font_t *ui_font_count(void)
{
    init_font_fallbacks();
    return &font_count;
}

static void init_style_bg(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, UI_COLOR_BG_DARK);
    lv_style_set_text_color(&style_bg, UI_COLOR_TEXT_PRIMARY);
    lv_style_set_radius(&style_bg, 0);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_pad_all(&style_bg, 0);
}

static void init_style_panel(void)
{
    // Normal panel
    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, UI_COLOR_BG_PANEL);
    lv_style_set_radius(&style_panel, UI_RADIUS_MD);
    lv_style_set_border_width(&style_panel, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_panel, UI_COLOR_BORDER);
    lv_style_set_pad_all(&style_panel, UI_PADDING_MD);
    lv_style_set_shadow_width(&style_panel, 0);

    // Pressed panel
    lv_style_init(&style_panel_pressed);
    lv_style_set_bg_color(&style_panel_pressed, lv_color_darken(UI_COLOR_BG_PANEL, 20));
    lv_style_set_radius(&style_panel_pressed, UI_RADIUS_MD);
    lv_style_set_border_width(&style_panel_pressed, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_panel_pressed, UI_COLOR_PRIMARY);
    lv_style_set_pad_all(&style_panel_pressed, UI_PADDING_MD);
}

static void init_style_btn_primary(void)
{
    // Normal state
    lv_style_init(&style_btn_primary);
    lv_style_set_bg_color(&style_btn_primary, UI_COLOR_BTN_PRIMARY_BG);
    lv_style_set_text_color(&style_btn_primary, UI_COLOR_BTN_PRIMARY_TEXT);
    lv_style_set_radius(&style_btn_primary, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_primary, 0);
    lv_style_set_pad_hor(&style_btn_primary, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_primary, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_primary, UI_FONT_BODY_BOLD);
    lv_style_set_shadow_width(&style_btn_primary, 4);
    lv_style_set_shadow_color(&style_btn_primary, UI_COLOR_PRIMARY_DIM);

    // Pressed state
    lv_style_init(&style_btn_primary_pressed);
    lv_style_set_bg_color(&style_btn_primary_pressed, lv_color_darken(UI_COLOR_BTN_PRIMARY_BG, 20));
    lv_style_set_text_color(&style_btn_primary_pressed, UI_COLOR_BTN_PRIMARY_TEXT);
    lv_style_set_radius(&style_btn_primary_pressed, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_primary_pressed, 0);
    lv_style_set_pad_hor(&style_btn_primary_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_primary_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_primary_pressed, UI_FONT_BODY_BOLD);

    lv_style_init(&style_btn_primary_large);
    lv_style_set_bg_color(&style_btn_primary_large, UI_COLOR_BTN_PRIMARY_BG);
    lv_style_set_text_color(&style_btn_primary_large, UI_COLOR_BTN_PRIMARY_TEXT);
    lv_style_set_radius(&style_btn_primary_large, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_primary_large, 0);
    lv_style_set_pad_hor(&style_btn_primary_large, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_primary_large, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_primary_large, UI_FONT_BODY_BOLD);
    lv_style_set_shadow_width(&style_btn_primary_large, 8);
    lv_style_set_shadow_color(&style_btn_primary_large, UI_COLOR_PRIMARY_DIM);

    lv_style_init(&style_btn_primary_large_pressed);
    lv_style_set_bg_color(&style_btn_primary_large_pressed, lv_color_darken(UI_COLOR_BTN_PRIMARY_BG, 28));
    lv_style_set_text_color(&style_btn_primary_large_pressed, UI_COLOR_BTN_PRIMARY_TEXT);
    lv_style_set_radius(&style_btn_primary_large_pressed, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_primary_large_pressed, 0);
    lv_style_set_pad_hor(&style_btn_primary_large_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_primary_large_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_primary_large_pressed, UI_FONT_BODY_BOLD);
}

static void init_style_btn_secondary(void)
{
    // Normal state
    lv_style_init(&style_btn_secondary);
    lv_style_set_bg_color(&style_btn_secondary, UI_COLOR_BTN_SECONDARY_BG);
    lv_style_set_text_color(&style_btn_secondary, UI_COLOR_BTN_SECONDARY_TEXT);
    lv_style_set_radius(&style_btn_secondary, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_secondary, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_btn_secondary, UI_COLOR_BORDER);
    lv_style_set_pad_hor(&style_btn_secondary, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_secondary, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_secondary, UI_FONT_BODY);

    // Pressed state
    lv_style_init(&style_btn_secondary_pressed);
    lv_style_set_bg_color(&style_btn_secondary_pressed, lv_color_darken(UI_COLOR_BTN_SECONDARY_BG, 20));
    lv_style_set_text_color(&style_btn_secondary_pressed, UI_COLOR_BTN_SECONDARY_TEXT);
    lv_style_set_radius(&style_btn_secondary_pressed, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_secondary_pressed, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_btn_secondary_pressed, UI_COLOR_PRIMARY);
    lv_style_set_pad_hor(&style_btn_secondary_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_secondary_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_secondary_pressed, UI_FONT_BODY);

    lv_style_init(&style_btn_secondary_large);
    lv_style_set_bg_color(&style_btn_secondary_large, UI_COLOR_BTN_SECONDARY_BG);
    lv_style_set_text_color(&style_btn_secondary_large, UI_COLOR_BTN_SECONDARY_TEXT);
    lv_style_set_radius(&style_btn_secondary_large, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_secondary_large, UI_BORDER_WIDTH_THICK);
    lv_style_set_border_color(&style_btn_secondary_large, UI_COLOR_BORDER);
    lv_style_set_pad_hor(&style_btn_secondary_large, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_secondary_large, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_secondary_large, UI_FONT_BODY_BOLD);
    lv_style_set_shadow_width(&style_btn_secondary_large, 4);
    lv_style_set_shadow_color(&style_btn_secondary_large, lv_color_hex(0x000000));

    lv_style_init(&style_btn_secondary_large_pressed);
    lv_style_set_bg_color(&style_btn_secondary_large_pressed, lv_color_darken(UI_COLOR_BTN_SECONDARY_BG, 20));
    lv_style_set_text_color(&style_btn_secondary_large_pressed, UI_COLOR_BTN_SECONDARY_TEXT);
    lv_style_set_radius(&style_btn_secondary_large_pressed, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_secondary_large_pressed, UI_BORDER_WIDTH_THICK);
    lv_style_set_border_color(&style_btn_secondary_large_pressed, UI_COLOR_PRIMARY);
    lv_style_set_pad_hor(&style_btn_secondary_large_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_secondary_large_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_secondary_large_pressed, UI_FONT_BODY_BOLD);
}

static void init_style_btn_danger(void)
{
    // Normal state
    lv_style_init(&style_btn_danger);
    lv_style_set_bg_color(&style_btn_danger, UI_COLOR_BTN_DANGER_BG);
    lv_style_set_text_color(&style_btn_danger, UI_COLOR_BTN_DANGER_TEXT);
    lv_style_set_radius(&style_btn_danger, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_danger, 0);
    lv_style_set_pad_hor(&style_btn_danger, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_danger, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_danger, UI_FONT_BODY_BOLD);
    lv_style_set_shadow_width(&style_btn_danger, 4);
    lv_style_set_shadow_color(&style_btn_danger, UI_COLOR_DANGER_DIM);

    // Pressed state
    lv_style_init(&style_btn_danger_pressed);
    lv_style_set_bg_color(&style_btn_danger_pressed, lv_color_darken(UI_COLOR_BTN_DANGER_BG, 20));
    lv_style_set_text_color(&style_btn_danger_pressed, UI_COLOR_BTN_DANGER_TEXT);
    lv_style_set_radius(&style_btn_danger_pressed, UI_RADIUS_MD);
    lv_style_set_border_width(&style_btn_danger_pressed, 0);
    lv_style_set_pad_hor(&style_btn_danger_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_danger_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_danger_pressed, UI_FONT_BODY_BOLD);

    lv_style_init(&style_btn_danger_large);
    lv_style_set_bg_color(&style_btn_danger_large, UI_COLOR_BTN_DANGER_BG);
    lv_style_set_text_color(&style_btn_danger_large, UI_COLOR_BTN_DANGER_TEXT);
    lv_style_set_radius(&style_btn_danger_large, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_danger_large, 0);
    lv_style_set_pad_hor(&style_btn_danger_large, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_danger_large, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_danger_large, UI_FONT_BODY_BOLD);
    lv_style_set_shadow_width(&style_btn_danger_large, 8);
    lv_style_set_shadow_color(&style_btn_danger_large, UI_COLOR_DANGER_DIM);

    lv_style_init(&style_btn_danger_large_pressed);
    lv_style_set_bg_color(&style_btn_danger_large_pressed, lv_color_darken(UI_COLOR_BTN_DANGER_BG, 25));
    lv_style_set_text_color(&style_btn_danger_large_pressed, UI_COLOR_BTN_DANGER_TEXT);
    lv_style_set_radius(&style_btn_danger_large_pressed, UI_RADIUS_LG);
    lv_style_set_border_width(&style_btn_danger_large_pressed, 0);
    lv_style_set_pad_hor(&style_btn_danger_large_pressed, UI_PADDING_LG);
    lv_style_set_pad_ver(&style_btn_danger_large_pressed, UI_PADDING_MD);
    lv_style_set_text_font(&style_btn_danger_large_pressed, UI_FONT_BODY_BOLD);
}

static void init_style_title(void)
{
    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, UI_COLOR_TEXT_PRIMARY);
    lv_style_set_text_font(&style_title, UI_FONT_TITLE);
    lv_style_set_text_align(&style_title, LV_TEXT_ALIGN_CENTER);
}

static void init_style_subtitle(void)
{
    lv_style_init(&style_subtitle);
    lv_style_set_text_color(&style_subtitle, UI_COLOR_TEXT_SECOND);
    lv_style_set_text_font(&style_subtitle, UI_FONT_SUBTITLE);
    lv_style_set_text_align(&style_subtitle, LV_TEXT_ALIGN_CENTER);
}

static void init_style_body(void)
{
    lv_style_init(&style_body);
    lv_style_set_text_color(&style_body, UI_COLOR_TEXT_PRIMARY);
    lv_style_set_text_font(&style_body, UI_FONT_BODY);
    lv_style_set_text_align(&style_body, LV_TEXT_ALIGN_LEFT);
}

static void init_style_body_bold(void)
{
    lv_style_init(&style_body_bold);
    lv_style_set_text_color(&style_body_bold, UI_COLOR_TEXT_PRIMARY);
    lv_style_set_text_font(&style_body_bold, UI_FONT_BODY_BOLD);
    lv_style_set_text_align(&style_body_bold, LV_TEXT_ALIGN_LEFT);
}

static void init_style_caption(void)
{
    lv_style_init(&style_caption);
    lv_style_set_text_color(&style_caption, UI_COLOR_TEXT_HINT);
    lv_style_set_text_font(&style_caption, UI_FONT_CAPTION);
    lv_style_set_text_align(&style_caption, LV_TEXT_ALIGN_LEFT);
}

static void init_style_card(void)
{
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, UI_COLOR_BG_CARD);
    lv_style_set_radius(&style_card, UI_RADIUS_LG);
    lv_style_set_border_width(&style_card, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_card, UI_COLOR_BORDER);
    lv_style_set_pad_all(&style_card, UI_PADDING_LG);
    lv_style_set_shadow_width(&style_card, 8);
    lv_style_set_shadow_color(&style_card, lv_color_hex(0x00000040));

    lv_style_init(&style_status_badge);
    lv_style_set_bg_color(&style_status_badge, UI_COLOR_BG_CARD);
    lv_style_set_bg_opa(&style_status_badge, LV_OPA_COVER);
    lv_style_set_text_color(&style_status_badge, UI_COLOR_TEXT_PRIMARY);
    lv_style_set_text_font(&style_status_badge, UI_FONT_STATUS);
    lv_style_set_radius(&style_status_badge, UI_RADIUS_MD);
    lv_style_set_border_width(&style_status_badge, UI_BORDER_WIDTH);
    lv_style_set_border_color(&style_status_badge, UI_COLOR_BORDER);
    lv_style_set_pad_hor(&style_status_badge, UI_PADDING_MD);
    lv_style_set_pad_ver(&style_status_badge, UI_PADDING_SM);
}

static void init_style_divider(void)
{
    lv_style_init(&style_divider);
    lv_style_set_bg_color(&style_divider, UI_COLOR_DIVIDER);
    lv_style_set_radius(&style_divider, 0);
    lv_style_set_border_width(&style_divider, 0);
}

// ============================================================================
// Public API Implementation
// ============================================================================

void ui_theme_init(void)
{
    ESP_LOGI(TAG, "Initializing UI theme...");

    init_font_fallbacks();
    init_style_bg();
    init_style_panel();
    init_style_btn_primary();
    init_style_btn_secondary();
    init_style_btn_danger();
    init_style_title();
    init_style_subtitle();
    init_style_body();
    init_style_body_bold();
    init_style_caption();
    init_style_card();
    init_style_divider();

    ESP_LOGI(TAG, "UI theme initialized");
}

const lv_style_t *ui_style_get_bg(void)
{
    return &style_bg;
}

const lv_style_t *ui_style_get_panel(void)
{
    return &style_panel;
}

const lv_style_t *ui_style_get_btn_primary(void)
{
    return &style_btn_primary;
}

const lv_style_t *ui_style_get_btn_primary_large(void)
{
    return &style_btn_primary_large;
}

const lv_style_t *ui_style_get_btn_secondary(void)
{
    return &style_btn_secondary;
}

const lv_style_t *ui_style_get_btn_secondary_large(void)
{
    return &style_btn_secondary_large;
}

const lv_style_t *ui_style_get_btn_danger(void)
{
    return &style_btn_danger;
}

const lv_style_t *ui_style_get_btn_danger_large(void)
{
    return &style_btn_danger_large;
}

const lv_style_t *ui_style_get_status_badge(void)
{
    return &style_status_badge;
}

const lv_style_t *ui_style_get_title(void)
{
    return &style_title;
}

const lv_style_t *ui_style_get_body(void)
{
    return &style_body;
}

const lv_style_t *ui_style_get_caption(void)
{
    return &style_caption;
}

// ============================================================================
// Helper Widget Creation
// ============================================================================

lv_obj_t *ui_create_container(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &style_panel, LV_PART_MAIN);
    lv_obj_set_width(obj, LV_SIZE_CONTENT);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    return obj;
}

lv_obj_t *ui_create_btn_primary(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_add_style(obj, &style_btn_primary, LV_PART_MAIN);
    lv_obj_add_style(obj, &style_btn_primary_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    return obj;
}

lv_obj_t *ui_create_btn_primary_large(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_add_style(obj, &style_btn_primary_large, LV_PART_MAIN);
    lv_obj_add_style(obj, &style_btn_primary_large_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    return obj;
}

lv_obj_t *ui_create_btn_secondary(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_add_style(obj, &style_btn_secondary, LV_PART_MAIN);
    lv_obj_add_style(obj, &style_btn_secondary_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    return obj;
}

lv_obj_t *ui_create_btn_secondary_large(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_add_style(obj, &style_btn_secondary_large, LV_PART_MAIN);
    lv_obj_add_style(obj, &style_btn_secondary_large_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    return obj;
}

lv_obj_t *ui_create_btn_danger_large(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_add_style(obj, &style_btn_danger_large, LV_PART_MAIN);
    lv_obj_add_style(obj, &style_btn_danger_large_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    return obj;
}

lv_obj_t *ui_screen_create_header(lv_obj_t *parent, const char *title, lv_obj_t **title_label_out)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(header);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, UI_PADDING_LG, 0);
    lv_obj_set_style_text_font(label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(label, title != NULL ? title : "");

    if (title_label_out != NULL) {
        *title_label_out = label;
    }
    return header;
}

void ui_label_set_font_color(lv_obj_t *label, const lv_font_t *font, lv_color_t color)
{
    if (label == NULL) {
        return;
    }
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

lv_obj_t *ui_create_title(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_add_style(obj, &style_title, LV_PART_MAIN);
    return obj;
}

lv_obj_t *ui_create_body(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_add_style(obj, &style_body, LV_PART_MAIN);
    return obj;
}

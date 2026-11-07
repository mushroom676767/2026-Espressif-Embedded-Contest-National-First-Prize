/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen 1: Standby Screen (Revised)
 *
 * Layout (1024x600):
 *  +-----------------------------------------------------------+
 *  | [Wi-Fi dot] Wi-Fi       Title    2026-06-03    [Settings] |  <- 40px status bar
 *  +-----------------------------------------------------------+
 *  |                                                           |
 *  |              FOD Smart Tool Cabinet                       |
 *  |         Aviation Maintenance Workshop | NAS 412           |
 *  |                                                           |
 *  |                   14:30:25                                |  <- Large time (SNTP)
 *  |                                                           |
 *  |   +------------------+      +------------------+          |
 *  |   |  [NFC]  NFC Card |      | [FACE] Face ID   |          |  <- Auth buttons
 *  |   |  Swipe to login  |      | Look at camera   |          |
 *  |   +------------------+      +------------------+          |
 *  |                                                           |
 *  |              Tools: 10 / 12 available                     |  <- Status line
 *  |                                                           |
 *  +-----------------------------------------------------------+
 */

#include "screen_standby.h"
#include "ui_theme.h"
#include "ui_touch_layout.h"
#include "esp_log.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "screen_standby";

// ============================================================================
// Internal Data
// ============================================================================
struct screen_standby_data_s {
    lv_obj_t *screen;
    lv_obj_t *status_bar;
    lv_obj_t *online_dot;
    lv_obj_t *online_label;
    lv_obj_t *date_label;
    lv_obj_t *settings_btn;
    // Main content
    lv_obj_t *title_label;
    lv_obj_t *wifi_wait_label;
    // Auth buttons
    lv_obj_t *nfc_btn;
    lv_obj_t *nfc_icon_label;
    lv_obj_t *nfc_hint_label;
    lv_obj_t *face_btn;
    lv_obj_t *face_icon_label;
    lv_obj_t *face_hint_label;
    lv_obj_t *register_btn;
    lv_obj_t *register_icon_label;
    lv_obj_t *register_hint_label;
    lv_obj_t *admin_btn;
    lv_obj_t *admin_icon_label;
    lv_obj_t *admin_hint_label;
    // Status
    lv_obj_t *tool_count_label;
};

static struct screen_standby_data_s s_data;
static bool s_online = false;
static bool s_time_synced = false;
static uint16_t s_total = 0;
static uint16_t s_available = 0;

// ============================================================================
// Callback declarations
// ============================================================================
static void (*s_nfc_btn_cb)(void) = NULL;
static void (*s_face_btn_cb)(void) = NULL;
static void (*s_register_btn_cb)(void) = NULL;
static void (*s_admin_btn_cb)(void) = NULL;
static void (*s_settings_btn_cb)(void) = NULL;

// ============================================================================
// Internal callback setters (called by ui_manager wrappers)
// ============================================================================
void screen_standby_set_nfc_callback(void (*cb)(void)) { s_nfc_btn_cb = cb; }
void screen_standby_set_face_callback(void (*cb)(void)) { s_face_btn_cb = cb; }
void screen_standby_set_register_callback(void (*cb)(void)) { s_register_btn_cb = cb; }
void screen_standby_set_admin_callback(void (*cb)(void)) { s_admin_btn_cb = cb; }
void screen_standby_set_settings_callback(void (*cb)(void)) { s_settings_btn_cb = cb; }

// ============================================================================
// Event callbacks
// ============================================================================
static void nfc_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "NFC login button pressed");
    if (s_nfc_btn_cb) s_nfc_btn_cb();
}

static void face_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Face login button pressed");
    if (s_face_btn_cb) s_face_btn_cb();
}

static void register_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Register button pressed");
    if (s_register_btn_cb) s_register_btn_cb();
}

static void admin_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Admin button pressed");
    if (s_admin_btn_cb) s_admin_btn_cb();
}

static void settings_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings button pressed");
    if (s_settings_btn_cb) s_settings_btn_cb();
}

// ============================================================================
// Timer: update time every 1s
// ============================================================================
static lv_timer_t *s_time_timer = NULL;

static void time_timer_cb(lv_timer_t *timer)
{
    if (s_data.date_label == NULL) return;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // If time is synced (year >= 2020), display real time
    if (timeinfo.tm_year >= (2020 - 1900)) {
        char bar_date_time[32];
        snprintf(bar_date_time, sizeof(bar_date_time), "%04d-%02d-%02d  %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        lv_label_set_text(s_data.date_label, bar_date_time);
    } else {
        // Time not synced yet
        lv_label_set_text(s_data.date_label, "----/--/--  --:--:--");
    }
}

// ============================================================================
// Update helpers
// ============================================================================
static void update_status_bar(void)
{
    if (s_data.online_dot == NULL) return;
    if (s_online) {
        lv_obj_set_style_bg_color(s_data.online_dot, UI_COLOR_ONLINE, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_data.online_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
        if (s_data.wifi_wait_label != NULL) {
            lv_obj_add_flag(s_data.wifi_wait_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_set_style_bg_color(s_data.online_dot, UI_COLOR_OFFLINE, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_data.online_label, UI_COLOR_DANGER, LV_PART_MAIN);
        if (s_data.wifi_wait_label != NULL) {
            lv_obj_clear_flag(s_data.wifi_wait_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_tool_count_display(void)
{
    if (s_data.tool_count_label == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "工具:%u / %u 可用", s_available, s_total);
    lv_label_set_text(s_data.tool_count_label, buf);
}

// ============================================================================
// Create auth button helper
// ============================================================================
static void create_auth_button(lv_obj_t **btn_out, lv_obj_t **icon_out,
                                lv_obj_t **hint_out, lv_obj_t *parent,
                                const char *icon_text, const char *title,
                                const char *hint, lv_color_t accent_color,
                                lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 220, UI_TILE_H_LG);
    lv_obj_set_style_bg_color(btn, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, accent_color, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x00000040), LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    // Pressed state
    lv_obj_set_style_bg_color(btn, lv_color_darken(UI_COLOR_BG_PANEL, 20), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, UI_COLOR_PRIMARY, LV_PART_MAIN | LV_STATE_PRESSED);

    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_style_pad_all(btn, UI_PADDING_SM, LV_PART_MAIN);

        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, -10);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, accent_color, LV_PART_MAIN);
        lv_label_set_text(icon, icon_text);

        lv_obj_t *title_label = lv_label_create(btn);
        lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 68, -12);
        lv_obj_set_style_text_font(title_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
        lv_label_set_text(title_label, title);

        lv_obj_t *hint_label = lv_label_create(btn);
        lv_obj_set_width(hint_label, UI_TOUCH_SAFE_CONTENT_W - 82);
        lv_obj_align(hint_label, LV_ALIGN_LEFT_MID, 68, 16);
        lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_style_text_font(hint_label, UI_FONT_STATUS, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
        lv_label_set_text(hint_label, hint);

        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        *btn_out = btn;
        *icon_out = icon;
        *hint_out = hint_label;
        return;
    }

    // Icon (large)
    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, accent_color, LV_PART_MAIN);
    lv_label_set_text(icon, icon_text);

    // Title
    lv_obj_t *title_label = lv_label_create(btn);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_text_font(title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(title_label, title);

    // Hint
    lv_obj_t *hint_label = lv_label_create(btn);
    lv_obj_set_width(hint_label, 190);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(hint_label, hint);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    *btn_out = btn;
    *icon_out = icon;
    *hint_out = hint_label;
}

// ============================================================================
// Create standby screen
// ============================================================================
void screen_standby_create(void)
{
    ESP_LOGI(TAG, "Creating standby screen...");
    memset(&s_data, 0, sizeof(s_data));

    // === Screen background ===
    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_data.screen, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_data.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);

    // === Top status bar (40px) ===
    s_data.status_bar = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.status_bar, UI_SCREEN_WIDTH, 40);
    lv_obj_align(s_data.status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.status_bar, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.status_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.status_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Online indicator dot
    s_data.online_dot = lv_obj_create(s_data.status_bar);
    lv_obj_set_size(s_data.online_dot, 10, 10);
    lv_obj_align(s_data.online_dot, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_radius(s_data.online_dot, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_data.online_dot, UI_COLOR_OFFLINE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.online_dot, 0, LV_PART_MAIN);

    // Wi-Fi label
    s_data.online_label = lv_label_create(s_data.status_bar);
    lv_obj_align(s_data.online_label, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_style_text_font(s_data.online_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.online_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(s_data.online_label, "Wi-Fi");

    // Date label (center of status bar)
    s_data.date_label = lv_label_create(s_data.status_bar);
    lv_obj_set_width(s_data.date_label, 360);
    lv_obj_align(s_data.date_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.date_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.date_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.date_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_data.date_label, "----/--/--  --:--:--");

    // Settings button (top-right)
    lv_obj_t *settings_parent = ui_touch_is_damaged_mode() ? s_data.screen : s_data.status_bar;
    s_data.settings_btn = lv_btn_create(settings_parent);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.settings_btn,
                          UI_TOUCH_SAFE_PAD,
                          72 + 4 * (84 + UI_TOUCH_SAFE_GAP),
                          UI_TOUCH_SAFE_CONTENT_W,
                          48);
    } else {
        lv_obj_set_size(s_data.settings_btn, 80, 30);
        lv_obj_align(s_data.settings_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    lv_obj_set_style_bg_color(s_data.settings_btn, UI_COLOR_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.settings_btn, UI_RADIUS_SM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.settings_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_data.settings_btn, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.settings_btn, 0, LV_PART_MAIN);
    lv_obj_t *settings_label = lv_label_create(s_data.settings_btn);
    lv_obj_set_style_text_font(settings_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(settings_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS "  设置");
    lv_obj_center(settings_label);
    lv_obj_add_event_cb(s_data.settings_btn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // === Main title ===
    s_data.title_label = lv_label_create(s_data.screen);
    lv_obj_align(s_data.title_label, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_text_font(s_data.title_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.title_label, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(s_data.title_label, "FOD智能工具柜");

    // Subtitle
    lv_obj_t *subtitle = lv_label_create(s_data.screen);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_text_font(subtitle, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(subtitle, "航空维修车间  |  符合NAS 412");

    // === WiFi waiting prompt ===
    s_data.wifi_wait_label = lv_label_create(s_data.screen);
    lv_obj_set_width(s_data.wifi_wait_label, 420);
    lv_obj_align(s_data.wifi_wait_label, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_text_font(s_data.wifi_wait_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.wifi_wait_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.wifi_wait_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.wifi_wait_label, "等待WiFi连接");

    // === Auth and registration buttons ===
    // NFC button (left)
    create_auth_button(
        &s_data.nfc_btn, &s_data.nfc_icon_label, &s_data.nfc_hint_label,
        s_data.screen,
        LV_SYMBOL_OK, "NFC卡", "刷卡登录",
        UI_COLOR_PRIMARY, nfc_btn_event_cb
    );

    // Face button (center)
    create_auth_button(
        &s_data.face_btn, &s_data.face_icon_label, &s_data.face_hint_label,
        s_data.screen,
        LV_SYMBOL_EYE_OPEN, "人脸识别", "看向摄像头",
        UI_COLOR_SUCCESS, face_btn_event_cb
    );

    // Register button (right)
    create_auth_button(
        &s_data.register_btn, &s_data.register_icon_label, &s_data.register_hint_label,
        s_data.screen,
        LV_SYMBOL_PLUS, "注册", "添加用户",
        UI_COLOR_WARNING, register_btn_event_cb
    );

    create_auth_button(
        &s_data.admin_btn, &s_data.admin_icon_label, &s_data.admin_hint_label,
        s_data.screen,
        LV_SYMBOL_SETTINGS, "管理", "管理用户",
        UI_COLOR_SECONDARY, admin_btn_event_cb
    );
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.nfc_btn, 0, 72, 84);
        ui_touch_place_stack(s_data.face_btn, 1, 72, 84);
        ui_touch_place_stack(s_data.register_btn, 2, 72, 84);
        ui_touch_place_stack(s_data.admin_btn, 3, 72, 84);
    } else {
        lv_obj_align(s_data.nfc_btn, LV_ALIGN_CENTER, -330, 80);
        lv_obj_align(s_data.face_btn, LV_ALIGN_CENTER, -110, 80);
        lv_obj_align(s_data.register_btn, LV_ALIGN_CENTER, 110, 80);
        lv_obj_align(s_data.admin_btn, LV_ALIGN_CENTER, 330, 80);
    }

    // === Tool count status line ===
    s_data.tool_count_label = lv_label_create(s_data.screen);
    lv_obj_align(s_data.tool_count_label, LV_ALIGN_BOTTOM_MID, 0, -65);
    lv_obj_set_style_text_font(s_data.tool_count_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.tool_count_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    lv_label_set_text(s_data.tool_count_label, "工具:-- / -- 可用");

    // === Bottom hint ===
    lv_obj_t *hint = lv_label_create(s_data.screen);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_font(hint, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(hint, "刷卡/点击人脸识别/注册或管理用户");

    // === Start timer & animation ===
    s_time_timer = lv_timer_create(time_timer_cb, 1000, NULL);
    lv_timer_ready(s_time_timer);

    // Initial display
    update_tool_count_display();
    update_status_bar();

    ESP_LOGI(TAG, "Standby screen created");
}

// ============================================================================
// Public API
// ============================================================================

lv_obj_t *screen_standby_get_screen(void)
{
    return s_data.screen;
}

void screen_standby_update_count(uint16_t total, uint16_t available)
{
    s_total = total;
    s_available = available;
    update_tool_count_display();
}

void screen_standby_set_online(bool online)
{
    s_online = online;
    update_status_bar();
}

void screen_standby_set_time_synced(bool synced)
{
    s_time_synced = synced;
    if (synced) {
        ESP_LOGI(TAG, "NTP time synced");
    }
}

void screen_standby_trigger_pulse(void)
{
}

struct screen_standby_data_s *screen_standby_get_data(void)
{
    return &s_data;
}

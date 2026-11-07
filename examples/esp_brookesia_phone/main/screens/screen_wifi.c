/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen: WiFi settings
 */

#include "screen_wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "ui_touch_layout.h"
#include "ui_manager.h"
#include "ui_theme.h"

static const char *TAG = "screen_wifi";

struct screen_wifi_data_s {
    lv_obj_t *screen;
    lv_obj_t *status_label;
    lv_obj_t *current_label;
    lv_obj_t *ap_list;
    lv_obj_t *spinner;
    lv_obj_t *password_overlay;
    lv_obj_t *password_panel;
    lv_obj_t *password_ssid_label;
    lv_obj_t *password_ta;
    lv_obj_t *keyboard;
};

static struct screen_wifi_data_s s_data;
static char s_selected_ssid[33] = {0};
static bool s_selected_secure = false;

static void copy_selected_ssid(const char *ssid)
{
    size_t len = 0;
    size_t max_len = sizeof(s_selected_ssid) - 1;

    s_selected_ssid[0] = '\0';
    if (ssid == NULL) {
        return;
    }

    while (len < max_len && ssid[len] != '\0') {
        len++;
    }

    memcpy(s_selected_ssid, ssid, len);
    s_selected_ssid[len] = '\0';
}

static void copy_password_text(char *dst, size_t dst_size, const char *src)
{
    size_t len = 0;
    size_t max_len = dst_size > 0 ? dst_size - 1 : 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (len < max_len && src[len] != '\0') {
        len++;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static lv_color_t signal_color(wifi_signal_level_t signal)
{
    switch (signal) {
        case WIFI_SIGNAL_GOOD:
            return UI_COLOR_SUCCESS;
        case WIFI_SIGNAL_MODERATE:
            return UI_COLOR_WARNING;
        case WIFI_SIGNAL_WEAK:
            return UI_COLOR_TEXT_SECOND;
        default:
            return UI_COLOR_TEXT_HINT;
    }
}

static const char *signal_text(wifi_signal_level_t signal)
{
    switch (signal) {
        case WIFI_SIGNAL_GOOD:
            return "GOOD";
        case WIFI_SIGNAL_MODERATE:
            return "MID";
        case WIFI_SIGNAL_WEAK:
            return "LOW";
        default:
            return "NONE";
    }
}

static void set_status(const char *text, lv_color_t color)
{
    if (s_data.status_label == NULL) {
        return;
    }
    lv_label_set_text(s_data.status_label, text);
    lv_obj_set_style_text_color(s_data.status_label, color, LV_PART_MAIN);
}

static void set_spinner_visible(bool visible)
{
    if (s_data.spinner == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_data.spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_data.spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_current_label(void)
{
    if (s_data.current_label == NULL) {
        return;
    }

    const char *ssid = wifi_manager_get_current_ssid();
    if (wifi_manager_is_connected() && ssid != NULL) {
        lv_label_set_text_fmt(s_data.current_label, "已连接:%s", ssid);
        lv_obj_set_style_text_color(s_data.current_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_data.current_label, "未连接");
        lv_obj_set_style_text_color(s_data.current_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    }
}

static void show_list_message(const char *message)
{
    lv_obj_clean(s_data.ap_list);

    lv_obj_t *label = lv_label_create(s_data.ap_list);
    lv_obj_set_width(label, 820);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(label, message);
}

static void hide_password_dialog(void)
{
    if (s_data.password_overlay != NULL) {
        lv_obj_add_flag(s_data.password_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.password_ta != NULL) {
        lv_textarea_set_text(s_data.password_ta, "");
    }
}

static void connect_selected_network(void)
{
    if (s_selected_ssid[0] == '\0') {
        return;
    }

    char password_buf[65] = {0};
    const char *password = "";
    if (s_selected_secure) {
        const char *entered_password = lv_textarea_get_text(s_data.password_ta);
        if (entered_password == NULL || entered_password[0] == '\0') {
            set_status("请输入密码", UI_COLOR_WARNING);
            return;
        }
        copy_password_text(password_buf, sizeof(password_buf), entered_password);
        password = password_buf;
    }

    char status[80];
    snprintf(status, sizeof(status), "正在连接:%s", s_selected_ssid);
    set_status(status, UI_COLOR_WARNING);
    set_spinner_visible(true);
    hide_password_dialog();
    wifi_manager_stop_scan();
    wifi_manager_connect(s_selected_ssid, password);
}

static void show_password_dialog(const wifi_ap_info_t *ap)
{
    if (ap == NULL || s_data.password_overlay == NULL) {
        return;
    }

    copy_selected_ssid(ap->ssid);
    s_selected_secure = true;

    lv_label_set_text_fmt(s_data.password_ssid_label, "SSID:%s", s_selected_ssid);
    lv_textarea_set_text(s_data.password_ta, "");
    lv_keyboard_set_textarea(s_data.keyboard, s_data.password_ta);
    lv_obj_clear_flag(s_data.password_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(s_data.password_ta, LV_STATE_FOCUSED);
}

static void ap_row_event_cb(lv_event_t *e)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    wifi_ap_info_t selected;
    if (!wifi_manager_get_ap_info((uint16_t)index, &selected)) {
        return;
    }

    copy_selected_ssid(selected.ssid);
    s_selected_secure = selected.has_password;

    if (selected.has_password) {
        show_password_dialog(&selected);
    } else {
        connect_selected_network();
    }
}

static void rebuild_ap_list(void)
{
    lv_obj_clean(s_data.ap_list);

    uint16_t count = wifi_manager_get_ap_count();
    if (count == 0) {
        show_list_message("未找到网络");
        return;
    }

    const char *current_ssid = wifi_manager_get_current_ssid();
    bool connected = wifi_manager_is_connected();

    for (uint16_t i = 0; i < count; i++) {
        wifi_ap_info_t ap;
        if (!wifi_manager_get_ap_info(i, &ap)) {
            continue;
        }

        bool damaged_mode = ui_touch_is_damaged_mode();
        lv_obj_t *row = lv_btn_create(s_data.ap_list);
        if (damaged_mode) {
            lv_obj_set_size(row, UI_TOUCH_SAFE_CONTENT_W - 8, UI_ROW_H_LG);
        } else {
            lv_obj_set_size(row, 860, UI_ROW_H_LG);
        }
        lv_obj_set_style_bg_color(row, UI_COLOR_BG_PANEL, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_darken(UI_COLOR_BG_PANEL, 18), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, UI_RADIUS_LG, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, UI_COLOR_BORDER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, ap_row_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *signal = lv_label_create(row);
        lv_obj_align(signal, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_text_font(signal, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(signal, signal_color(ap.signal), LV_PART_MAIN);
        lv_label_set_text(signal, LV_SYMBOL_WIFI);

        lv_obj_t *ssid = lv_label_create(row);
        lv_obj_set_width(ssid, 370);
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 60, 0);
        lv_obj_set_style_text_font(ssid, UI_FONT_BODY_BOLD, LV_PART_MAIN);
        lv_obj_set_style_text_color(ssid, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_label_set_text(ssid, ap.ssid);

        lv_obj_t *security = lv_label_create(row);
        lv_obj_align(security, LV_ALIGN_LEFT_MID, 455, 0);
        lv_obj_set_style_text_font(security, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(security, ap.has_password ? UI_COLOR_WARNING : UI_COLOR_SUCCESS, LV_PART_MAIN);
        lv_label_set_text(security, ap.has_password ? "加密" : "开放");

        lv_obj_t *level = lv_label_create(row);
        lv_obj_align(level, LV_ALIGN_LEFT_MID, 540, 0);
        lv_obj_set_style_text_font(level, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(level, signal_color(ap.signal), LV_PART_MAIN);
        lv_label_set_text_fmt(level, "%s  %d dBm", signal_text(ap.signal), ap.rssi);

        if (connected && current_ssid != NULL && strcmp(current_ssid, ap.ssid) == 0) {
            lv_obj_t *mark = lv_label_create(row);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_set_style_text_font(mark, UI_FONT_BODY_BOLD, LV_PART_MAIN);
            lv_obj_set_style_text_color(mark, UI_COLOR_SUCCESS, LV_PART_MAIN);
            lv_label_set_text(mark, LV_SYMBOL_OK " 已连");
        }
    }
}

static void start_scan_from_ui(void)
{
    if (wifi_manager_get_state() == WIFI_STATE_CONNECTING) {
        set_status("正在连接...", UI_COLOR_WARNING);
        set_spinner_visible(true);
        show_list_message("正在连接网络");
        return;
    }

    set_status("正在扫描网络...", UI_COLOR_WARNING);
    set_spinner_visible(true);
    show_list_message("正在扫描...");
    wifi_manager_start_scan();
}

static void back_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Back to standby");
    hide_password_dialog();
    wifi_manager_stop_scan();
    ui_manager_goto_screen(SCREEN_STANDBY);
}

static void refresh_btn_event_cb(lv_event_t *e)
{
    (void)e;
    start_scan_from_ui();
}

static void connect_btn_event_cb(lv_event_t *e)
{
    (void)e;
    connect_selected_network();
}

static void cancel_btn_event_cb(lv_event_t *e)
{
    (void)e;
    hide_password_dialog();
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        connect_selected_network();
    } else if (code == LV_EVENT_CANCEL) {
        hide_password_dialog();
    }
}

static void screen_loaded_event_cb(lv_event_t *e)
{
    (void)e;
    update_current_label();
    start_scan_from_ui();
}

static lv_obj_t *create_header_button(lv_obj_t *parent, const char *text, lv_align_t align, int32_t x)
{
    lv_obj_t *btn = ui_create_btn_secondary_large(parent);
    lv_obj_set_size(btn, 146, UI_BTN_H_MD);
    lv_obj_align(btn, align, x, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void create_password_dialog(void)
{
    s_data.password_overlay = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.password_overlay, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_align(s_data.password_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_data.password_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_data.password_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.password_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.password_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.password_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_data.password_overlay, LV_OBJ_FLAG_HIDDEN);

    s_data.password_panel = lv_obj_create(s_data.password_overlay);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.password_panel, UI_TOUCH_SAFE_W, UI_SCREEN_HEIGHT);
        lv_obj_align(s_data.password_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        lv_obj_set_size(s_data.password_panel, 860, 550);
        lv_obj_align(s_data.password_panel, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_set_style_bg_color(s_data.password_panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.password_panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.password_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_data.password_panel, UI_COLOR_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.password_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_data.password_panel);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_text_font(title, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi密码");

    s_data.password_ssid_label = lv_label_create(s_data.password_panel);
    lv_obj_set_width(s_data.password_ssid_label, 700);
    lv_obj_align(s_data.password_ssid_label, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_text_font(s_data.password_ssid_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.password_ssid_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.password_ssid_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_data.password_ssid_label, "SSID:");

    s_data.password_ta = lv_textarea_create(s_data.password_panel);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.password_ta, UI_TOUCH_SAFE_PAD, 100, UI_TOUCH_SAFE_CONTENT_W, 56);
    } else {
        lv_obj_set_size(s_data.password_ta, 720, 60);
        lv_obj_align(s_data.password_ta, LV_ALIGN_TOP_MID, 0, 104);
    }
    lv_textarea_set_one_line(s_data.password_ta, true);
    lv_textarea_set_password_mode(s_data.password_ta, true);
    lv_textarea_set_max_length(s_data.password_ta, 64);
    lv_textarea_set_placeholder_text(s_data.password_ta, "密码");
    lv_obj_set_style_text_font(s_data.password_ta, UI_FONT_BODY_BOLD, LV_PART_MAIN);

    lv_obj_t *connect_btn = ui_create_btn_primary_large(s_data.password_panel);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(connect_btn, 0, 170, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(connect_btn, 200, UI_BTN_H_MD);
        lv_obj_align(connect_btn, LV_ALIGN_TOP_MID, -110, 174);
    }
    lv_obj_add_event_cb(connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, LV_SYMBOL_OK " 连接");
    lv_obj_center(connect_label);

    lv_obj_t *cancel_btn = ui_create_btn_secondary_large(s_data.password_panel);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(cancel_btn, 1, 170, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(cancel_btn, 200, UI_BTN_H_MD);
        lv_obj_align(cancel_btn, LV_ALIGN_TOP_MID, 110, 174);
    }
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE " 取消");
    lv_obj_center(cancel_label);

    s_data.keyboard = lv_keyboard_create(s_data.password_panel);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.keyboard, UI_TOUCH_SAFE_CONTENT_W, 260);
        lv_obj_align(s_data.keyboard, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 292);
    } else {
        lv_obj_set_size(s_data.keyboard, 760, 250);
        lv_obj_align(s_data.keyboard, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
    lv_keyboard_set_textarea(s_data.keyboard, s_data.password_ta);
    lv_keyboard_set_mode(s_data.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_data.keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_data.keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

void screen_wifi_create(void)
{
    ESP_LOGI(TAG, "Creating WiFi settings screen...");
    memset(&s_data, 0, sizeof(s_data));

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_data.screen, screen_loaded_event_cb, LV_EVENT_SCREEN_LOADED, NULL);

    lv_obj_t *header = lv_obj_create(s_data.screen);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = create_header_button(header, LV_SYMBOL_LEFT " 返回", LV_ALIGN_LEFT_MID, 18);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(back_btn, UI_TOUCH_SAFE_PAD, 8,
                          (UI_TOUCH_SAFE_CONTENT_W - UI_TOUCH_SAFE_GAP) / 2, UI_BTN_H_SAFE);
    }

    lv_obj_t *title = lv_label_create(header);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(title, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi设置");

    lv_obj_t *refresh_btn = create_header_button(header, LV_SYMBOL_REFRESH " 扫描", LV_ALIGN_RIGHT_MID, -18);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(refresh_btn,
                          UI_TOUCH_SAFE_PAD + (UI_TOUCH_SAFE_CONTENT_W + UI_TOUCH_SAFE_GAP) / 2,
                          8,
                          (UI_TOUCH_SAFE_CONTENT_W - UI_TOUCH_SAFE_GAP) / 2,
                          UI_BTN_H_SAFE);
    }

    s_data.status_label = lv_label_create(s_data.screen);
    lv_obj_align(s_data.status_label, LV_ALIGN_TOP_LEFT, 62, 82);
    lv_obj_set_width(s_data.status_label, 520);
    lv_obj_set_style_text_font(s_data.status_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.status_label, LV_LABEL_LONG_DOT);
    set_status("就绪", UI_COLOR_TEXT_SECOND);

    s_data.current_label = lv_label_create(s_data.screen);
    lv_obj_align(s_data.current_label, LV_ALIGN_TOP_RIGHT, -62, 82);
    lv_obj_set_width(s_data.current_label, 360);
    lv_obj_set_style_text_font(s_data.current_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.current_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.current_label, LV_LABEL_LONG_DOT);
    update_current_label();

    s_data.spinner = lv_spinner_create(s_data.screen, 1000, 60);
    lv_obj_set_size(s_data.spinner, 36, 36);
    lv_obj_align(s_data.spinner, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_add_flag(s_data.spinner, LV_OBJ_FLAG_HIDDEN);

    s_data.ap_list = lv_obj_create(s_data.screen);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.ap_list, UI_TOUCH_SAFE_CONTENT_W, 412);
        lv_obj_align(s_data.ap_list, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 136);
    } else {
        lv_obj_set_size(s_data.ap_list, 900, 430);
        lv_obj_align(s_data.ap_list, LV_ALIGN_TOP_MID, 0, 136);
    }
    lv_obj_set_style_bg_color(s_data.ap_list, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.ap_list, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.ap_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_data.ap_list, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.ap_list, UI_PADDING_MD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_data.ap_list, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_data.ap_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_data.ap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_data.ap_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    show_list_message("点击扫描搜索网络");
    create_password_dialog();

    ESP_LOGI(TAG, "WiFi settings screen created");
}

lv_obj_t *screen_wifi_get_screen(void)
{
    return s_data.screen;
}

void screen_wifi_handle_wifi_event(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info)
{
    if (s_data.screen == NULL) {
        return;
    }

    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));

    switch (evt) {
        case WIFI_EVT_SCAN_STARTED:
            set_status("正在扫描网络...", UI_COLOR_WARNING);
            set_spinner_visible(true);
            break;
        case WIFI_EVT_SCAN_DONE: {
            char msg[64];
            snprintf(msg, sizeof(msg), "扫描完成:%u 个网络", wifi_manager_get_ap_count());
            set_status(msg, UI_COLOR_SUCCESS);
            set_spinner_visible(false);
            rebuild_ap_list();
            break;
        }
        case WIFI_EVT_SCAN_FAILED:
            set_status("扫描失败", UI_COLOR_DANGER);
            set_spinner_visible(false);
            show_list_message("扫描失败");
            break;
        case WIFI_EVT_CONNECTING:
            set_status("正在连接...", UI_COLOR_WARNING);
            set_spinner_visible(true);
            break;
        case WIFI_EVT_CONNECTED: {
            const char *ssid = (ap_info != NULL && ap_info->ssid[0] != '\0')
                                   ? ap_info->ssid
                                   : wifi_manager_get_current_ssid();
            char msg[80];
            snprintf(msg, sizeof(msg), "已连接:%s", ssid != NULL ? ssid : "WiFi");
            set_status(msg, UI_COLOR_SUCCESS);
            set_spinner_visible(false);
            update_current_label();
            rebuild_ap_list();
            break;
        }
        case WIFI_EVT_CONNECT_FAILED:
        {
            uint8_t reason = wifi_manager_get_last_disconnect_reason();
            char msg[96];
            if (reason != 0) {
                snprintf(msg, sizeof(msg), "连接失败:%s (%u)",
                         wifi_manager_get_last_disconnect_reason_name(), reason);
            } else {
                snprintf(msg, sizeof(msg), "连接失败:超时");
            }
            set_status(msg, UI_COLOR_DANGER);
            set_spinner_visible(false);
            update_current_label();
            rebuild_ap_list();
            break;
        }
        case WIFI_EVT_DISCONNECTED:
            set_status("已断开连接", UI_COLOR_DANGER);
            set_spinner_visible(false);
            update_current_label();
            rebuild_ap_list();
            break;
        default:
            break;
    }

    esp_lv_adapter_unlock();
}

struct screen_wifi_data_s *screen_wifi_get_data(void)
{
    return &s_data;
}

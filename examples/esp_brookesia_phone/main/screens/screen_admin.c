/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "screen_admin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_usb_preview.h"
#include "ui_touch_layout.h"
#include "ui_manager.h"
#include "ui_theme.h"
#include "user_manager.h"

static const char *TAG = "screen_admin";

#define ADMIN_PIN                  "000000"
#define ADMIN_HEADER_H             UI_HEADER_H
#define ADMIN_THUMB_W              160
#define ADMIN_THUMB_H              120
#define ADMIN_ROW_H                164
#define ADMIN_DELETE_TASK_STACK    (1024 * 14)
#define ADMIN_DELETE_TASK_PRIORITY 3

typedef struct {
    char employee_id[USER_EMPLOYEE_ID_MAX_LEN];
    esp_err_t ret;
} admin_delete_result_t;

struct screen_admin_data_s {
    lv_obj_t *screen;
    lv_obj_t *title_label;
    lv_obj_t *back_btn;
    lv_obj_t *pin_layer;
    lv_obj_t *pin_ta;
    lv_obj_t *pin_status_label;
    lv_obj_t *list_layer;
    lv_obj_t *list;
    lv_obj_t *open_door_btn;
    lv_obj_t *empty_label;
    lv_obj_t *list_status_label;
    lv_obj_t *confirm_overlay;
    lv_obj_t *confirm_text;
    lv_obj_t *confirm_delete_btn;
    lv_obj_t *confirm_cancel_btn;
    char delete_employee_id[USER_EMPLOYEE_ID_MAX_LEN];
    bool delete_in_progress;
    bool open_door_pending;
    uint8_t *thumb_bufs[USER_MANAGER_MAX_USERS];
};

static screen_admin_data_t s_data;

static void show_pin_layer(void);
static void show_list_layer(void);
static void refresh_user_list(void);
static void admin_delete_result_async_cb(void *user_data);
static void admin_delete_task(void *arg);
static void open_door_async_cb(void *user_data);

static void free_thumb_buffers(void)
{
    for (uint8_t i = 0; i < USER_MANAGER_MAX_USERS; i++) {
        if (s_data.thumb_bufs[i] != NULL) {
            heap_caps_free(s_data.thumb_bufs[i]);
            s_data.thumb_bufs[i] = NULL;
        }
    }
}

static void clear_list(void)
{
    if (s_data.list != NULL) {
        lv_obj_clean(s_data.list);
    }
    free_thumb_buffers();
}

static void set_layer_visible(lv_obj_t *layer, bool visible)
{
    if (layer == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void back_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_goto_screen(SCREEN_STANDBY);
}

static void pin_ok_event_cb(lv_event_t *e)
{
    (void)e;
    const char *pin = lv_textarea_get_text(s_data.pin_ta);
    if (pin != NULL && strcmp(pin, ADMIN_PIN) == 0) {
        lv_textarea_set_text(s_data.pin_ta, "");
        lv_label_set_text(s_data.pin_status_label, "");
        show_list_layer();
        return;
    }

    lv_label_set_text(s_data.pin_status_label, "PIN码错误");
    lv_obj_set_style_text_color(s_data.pin_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
}

static void pin_clear_event_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_data.pin_ta, "");
    lv_label_set_text(s_data.pin_status_label, "");
}

static void delete_btn_event_cb(lv_event_t *e)
{
    if (s_data.delete_in_progress) {
        lv_label_set_text(s_data.list_status_label, "正在删除, 请稍候");
        lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_WARNING, LV_PART_MAIN);
        return;
    }

    user_record_t *record = (user_record_t *)lv_event_get_user_data(e);
    if (record == NULL) {
        return;
    }

    snprintf(s_data.delete_employee_id, sizeof(s_data.delete_employee_id), "%s", record->employee_id);
    lv_label_set_text_fmt(s_data.confirm_text,
                          "确认删除用户?\n编号:%s\n姓名:%s",
                          record->employee_id,
                          record->name);
    set_layer_visible(s_data.confirm_overlay, true);
}

static void row_delete_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        free(lv_event_get_user_data(e));
    }
}

static void confirm_cancel_event_cb(lv_event_t *e)
{
    (void)e;
    s_data.delete_employee_id[0] = '\0';
    set_layer_visible(s_data.confirm_overlay, false);
}

static void confirm_delete_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_data.delete_employee_id[0] == '\0' || s_data.delete_in_progress) {
        return;
    }

    admin_delete_result_t *result = (admin_delete_result_t *)calloc(1, sizeof(*result));
    if (result == NULL) {
        set_layer_visible(s_data.confirm_overlay, false);
        lv_label_set_text(s_data.list_status_label, "删除失败: 内存不足");
        lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
        return;
    }

    snprintf(result->employee_id, sizeof(result->employee_id), "%s", s_data.delete_employee_id);
    s_data.delete_in_progress = true;
    set_layer_visible(s_data.confirm_overlay, false);
    lv_label_set_text(s_data.list_status_label, "正在删除...");
    lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_WARNING, LV_PART_MAIN);

    BaseType_t task_ret = xTaskCreate(admin_delete_task, "admin_delete",
                                      ADMIN_DELETE_TASK_STACK, result,
                                      ADMIN_DELETE_TASK_PRIORITY, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Admin delete task");
        s_data.delete_in_progress = false;
        free(result);
        lv_label_set_text(s_data.list_status_label, "删除失败: 内存不足");
        lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
    }
}

static void admin_delete_result_async_cb(void *user_data)
{
    admin_delete_result_t *result = (admin_delete_result_t *)user_data;
    if (result == NULL) {
        return;
    }

    s_data.delete_in_progress = false;
    if (result->ret != ESP_OK) {
        ESP_LOGE(TAG, "Delete user %s failed: %s",
                 result->employee_id, esp_err_to_name(result->ret));
        lv_label_set_text_fmt(s_data.list_status_label,
                              "删除失败: %s",
                              esp_err_to_name(result->ret));
        lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
        free(result);
        return;
    }

    ESP_LOGI(TAG, "Admin deleted user: %s", result->employee_id);
    s_data.delete_employee_id[0] = '\0';
    lv_label_set_text(s_data.list_status_label, "用户已删除");
    lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    refresh_user_list();
    free(result);
}

static void admin_delete_task(void *arg)
{
    admin_delete_result_t *result = (admin_delete_result_t *)arg;
    if (result == NULL) {
        vTaskDelete(NULL);
        return;
    }

    result->ret = user_manager_delete_user(result->employee_id);

    esp_err_t lock_ret = esp_lv_adapter_lock(portMAX_DELAY);
    if (lock_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock LVGL for Admin delete result: %s", esp_err_to_name(lock_ret));
        s_data.delete_in_progress = false;
        free(result);
        vTaskDelete(NULL);
        return;
    }

    lv_res_t async_ret = lv_async_call(admin_delete_result_async_cb, result);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGE(TAG, "Failed to queue Admin delete result");
        s_data.delete_in_progress = false;
        free(result);
    }

    vTaskDelete(NULL);
}

static bool load_thumbnail(const user_record_t *record, uint8_t **buf_out)
{
    if (record == NULL || record->face_photo_path[0] == '\0' || buf_out == NULL) {
        return false;
    }

    FILE *file = fopen(record->face_photo_path, "rb");
    if (file == NULL) {
        return false;
    }

    size_t size = ADMIN_THUMB_W * ADMIN_THUMB_H * sizeof(uint16_t);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(file);
        return false;
    }

    size_t read_len = fread(buf, 1, size, file);
    fclose(file);
    if (read_len != size) {
        free(buf);
        return false;
    }

    *buf_out = buf;
    return true;
}

static lv_obj_t *create_text_btn(lv_obj_t *parent, const char *text, const lv_style_t *style,
                                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_style(btn, (lv_style_t *)style, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void create_user_row(const user_record_t *record, uint8_t index)
{
    lv_obj_t *row = lv_obj_create(s_data.list);
    lv_obj_set_size(row, ui_touch_is_damaged_mode() ? UI_TOUCH_SAFE_CONTENT_W - 8 : 930, ADMIN_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    uint8_t *thumb = NULL;
    if (load_thumbnail(record, &thumb)) {
        s_data.thumb_bufs[index] = thumb;
        lv_obj_t *img = lv_canvas_create(row);
        lv_obj_set_size(img, ADMIN_THUMB_W, ADMIN_THUMB_H);
        lv_canvas_set_buffer(img, thumb, ADMIN_THUMB_W, ADMIN_THUMB_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);
    } else {
        lv_obj_t *placeholder = lv_obj_create(row);
        lv_obj_set_size(placeholder, ADMIN_THUMB_W, ADMIN_THUMB_H);
        lv_obj_align(placeholder, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(placeholder, UI_COLOR_BG_DARK, LV_PART_MAIN);
        lv_obj_set_style_border_width(placeholder, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(placeholder, UI_COLOR_BORDER, LV_PART_MAIN);
        lv_obj_t *label = lv_label_create(placeholder);
        lv_obj_set_style_text_font(label, UI_FONT_CAPTION, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
        lv_label_set_text(label, "无照片");
        lv_obj_center(label);
    }

    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_width(info, 560);
    lv_obj_align(info, LV_ALIGN_LEFT_MID, 180, 0);
    lv_obj_set_style_text_font(info, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(info,
                          "编号:%s\n姓名:%s\nNFC:%s\n人脸:%u/3",
                          record->employee_id,
                          record->name,
                          record->nfc_card_hex[0] != '\0' ? record->nfc_card_hex : "--",
                          record->face_feature_count);

    user_record_t *row_record = (user_record_t *)malloc(sizeof(*row_record));
    if (row_record != NULL) {
        *row_record = *record;
        lv_obj_add_event_cb(row, row_delete_event_cb, LV_EVENT_DELETE, row_record);
    }

    lv_obj_t *delete_btn = create_text_btn(row, LV_SYMBOL_TRASH " 删除",
                                           ui_style_get_btn_danger_large(),
                                           delete_btn_event_cb,
                                           row_record);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(delete_btn, UI_TOUCH_SAFE_W - 64, UI_BTN_H_SAFE);
        lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    } else {
        lv_obj_set_size(delete_btn, 146, UI_BTN_H_MD);
        lv_obj_align(delete_btn, LV_ALIGN_RIGHT_MID, -6, 0);
    }
}

static void open_door_btn_event_cb(lv_event_t *e)
{
    (void)e;

    if (s_data.open_door_pending) {
        return;
    }

    s_data.open_door_pending = true;
    lv_obj_add_state(s_data.open_door_btn, LV_STATE_DISABLED);
    lv_res_t ret = lv_async_call(open_door_async_cb, NULL);
    if (ret != LV_RES_OK) {
        s_data.open_door_pending = false;
        lv_obj_clear_state(s_data.open_door_btn, LV_STATE_DISABLED);
        ESP_LOGE(TAG, "Failed to queue Admin direct-open transition");
    }
}

static void open_door_async_cb(void *user_data)
{
    (void)user_data;

    if (!s_data.open_door_pending || ui_manager_get_current_screen() != SCREEN_ADMIN) {
        s_data.open_door_pending = false;
        lv_obj_clear_state(s_data.open_door_btn, LV_STATE_DISABLED);
        return;
    }

    ESP_LOGI(TAG, "Admin bypassed PPE; starting IMU open detection and bounded unlock pulse");
    screen_usb_preview_start("管理员", AUTH_TYPE_ADMIN);
    ui_manager_goto_screen(SCREEN_USB_PREVIEW);
}

static void screen_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOADED) {
        return;
    }

    /* Delete image objects before releasing their external thumbnail buffers. */
    clear_list();
    s_data.open_door_pending = false;
    lv_obj_clear_state(s_data.open_door_btn, LV_STATE_DISABLED);
}

static void refresh_user_list(void)
{
    user_record_t records[USER_MANAGER_MAX_USERS];
    size_t count = 0;

    clear_list();

    esp_err_t ret = user_manager_list(records, USER_MANAGER_MAX_USERS, &count);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_data.empty_label, "加载用户失败: %s", esp_err_to_name(ret));
        lv_obj_clear_flag(s_data.empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (count == 0) {
        lv_label_set_text(s_data.empty_label, "暂无注册用户");
        lv_obj_clear_flag(s_data.empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_data.empty_label, LV_OBJ_FLAG_HIDDEN);
    size_t shown = count > USER_MANAGER_MAX_USERS ? USER_MANAGER_MAX_USERS : count;
    for (size_t i = 0; i < shown; i++) {
        create_user_row(&records[i], (uint8_t)i);
    }
}

static void show_pin_layer(void)
{
    lv_label_set_text(s_data.title_label, "管理员登录");
    set_layer_visible(s_data.pin_layer, true);
    set_layer_visible(s_data.list_layer, false);
    set_layer_visible(s_data.confirm_overlay, false);
}

static void show_list_layer(void)
{
    lv_label_set_text(s_data.title_label, "用户管理");
    set_layer_visible(s_data.pin_layer, false);
    set_layer_visible(s_data.list_layer, true);
    set_layer_visible(s_data.confirm_overlay, false);
    lv_label_set_text(s_data.list_status_label, "");
    lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    refresh_user_list();
}

static void create_header(void)
{
    lv_obj_t *header = lv_obj_create(s_data.screen);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, ADMIN_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_data.title_label = lv_label_create(header);
    lv_obj_align(s_data.title_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.title_label, UI_COLOR_PRIMARY, LV_PART_MAIN);

    s_data.back_btn = create_text_btn(header, LV_SYMBOL_LEFT " 返回",
                                      ui_style_get_btn_secondary_large(),
                                      back_btn_event_cb, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.back_btn, UI_TOUCH_SAFE_PAD, 8, UI_TOUCH_SAFE_CONTENT_W, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.back_btn, 146, UI_BTN_H_MD);
        lv_obj_align(s_data.back_btn, LV_ALIGN_RIGHT_MID, -18, 0);
    }
}

static void create_pin_layer(void)
{
    s_data.pin_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.pin_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - ADMIN_HEADER_H);
    lv_obj_align(s_data.pin_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.pin_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.pin_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.pin_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.pin_ta = lv_textarea_create(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.pin_ta, UI_TOUCH_SAFE_PAD, 72, UI_TOUCH_SAFE_CONTENT_W, 56);
    } else {
        lv_obj_set_size(s_data.pin_ta, 420, 64);
        lv_obj_align(s_data.pin_ta, LV_ALIGN_TOP_MID, 0, 80);
    }
    lv_textarea_set_one_line(s_data.pin_ta, true);
    lv_textarea_set_password_mode(s_data.pin_ta, true);
    lv_textarea_set_max_length(s_data.pin_ta, 12);
    lv_textarea_set_placeholder_text(s_data.pin_ta, "管理员PIN码");

    lv_obj_t *ok_btn = create_text_btn(s_data.pin_layer, LV_SYMBOL_OK " 进入",
                                       ui_style_get_btn_primary_large(),
                                       pin_ok_event_cb, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(ok_btn, 0, 460, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(ok_btn, 170, UI_BTN_H_MD);
        lv_obj_align(ok_btn, LV_ALIGN_TOP_MID, -90, 160);
    }

    lv_obj_t *clear_btn = create_text_btn(s_data.pin_layer, "清除",
                                          ui_style_get_btn_secondary_large(),
                                          pin_clear_event_cb, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(clear_btn, 1, 460, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(clear_btn, 170, UI_BTN_H_MD);
        lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 90, 160);
    }

    s_data.pin_status_label = lv_label_create(s_data.pin_layer);
    lv_obj_set_width(s_data.pin_status_label, 480);
    lv_obj_align(s_data.pin_status_label, LV_ALIGN_TOP_MID, 0, 226);
    lv_obj_set_style_text_font(s_data.pin_status_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.pin_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.pin_status_label, "");

    lv_obj_t *keyboard = lv_keyboard_create(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(keyboard, UI_TOUCH_SAFE_CONTENT_W, 300);
        lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 150);
    } else {
        lv_obj_set_size(keyboard, 520, 230);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(keyboard, s_data.pin_ta);
}

static void create_list_layer(void)
{
    s_data.list_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.list_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - ADMIN_HEADER_H);
    lv_obj_align(s_data.list_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.list_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.list_layer, 0, LV_PART_MAIN);

    s_data.open_door_btn = create_text_btn(s_data.list_layer,
                                           LV_SYMBOL_HOME " 直接开门",
                                           ui_style_get_btn_primary_large(),
                                           open_door_btn_event_cb,
                                           NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.open_door_btn, UI_TOUCH_SAFE_PAD, 8,
                          UI_TOUCH_SAFE_CONTENT_W, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.open_door_btn, 190, UI_BTN_H_MD);
        lv_obj_align(s_data.open_door_btn, LV_ALIGN_TOP_LEFT, 32, 8);
    }

    s_data.list = lv_obj_create(s_data.list_layer);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.list, UI_TOUCH_SAFE_CONTENT_W, 420);
        lv_obj_align(s_data.list, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 138);
    } else {
        lv_obj_set_size(s_data.list, 960, 440);
        lv_obj_align(s_data.list, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
    lv_obj_set_style_bg_color(s_data.list, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_data.list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_data.list, LV_FLEX_FLOW_COLUMN);

    s_data.empty_label = lv_label_create(s_data.list_layer);
    lv_obj_align(s_data.empty_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.empty_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.empty_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(s_data.empty_label, "暂无注册用户");

    s_data.list_status_label = lv_label_create(s_data.list_layer);
    lv_obj_set_width(s_data.list_status_label, 900);
    lv_obj_align(s_data.list_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(s_data.list_status_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.list_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.list_status_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(s_data.list_status_label, "");
}

static void create_confirm_overlay(void)
{
    s_data.confirm_overlay = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.confirm_overlay, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_align(s_data.confirm_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_data.confirm_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_data.confirm_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.confirm_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_data.confirm_overlay);
    lv_obj_set_size(panel, 520, 300);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, UI_COLOR_DANGER, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_data.confirm_text = lv_label_create(panel);
    lv_obj_set_width(s_data.confirm_text, 450);
    lv_obj_align(s_data.confirm_text, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_font(s_data.confirm_text, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.confirm_text, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.confirm_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.confirm_text, LV_LABEL_LONG_WRAP);

    lv_obj_t *confirm_parent = ui_touch_is_damaged_mode() ? s_data.confirm_overlay : panel;

    s_data.confirm_delete_btn = create_text_btn(confirm_parent, LV_SYMBOL_TRASH " 删除",
                                                ui_style_get_btn_danger_large(),
                                                confirm_delete_event_cb, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.confirm_delete_btn, 0, 430, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.confirm_delete_btn, 170, UI_BTN_H_MD);
        lv_obj_align(s_data.confirm_delete_btn, LV_ALIGN_BOTTOM_MID, -90, -24);
    }

    s_data.confirm_cancel_btn = create_text_btn(confirm_parent, "取消",
                                                ui_style_get_btn_secondary_large(),
                                                confirm_cancel_event_cb, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.confirm_cancel_btn, 1, 430, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.confirm_cancel_btn, 170, UI_BTN_H_MD);
        lv_obj_align(s_data.confirm_cancel_btn, LV_ALIGN_BOTTOM_MID, 90, -24);
    }
}

void screen_admin_create(void)
{
    ESP_LOGI(TAG, "Creating admin screen...");
    memset(&s_data, 0, sizeof(s_data));

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_data.screen, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    create_header();
    create_pin_layer();
    create_list_layer();
    create_confirm_overlay();
    show_pin_layer();
    ESP_LOGI(TAG, "Admin screen created");
}

lv_obj_t *screen_admin_get_screen(void)
{
    return s_data.screen;
}

void screen_admin_start(void)
{
    clear_list();
    s_data.open_door_pending = false;
    lv_obj_clear_state(s_data.open_door_btn, LV_STATE_DISABLED);
    lv_textarea_set_text(s_data.pin_ta, "");
    lv_label_set_text(s_data.pin_status_label, "");
    s_data.delete_employee_id[0] = '\0';
    show_pin_layer();
}

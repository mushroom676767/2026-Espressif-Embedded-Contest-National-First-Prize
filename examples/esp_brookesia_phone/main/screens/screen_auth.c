/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authentication screen for FOD Smart Tool Cabinet.
 * Face and NFC flows are 60-second wait sessions in this stage.
 */

#include "screen_auth.h"
#include "auth_camera_preview.h"
#include "face_recognition_service.h"
#include "face_model_storage.h"
#include "nfc_manager.h"
#include "ppe_detection_service.h"
#include "user_manager.h"
#include "screen_usb_preview.h"
#include "ui_theme.h"
#include "ui_touch_layout.h"
#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "screen_auth";

#define AUTH_STEP_COUNT             3
#define STEP_PANEL_W                300
#define STEP_PANEL_H                380
#define AUTH_HEADER_H               UI_HEADER_H
#define AUTH_SESSION_TIMEOUT_MS     60000
#define AUTH_SESSION_TIMER_MS       1000
#define SAFETY_PANEL_W              900
#define SAFETY_PANEL_H              112
#define SAFETY_PANEL_BOTTOM_GAP     12
#define SAFETY_PANEL_TEXT_W         640
#define SAFETY_CONFIRM_BTN_W        180

// ============================================================================
// Legacy step strings
// ============================================================================
static const char *s_step_names[AUTH_STEP_COUNT] = {
    "NFC卡",
    "人脸识别",
    "安全检测"
};

static const char *s_step_icons_idle[AUTH_STEP_COUNT] = {
    LV_SYMBOL_REFRESH,
    LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_BELL
};

static const char *s_step_icons_ok[AUTH_STEP_COUNT] = {
    LV_SYMBOL_OK,
    LV_SYMBOL_OK,
    LV_SYMBOL_OK
};

static const char *s_step_icons_fail[AUTH_STEP_COUNT] = {
    LV_SYMBOL_CLOSE,
    LV_SYMBOL_CLOSE,
    LV_SYMBOL_CLOSE
};

// ============================================================================
// Internal Data
// ============================================================================

struct step_panel_s {
    lv_obj_t *panel;
    lv_obj_t *icon_label;
    lv_obj_t *name_label;
    lv_obj_t *state_label;
    lv_obj_t *info_label;
    auth_step_state_t state;
};

struct screen_auth_data_s {
    lv_obj_t *screen;
    lv_obj_t *header;
    lv_obj_t *title_label;
    lv_obj_t *countdown_label;
    lv_obj_t *cancel_btn;

    lv_obj_t *face_layer;
    lv_obj_t *face_canvas;
    lv_obj_t *face_status_label;

    lv_obj_t *nfc_layer;
    lv_obj_t *nfc_status_label;
    lv_obj_t *nfc_hint_label;

    lv_obj_t *safety_layer;
    lv_obj_t *safety_canvas;
    lv_obj_t *safety_status_label;
    lv_obj_t *safety_hint_label;
    lv_obj_t *safety_confirm_btn;
    char pending_operator[USER_DISPLAY_NAME_MAX_LEN];
    uint32_t safety_session_id;

    lv_obj_t *legacy_layer;
    lv_obj_t *operator_label;
    struct step_panel_s steps[AUTH_STEP_COUNT];

    lv_timer_t *session_timer;
    auth_type_t active_type;
    bool session_active;
    int32_t remaining_ms;
};

static struct screen_auth_data_s s_data;

static void start_safety_preview_for_operator(const char *display_name);
static void start_safety_model_check(void);

typedef struct {
    face_recognition_event_t event;
} pending_face_event_t;

typedef struct {
    nfc_event_t event;
} pending_nfc_event_t;

typedef struct {
    ppe_detection_event_t event;
    uint32_t session_id;
} pending_ppe_event_t;

// ============================================================================
// Internal helpers
// ============================================================================

static lv_color_t get_step_state_color(auth_step_state_t state)
{
    switch (state) {
        case AUTH_STEP_IDLE:        return UI_COLOR_TEXT_HINT;
        case AUTH_STEP_WAITING:     return UI_COLOR_PRIMARY;
        case AUTH_STEP_PROCESSING:  return UI_COLOR_WARNING;
        case AUTH_STEP_SUCCESS:     return UI_COLOR_SUCCESS;
        case AUTH_STEP_FAILED:      return UI_COLOR_DANGER;
        default:                    return UI_COLOR_TEXT_HINT;
    }
}

static const char *get_step_state_text(auth_step_state_t state)
{
    switch (state) {
        case AUTH_STEP_IDLE:        return "等待";
        case AUTH_STEP_WAITING:     return "就绪";
        case AUTH_STEP_PROCESSING:  return "处理中...";
        case AUTH_STEP_SUCCESS:     return "已通过";
        case AUTH_STEP_FAILED:      return "未通过";
        default:                    return "";
    }
}

static void update_step_ui(uint8_t step_idx)
{
    if (step_idx >= AUTH_STEP_COUNT) {
        return;
    }

    struct step_panel_s *sp = &s_data.steps[step_idx];
    if (sp->panel == NULL) {
        return;
    }

    lv_color_t color = get_step_state_color(sp->state);
    const char *icon = s_step_icons_idle[step_idx];
    if (sp->state == AUTH_STEP_SUCCESS) {
        icon = s_step_icons_ok[step_idx];
    } else if (sp->state == AUTH_STEP_FAILED) {
        icon = s_step_icons_fail[step_idx];
    }

    lv_label_set_text(sp->icon_label, icon);
    lv_obj_set_style_text_color(sp->icon_label, color, LV_PART_MAIN);
    lv_label_set_text(sp->state_label, get_step_state_text(sp->state));
    lv_obj_set_style_text_color(sp->state_label, color, LV_PART_MAIN);

    if (sp->state == AUTH_STEP_PROCESSING) {
        lv_obj_set_style_border_color(sp->panel, UI_COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_border_width(sp->panel, 2, LV_PART_MAIN);
    } else if (sp->state == AUTH_STEP_SUCCESS) {
        lv_obj_set_style_border_color(sp->panel, UI_COLOR_SUCCESS, LV_PART_MAIN);
        lv_obj_set_style_border_width(sp->panel, 2, LV_PART_MAIN);
    } else if (sp->state == AUTH_STEP_FAILED) {
        lv_obj_set_style_border_color(sp->panel, UI_COLOR_DANGER, LV_PART_MAIN);
        lv_obj_set_style_border_width(sp->panel, 2, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_color(sp->panel, UI_COLOR_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_width(sp->panel, 1, LV_PART_MAIN);
    }
}

static void update_countdown_label(void)
{
    if (s_data.countdown_label == NULL) {
        return;
    }

    if (!s_data.session_active) {
        lv_label_set_text(s_data.countdown_label, "");
        return;
    }

    int32_t seconds = (s_data.remaining_ms + 999) / 1000;
    if (seconds < 0) {
        seconds = 0;
    }
    lv_label_set_text_fmt(s_data.countdown_label, "%02lds", (long)seconds);
}

static void hide_all_layers(void)
{
    if (s_data.face_layer != NULL) {
        lv_obj_add_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.nfc_layer != NULL) {
        lv_obj_add_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.safety_layer != NULL) {
        lv_obj_add_flag(s_data.safety_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.legacy_layer != NULL) {
        lv_obj_add_flag(s_data.legacy_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_legacy_layer(void)
{
    hide_all_layers();
    lv_label_set_text(s_data.title_label, "身份验证");
    lv_label_set_text(s_data.countdown_label, "");
    lv_obj_clear_flag(s_data.legacy_layer, LV_OBJ_FLAG_HIDDEN);
}

static void set_face_status_text(const char *text)
{
    if (s_data.face_status_label == NULL) {
        return;
    }

    lv_label_set_text(s_data.face_status_label, text != NULL ? text : "");
    lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
}

static void set_nfc_status_text(const char *text, lv_color_t color)
{
    if (s_data.nfc_status_label == NULL) {
        return;
    }

    lv_label_set_text(s_data.nfc_status_label, text != NULL ? text : "");
    lv_obj_set_style_text_color(s_data.nfc_status_label, color, LV_PART_MAIN);
}

static void set_safety_status(const char *status, const char *hint, lv_color_t color, bool show_confirm)
{
    lv_label_set_text(s_data.safety_status_label, status != NULL ? status : "");
    lv_obj_set_style_text_color(s_data.safety_status_label, color, LV_PART_MAIN);
    lv_label_set_text(s_data.safety_hint_label, hint != NULL ? hint : "");
    if (show_confirm) {
        lv_obj_clear_flag(s_data.safety_confirm_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_data.safety_confirm_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static esp_err_t start_safety_camera_preview(void)
{
    if (s_data.safety_canvas == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    auth_camera_preview_set_frame_callback(NULL, NULL);
    return auth_camera_preview_start(s_data.safety_canvas, NULL);
}

static void stop_wait_session(bool show_legacy)
{
    bool was_face = s_data.session_active && s_data.active_type == AUTH_TYPE_FACE;
    bool was_nfc = s_data.session_active && s_data.active_type == AUTH_TYPE_NFC;
    bool was_safety = s_data.session_active && s_data.active_type == AUTH_TYPE_SAFETY_CHECK;

    if (was_safety) {
        s_data.safety_session_id++;
    }

    s_data.session_active = false;
    s_data.active_type = AUTH_TYPE_NONE;
    s_data.remaining_ms = 0;
    update_countdown_label();

    if (s_data.session_timer != NULL) {
        lv_timer_pause(s_data.session_timer);
    }

    if (was_face) {
        face_recognition_cancel();
        auth_camera_preview_stop();
    }
    if (was_nfc) {
        nfc_manager_set_event_callback(NULL, NULL);
    }
    if (was_safety) {
        if (ppe_detection_is_active()) {
            ppe_detection_cancel();
        } else {
            auth_camera_preview_stop_and_release_idle_resources();
        }
    }

    if (show_legacy) {
        show_legacy_layer();
    }
}

static void finish_wait_session(const char *reason)
{
    ESP_LOGI(TAG, "Auth wait session finished: %s", reason);
    stop_wait_session(false);
    ui_manager_goto_screen(SCREEN_STANDBY);
}

static void session_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_data.session_active) {
        return;
    }

    s_data.remaining_ms -= AUTH_SESSION_TIMER_MS;
    update_countdown_label();

    if (s_data.remaining_ms <= 0) {
        finish_wait_session("timeout");
    }
}

static void cancel_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Auth cancelled by user");
    finish_wait_session("cancel");
}

static void auth_nfc_async_cb(void *user_data)
{
    pending_nfc_event_t *pending = (pending_nfc_event_t *)user_data;
    if (pending == NULL) {
        return;
    }

    nfc_event_t event = pending->event;
    free(pending);

    if (!s_data.session_active || s_data.active_type != AUTH_TYPE_NFC) {
        nfc_manager_ack_event();
        return;
    }

    char verify_status[48];
    snprintf(verify_status, sizeof(verify_status), "正在验证NFC卡（%u字节UID）",
             (unsigned)event.raw_len);
    set_nfc_status_text(verify_status, UI_COLOR_WARNING);

    user_record_t record = {};
    esp_err_t ret = user_manager_find_by_nfc_card(event.card_hex, &record);
    if (ret == ESP_OK) {
        char display_name[USER_DISPLAY_NAME_MAX_LEN];
        user_manager_format_display_name(&record, display_name, sizeof(display_name));
        set_nfc_status_text("NFC卡验证通过", UI_COLOR_SUCCESS);
        stop_wait_session(false);
        nfc_manager_ack_event();
        start_safety_preview_for_operator(display_name);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        set_nfc_status_text("卡片未注册", UI_COLOR_DANGER);
        if (s_data.nfc_hint_label != NULL) {
            lv_label_set_text_fmt(s_data.nfc_hint_label, "UID：%s（%u字节）",
                                  event.card_hex, (unsigned)event.raw_len);
        }
        nfc_manager_ack_event();
    } else {
        ESP_LOGE(TAG, "NFC lookup failed: %s", esp_err_to_name(ret));
        set_nfc_status_text("NFC查询失败", UI_COLOR_DANGER);
        nfc_manager_ack_event();
    }
}

static void auth_nfc_event_cb(const nfc_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    pending_nfc_event_t *pending = (pending_nfc_event_t *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "No memory to queue NFC auth event");
        nfc_manager_ack_event();
        return;
    }
    pending->event = *event;

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for NFC auth event: %s", esp_err_to_name(lock_ret));
        free(pending);
        nfc_manager_ack_event();
        return;
    }

    lv_res_t async_ret = lv_async_call(auth_nfc_async_cb, pending);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue NFC auth event");
        free(pending);
        nfc_manager_ack_event();
    }
}

static void ppe_async_cb(void *user_data)
{
    pending_ppe_event_t *pending = (pending_ppe_event_t *)user_data;
    if (pending == NULL) {
        return;
    }
    uint32_t session_id = pending->session_id;
    ppe_detection_event_t event = pending->event;

    if (session_id != s_data.safety_session_id || s_data.active_type != AUTH_TYPE_SAFETY_CHECK) {
        free(pending);
        return;
    }

    free(pending);

    switch (event.type) {
        case PPE_EVT_STARTED:
            set_safety_status("正在采集安全检测画面...", "请正对摄像头, 确保头盔和护目镜可见",
                              UI_COLOR_WARNING, false);
            break;
        case PPE_EVT_FRAME_READY:
            set_safety_status("正在检测头盔和护目镜...", "", UI_COLOR_WARNING, false);
            break;
        case PPE_EVT_PASS:
            set_safety_status("安全检测通过", "", UI_COLOR_SUCCESS, false);
            stop_wait_session(false);
            screen_usb_preview_start(s_data.pending_operator, AUTH_TYPE_SAFETY_CHECK);
            ui_manager_goto_screen(SCREEN_USB_PREVIEW);
            break;
        case PPE_EVT_MISSING_HELMET:
        case PPE_EVT_MISSING_GOGGLES:
        case PPE_EVT_MISSING_BOTH:
            start_safety_camera_preview();
            set_safety_status(event.message, "请佩戴缺少的装备, 然后点击检测.",
                              UI_COLOR_DANGER, true);
            break;
        case PPE_EVT_ERROR:
        default:
            start_safety_camera_preview();
            set_safety_status("安全检测异常, 请重试", "点击检测重试.",
                              UI_COLOR_DANGER, true);
            break;
    }
}

static void ppe_event_cb(const ppe_detection_event_t *event, void *user_ctx)
{
    if (event == NULL) {
        return;
    }

    uint32_t session_id = (uint32_t)(uintptr_t)user_ctx;
    if (session_id == 0) {
        ESP_LOGW(TAG, "Drop PPE event without safety session token");
        return;
    }

    pending_ppe_event_t *pending = (pending_ppe_event_t *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "No memory to queue PPE event");
        return;
    }
    pending->event = *event;
    pending->session_id = session_id;

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for PPE event: %s", esp_err_to_name(lock_ret));
        free(pending);
        return;
    }
    lv_res_t async_ret = lv_async_call(ppe_async_cb, pending);
    esp_lv_adapter_unlock();
    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue PPE event");
        free(pending);
    }
}

static void auth_face_async_cb(void *user_data)
{
    pending_face_event_t *pending = (pending_face_event_t *)user_data;
    if (pending == NULL) {
        return;
    }

    face_recognition_event_t event = pending->event;
    free(pending);

    if (!s_data.session_active || s_data.active_type != AUTH_TYPE_FACE) {
        return;
    }

    switch (event.type) {
        case FACE_REC_EVT_NO_FACE:
            set_face_status_text("未检测到人脸");
            break;
        case FACE_REC_EVT_MULTI_FACE:
            set_face_status_text("检测到多张人脸");
            break;
        case FACE_REC_EVT_NO_USER:
            set_face_status_text("人脸未注册");
            break;
        case FACE_REC_EVT_PROGRESS:
            if (event.name[0] != '\0') {
                char display_name[USER_DISPLAY_NAME_MAX_LEN];
                user_manager_format_identity(event.employee_id, event.name,
                                             display_name, sizeof(display_name));
                lv_label_set_text_fmt(s_data.face_status_label,
                                      "正在确认 %s  %u/2",
                                      display_name,
                                      event.sample_count);
            } else {
                lv_label_set_text(s_data.face_status_label, "正在确认人脸匹配");
            }
            lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case FACE_REC_EVT_AUTH_MATCH:
        {
            char display_name[USER_DISPLAY_NAME_MAX_LEN];
            user_manager_format_identity(event.employee_id, event.name,
                                         display_name, sizeof(display_name));
            set_face_status_text("人脸验证通过");
            stop_wait_session(false);
            start_safety_preview_for_operator(display_name);
            break;
        }
        case FACE_REC_EVT_ERROR:
            set_face_status_text(event.message[0] != '\0' ? event.message : "人脸识别异常");
            break;
        default:
            break;
    }
}

static void auth_face_event_cb(const face_recognition_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    pending_face_event_t *pending = (pending_face_event_t *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "No memory to queue face auth event");
        return;
    }
    pending->event = *event;

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for face auth event: %s", esp_err_to_name(lock_ret));
        free(pending);
        return;
    }

    lv_res_t async_ret = lv_async_call(auth_face_async_cb, pending);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue face auth event");
        free(pending);
    }
}

static void screen_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_UNLOADED) {
        stop_wait_session(false);
    }
}

// ============================================================================
// Layer creation helpers
// ============================================================================

static void safety_confirm_event_cb(lv_event_t *e);

static void create_header(void)
{
    s_data.header = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.header, UI_SCREEN_WIDTH, AUTH_HEADER_H);
    lv_obj_align(s_data.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.header, LV_OBJ_FLAG_SCROLLABLE);

    s_data.countdown_label = lv_label_create(s_data.header);
    lv_obj_set_width(s_data.countdown_label, 120);
    lv_obj_align(s_data.countdown_label, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_set_style_text_font(s_data.countdown_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.countdown_label, UI_COLOR_WARNING, LV_PART_MAIN);
    lv_label_set_text(s_data.countdown_label, "");

    s_data.title_label = lv_label_create(s_data.header);
    lv_obj_align(s_data.title_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.title_label, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(s_data.title_label, "身份验证");

    s_data.cancel_btn = ui_create_btn_secondary_large(s_data.header);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.cancel_btn, UI_TOUCH_SAFE_PAD, 8, UI_TOUCH_SAFE_CONTENT_W, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.cancel_btn, 146, UI_BTN_H_MD);
        lv_obj_align(s_data.cancel_btn, LV_ALIGN_RIGHT_MID, -18, 0);
    }
    lv_obj_t *cancel_label = lv_label_create(s_data.cancel_btn);
    lv_obj_set_style_text_font(cancel_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE " 取消");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(s_data.cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

static void create_face_layer(void)
{
    s_data.face_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.face_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - AUTH_HEADER_H);
    lv_obj_align(s_data.face_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.face_layer, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.face_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.face_canvas = lv_canvas_create(s_data.face_layer);
    lv_obj_align(s_data.face_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_data.face_canvas, LV_OBJ_FLAG_SCROLLABLE);

    s_data.face_status_label = lv_label_create(s_data.face_layer);
    lv_obj_set_width(s_data.face_status_label, 760);
    lv_obj_align(s_data.face_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.face_status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.face_status_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.face_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.face_status_label, "正在启动摄像头...");

    lv_obj_t *hint = lv_label_create(s_data.face_layer);
    lv_obj_set_width(hint, 760);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_text_font(hint, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "请正对摄像头并保持稳定");

    lv_obj_add_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_nfc_layer(void)
{
    s_data.nfc_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.nfc_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - AUTH_HEADER_H);
    lv_obj_align(s_data.nfc_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.nfc_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.nfc_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.nfc_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.nfc_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_data.nfc_layer);
    lv_obj_set_size(panel, 600, 340);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, UI_COLOR_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(panel);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);

    s_data.nfc_status_label = lv_label_create(panel);
    lv_obj_set_width(s_data.nfc_status_label, 520);
    lv_obj_align(s_data.nfc_status_label, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_text_font(s_data.nfc_status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.nfc_status_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.nfc_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.nfc_status_label, "等待NFC卡");

    s_data.nfc_hint_label = lv_label_create(panel);
    lv_obj_set_width(s_data.nfc_hint_label, 520);
    lv_obj_align(s_data.nfc_hint_label, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_style_text_font(s_data.nfc_hint_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.nfc_hint_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.nfc_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.nfc_hint_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.nfc_hint_label, "请将员工卡靠近读卡器");

    lv_obj_add_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_safety_layer(void)
{
    s_data.safety_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.safety_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - AUTH_HEADER_H);
    lv_obj_align(s_data.safety_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.safety_layer, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.safety_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.safety_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.safety_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.safety_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.safety_canvas = lv_canvas_create(s_data.safety_layer);
    lv_obj_align(s_data.safety_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_data.safety_canvas, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_data.safety_layer);
    lv_obj_set_size(panel, SAFETY_PANEL_W, SAFETY_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -SAFETY_PANEL_BOTTOM_GAP);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, UI_COLOR_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_data.safety_status_label = lv_label_create(panel);
    lv_obj_set_width(s_data.safety_status_label, SAFETY_PANEL_TEXT_W);
    lv_obj_align(s_data.safety_status_label, LV_ALIGN_TOP_LEFT, 18, 12);
    lv_obj_set_style_text_font(s_data.safety_status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.safety_status_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.safety_status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.safety_status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.safety_status_label, "正在启动安全检测...");

    s_data.safety_hint_label = lv_label_create(panel);
    lv_obj_set_width(s_data.safety_hint_label, SAFETY_PANEL_TEXT_W);
    lv_obj_align(s_data.safety_hint_label, LV_ALIGN_TOP_LEFT, 18, 62);
    lv_obj_set_style_text_font(s_data.safety_hint_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.safety_hint_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.safety_hint_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.safety_hint_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.safety_hint_label, "请正对摄像头, 确保头盔和护目镜可见");

    lv_obj_t *safety_confirm_parent = ui_touch_is_damaged_mode() ? s_data.safety_layer : panel;
    s_data.safety_confirm_btn = ui_create_btn_primary_large(safety_confirm_parent);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.safety_confirm_btn, 0, 360, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.safety_confirm_btn, SAFETY_CONFIRM_BTN_W, UI_BTN_H_SAFE);
        lv_obj_align(s_data.safety_confirm_btn, LV_ALIGN_RIGHT_MID, -18, 0);
    }
    lv_obj_t *confirm_label = lv_label_create(s_data.safety_confirm_btn);
    lv_obj_set_style_text_font(confirm_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(confirm_label, LV_SYMBOL_OK " 检测");
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(s_data.safety_confirm_btn, safety_confirm_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_data.safety_confirm_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_data.safety_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_step_panel(uint8_t idx, lv_obj_t *parent, int32_t x_offset)
{
    struct step_panel_s *sp = &s_data.steps[idx];
    sp->state = AUTH_STEP_IDLE;

    sp->panel = lv_obj_create(parent);
    lv_obj_set_size(sp->panel, STEP_PANEL_W, STEP_PANEL_H);
    lv_obj_align(sp->panel, LV_ALIGN_TOP_MID, x_offset, 80);
    lv_obj_set_style_bg_color(sp->panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(sp->panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(sp->panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sp->panel, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sp->panel, UI_PADDING_MD, LV_PART_MAIN);
    lv_obj_clear_flag(sp->panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_label_create(sp->panel);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(badge, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(badge, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text_fmt(badge, "第%d步", idx + 1);

    sp->icon_label = lv_label_create(sp->panel);
    lv_obj_align(sp->icon_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_font(sp->icon_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(sp->icon_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(sp->icon_label, s_step_icons_idle[idx]);

    sp->name_label = lv_label_create(sp->panel);
    lv_obj_align(sp->name_label, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_text_font(sp->name_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(sp->name_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(sp->name_label, s_step_names[idx]);

    sp->state_label = lv_label_create(sp->panel);
    lv_obj_align(sp->state_label, LV_ALIGN_TOP_MID, 0, 154);
    lv_obj_set_style_text_font(sp->state_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(sp->state_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(sp->state_label, "等待");

    sp->info_label = lv_label_create(sp->panel);
    lv_obj_align(sp->info_label, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_text_font(sp->info_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(sp->info_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_label_set_text(sp->info_label, "");

    lv_obj_t *sep = lv_obj_create(sp->panel);
    lv_obj_set_size(sep, STEP_PANEL_W - 40, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_set_style_bg_color(sep, UI_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    lv_obj_t *desc = lv_label_create(sp->panel);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 252);
    lv_obj_set_style_text_font(desc, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(desc, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, STEP_PANEL_W - 32);

    switch (idx) {
        case 0:
            lv_label_set_text(desc, "请在读卡器上刷员工NFC卡");
            break;
        case 1:
            lv_label_set_text(desc, "请正对摄像头进行身份验证");
            break;
        case 2:
            lv_label_set_text(desc, "检测安全装备: 头盔/护目镜");
            break;
        default:
            break;
    }
}

static void create_legacy_layer(void)
{
    s_data.legacy_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.legacy_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - AUTH_HEADER_H);
    lv_obj_align(s_data.legacy_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.legacy_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.legacy_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.legacy_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.legacy_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.operator_label = lv_label_create(s_data.legacy_layer);
    lv_obj_align(s_data.operator_label, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_text_font(s_data.operator_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.operator_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    lv_label_set_text(s_data.operator_label, "");
    lv_obj_add_flag(s_data.operator_label, LV_OBJ_FLAG_HIDDEN);

    int32_t gap = (UI_SCREEN_WIDTH - STEP_PANEL_W * 3) / 4;
    create_step_panel(0, s_data.legacy_layer, -(STEP_PANEL_W + gap));
    create_step_panel(1, s_data.legacy_layer, 0);
    create_step_panel(2, s_data.legacy_layer, STEP_PANEL_W + gap);
}

// ============================================================================
// Public API
// ============================================================================

void screen_auth_create(void)
{
    ESP_LOGI(TAG, "Creating auth screen...");

    memset(&s_data, 0, sizeof(s_data));

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_data.screen, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_data.screen, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    create_header();
    create_face_layer();
    create_nfc_layer();
    create_safety_layer();
    create_legacy_layer();

    s_data.session_timer = lv_timer_create(session_timer_cb, AUTH_SESSION_TIMER_MS, NULL);
    lv_timer_pause(s_data.session_timer);

    show_legacy_layer();

    ESP_LOGI(TAG, "Auth screen created");
}

static void start_safety_preview_for_operator(const char *display_name)
{
    stop_wait_session(false);
    hide_all_layers();

    s_data.safety_session_id++;
    snprintf(s_data.pending_operator, sizeof(s_data.pending_operator), "%s",
             display_name != NULL ? display_name : "");
    s_data.active_type = AUTH_TYPE_SAFETY_CHECK;
    s_data.session_active = true;
    s_data.remaining_ms = AUTH_SESSION_TIMEOUT_MS;

    lv_label_set_text(s_data.title_label, "安全检测");
    lv_obj_clear_flag(s_data.safety_layer, LV_OBJ_FLAG_HIDDEN);
    update_countdown_label();
    lv_timer_reset(s_data.session_timer);
    lv_timer_resume(s_data.session_timer);

    esp_err_t ret = start_safety_camera_preview();
    if (ret == ESP_OK) {
        set_safety_status("安全检测已就绪", "请正对摄像头, 然后点击检测.",
                          UI_COLOR_PRIMARY, true);
    } else {
        ESP_LOGE(TAG, "Start safety camera preview failed: %s", esp_err_to_name(ret));
        set_safety_status("摄像头异常, 请重试", "点击检测重试.",
                          UI_COLOR_DANGER, true);
    }
}

static void start_safety_model_check(void)
{
    if (!s_data.session_active || s_data.active_type != AUTH_TYPE_SAFETY_CHECK) {
        return;
    }
    if (ppe_detection_is_active()) {
        return;
    }

    set_safety_status("正在检测头盔和护目镜...", "", UI_COLOR_WARNING, false);
    uint32_t session_id = s_data.safety_session_id;
    esp_err_t ret = ppe_detection_start(ppe_event_cb, (void *)(uintptr_t)session_id,
                                        s_data.safety_canvas);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start PPE detection failed: %s", esp_err_to_name(ret));
        start_safety_camera_preview();
        set_safety_status("安全检测异常, 请重试", "点击检测重试.",
                          UI_COLOR_DANGER, true);
    }
}

static void safety_confirm_event_cb(lv_event_t *e)
{
    (void)e;
    start_safety_model_check();
}

lv_obj_t *screen_auth_get_screen(void)
{
    return s_data.screen;
}

void screen_auth_start_wait(auth_type_t type)
{
    if (type != AUTH_TYPE_FACE && type != AUTH_TYPE_NFC) {
        ESP_LOGW(TAG, "Unsupported auth wait type: %d", type);
        return;
    }

    stop_wait_session(false);
    hide_all_layers();

    s_data.active_type = type;
    s_data.session_active = true;
    s_data.remaining_ms = AUTH_SESSION_TIMEOUT_MS;

    if (type == AUTH_TYPE_FACE) {
        lv_label_set_text(s_data.title_label, "人脸验证");
        lv_label_set_text(s_data.face_status_label, "正在启动摄像头...");
        lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);
        if (!face_model_storage_is_ready()) {
            lv_label_set_text_fmt(s_data.face_status_label,
                                  "人脸模型不可用: %s",
                                  face_model_storage_last_error());
        } else if (auth_camera_preview_start(s_data.face_canvas, s_data.face_status_label) != ESP_OK) {
            lv_label_set_text(s_data.face_status_label, "摄像头不可用");
            stop_wait_session(false);
        } else {
            esp_err_t ret = face_recognition_start_auth(auth_face_event_cb, NULL);
            if (ret == ESP_ERR_NOT_FOUND) {
                set_face_status_text("暂无已注册人脸, 请先注册.");
                stop_wait_session(false);
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Start face auth failed: %s", esp_err_to_name(ret));
                set_face_status_text("人脸识别不可用");
                stop_wait_session(false);
            }
        }
    } else {
        ESP_LOGI(TAG, "Starting NFC auth wait session");
        lv_label_set_text(s_data.title_label, "NFC验证");
        set_nfc_status_text("等待NFC卡", UI_COLOR_TEXT_PRIMARY);
        lv_label_set_text(s_data.nfc_hint_label, "请将员工卡靠近读卡器并保持不动");
        lv_obj_clear_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);
        if (!nfc_manager_is_ready()) {
            set_nfc_status_text("PN532读卡器不可用", UI_COLOR_DANGER);
            lv_label_set_text(s_data.nfc_hint_label,
                              "请检查供电、UART接线和HSU模式，然后重新启动设备");
            stop_wait_session(false);
        } else {
            nfc_manager_flush_input();
            nfc_manager_set_event_callback(auth_nfc_event_cb, NULL);
        }
    }

    update_countdown_label();
    lv_timer_reset(s_data.session_timer);
    lv_timer_resume(s_data.session_timer);
}

void screen_auth_cancel_wait(void)
{
    finish_wait_session("cancel");
}

void screen_auth_set_step(uint8_t step, auth_step_state_t state, const char *info)
{
    if (step >= AUTH_STEP_COUNT) {
        return;
    }

    stop_wait_session(false);
    show_legacy_layer();

    s_data.steps[step].state = state;
    if (info != NULL) {
        lv_label_set_text(s_data.steps[step].info_label, info);
    } else {
        lv_label_set_text(s_data.steps[step].info_label, "");
    }

    update_step_ui(step);
}

void screen_auth_reset(void)
{
    stop_wait_session(false);
    show_legacy_layer();

    for (uint8_t i = 0; i < AUTH_STEP_COUNT; i++) {
        s_data.steps[i].state = AUTH_STEP_IDLE;
        if (s_data.steps[i].info_label != NULL) {
            lv_label_set_text(s_data.steps[i].info_label, "");
        }
        update_step_ui(i);
    }

    lv_obj_add_flag(s_data.operator_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_data.operator_label, "");
}

void screen_auth_set_operator(const char *name)
{
    stop_wait_session(false);
    show_legacy_layer();

    if (name != NULL && strlen(name) > 0) {
        lv_label_set_text_fmt(s_data.operator_label, "欢迎, %s", name);
        lv_obj_clear_flag(s_data.operator_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_data.operator_label, LV_OBJ_FLAG_HIDDEN);
    }
}

struct screen_auth_data_s *screen_auth_get_data(void)
{
    return &s_data;
}

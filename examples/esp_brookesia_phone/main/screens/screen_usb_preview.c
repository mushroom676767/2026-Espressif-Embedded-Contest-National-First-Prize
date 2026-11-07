#include "screen_usb_preview.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "auth_camera_preview.h"
#include "cabinet_core.h"
#include "cabinet_door_signal.h"
#include "cabinet_imu.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "face_recognition_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_adapter_init.h"
#include "screen_operation.h"
#include "tool_detection_service.h"
#include "ui_theme.h"
#include "usb_uvc_camera_service.h"

static const char *TAG = "screen_usb_preview";

#define USB_PREVIEW_TASK_STACK_SIZE      (1024 * 8)
#define USB_PREVIEW_TASK_PRIORITY        5
#define USB_PREVIEW_LVGL_PAUSE_MS        2000
#define USB_PREVIEW_AUTH_IDLE_WAIT_MS    5000
#define USB_PREVIEW_START_RETRY_COUNT    8
#define USB_PREVIEW_START_RETRY_DELAY_MS 1000
#define USB_PREVIEW_LCD_W                UI_SCREEN_WIDTH
#define USB_PREVIEW_LCD_H                UI_SCREEN_HEIGHT
#define USB_PREVIEW_SNAPSHOT_PATH        "/sdcard/tool_detection_last.jpg"
#define USB_PREVIEW_IMU_POLL_MS          100

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status_label;
    lv_obj_t *retry_btn;
    lv_obj_t *close_btn;
    TaskHandle_t preview_task;
    esp_lcd_panel_handle_t panel;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    char operator_name[64];
    auth_type_t auth_type;
    bool start_pending;
    bool running;
    bool transitioning;
    bool cancel_requested;
    bool lvgl_paused;
    esp_err_t start_result;
    uint32_t frames_drawn;
    uint32_t last_frame_width;
    uint32_t last_frame_height;
    uint32_t session_id;
    int64_t last_stats_us;
} screen_usb_preview_data_t;

static screen_usb_preview_data_t s_data;
static void start_usb_preview_task(void);

static void prepare_operation_session(void)
{
    (void)cabinet_door_signal_lock();
    cabinet_core_start_operation(s_data.operator_name, s_data.auth_type);
    esp_err_t ret = cabinet_imu_start_open_detection();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start IMU open detection failed: %s", esp_err_to_name(ret));
    } else {
        ret = cabinet_door_signal_unlock_pulse();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Start five-second unlock pulse failed: %s", esp_err_to_name(ret));
            cabinet_imu_stop_open_detection();
        }
    }
    if (ret != ESP_OK) {
        (void)cabinet_door_signal_lock();
        cabinet_core_abort_operation();
    }
    s_data.start_result = ret;
}

static bool flow_cancelled(uint32_t session_id)
{
    return s_data.cancel_requested || session_id != s_data.session_id;
}

static void restart_pending_flow_async(void *user_data)
{
    (void)user_data;
    prepare_operation_session();
    start_usb_preview_task();
}

static void finish_preview_task(uint32_t session_id)
{
    bool restart_pending = s_data.start_pending && session_id != s_data.session_id;
    s_data.preview_task = NULL;

    if (restart_pending && esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
        lv_res_t ret = lv_async_call(restart_pending_flow_async, NULL);
        esp_lv_adapter_unlock();
        if (ret != LV_RES_OK) {
            ESP_LOGW(TAG, "Queue pending USB preview restart failed");
        }
    }
}

static void resume_lvgl_if_needed(void)
{
    if (!s_data.lvgl_paused) {
        return;
    }

    esp_err_t ret = esp_lv_adapter_resume();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Resume LVGL adapter failed: %s", esp_err_to_name(ret));
    }
    s_data.lvgl_paused = false;
}

static void set_status_text(const char *text)
{
    if (s_data.status_label == NULL) {
        return;
    }
    if (esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
        lv_label_set_text(s_data.status_label, text != NULL ? text : "");
        lv_obj_center(s_data.status_label);
        esp_lv_adapter_unlock();
    }
}

static void set_retry_visible(bool visible)
{
    if (s_data.retry_btn == NULL) {
        return;
    }
    if (esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
        if (visible) {
            lv_obj_clear_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
        }
        esp_lv_adapter_unlock();
    }
}

static esp_err_t wait_for_imu_state(uint32_t session_id,
                                    cabinet_imu_door_state_t desired_state)
{
    while (!flow_cancelled(session_id)) {
        cabinet_imu_status_t status;
        if (cabinet_imu_get_status(&status) == ESP_OK && status.sample_valid &&
            status.door_state == desired_state) {
            return ESP_OK;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(USB_PREVIEW_IMU_POLL_MS));
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t wait_for_fully_open(uint32_t session_id)
{
    set_status_text("箱门已解锁，请将箱盖完全打开并保持稳定");
    ESP_RETURN_ON_ERROR(wait_for_imu_state(session_id, CABINET_IMU_DOOR_FULLY_OPEN),
                        TAG, "Wait for fully open cabinet failed");
    (void)cabinet_door_signal_lock();
    cabinet_imu_stop_open_detection();
    ESP_LOGI(TAG, "IMU confirmed cabinet fully open; starting USB inventory window");
    return ESP_OK;
}

static void wait_for_auth_camera_idle(void)
{
    ESP_LOGI(TAG, "Stopping MIPI auth camera before USB preview");
    auth_camera_preview_stop_and_release_idle_resources();

    uint32_t waited_ms = 0;
    while (auth_camera_preview_is_running() && waited_ms < USB_PREVIEW_AUTH_IDLE_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }

    ESP_LOGI(TAG, "MIPI auth camera idle: running=%d waited_ms=%u",
             auth_camera_preview_is_running(), (unsigned int)waited_ms);

    esp_err_t face_ret = face_recognition_release_idle_resources();
    if (face_ret != ESP_OK && face_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Release idle face resources failed: %s", esp_err_to_name(face_ret));
    } else if (face_ret == ESP_ERR_INVALID_STATE) {
        /* A final face frame can still be returning from ESP-DL after the UI
         * has accepted the match. Give that task a short bounded drain window
         * before the hardware JPEG engine needs contiguous internal DMA RAM. */
        for (uint32_t face_wait_ms = 0;
             face_wait_ms < USB_PREVIEW_AUTH_IDLE_WAIT_MS;
             face_wait_ms += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
            face_ret = face_recognition_release_idle_resources();
            if (face_ret == ESP_OK) {
                break;
            }
        }
        if (face_ret != ESP_OK) {
            ESP_LOGW(TAG, "Face resources still busy before USB preview: %s",
                     esp_err_to_name(face_ret));
        }
    }
}

static void preview_frame_cb(const uint8_t *rgb565,
                             uint32_t width,
                             uint32_t height,
                             void *user_ctx)
{
    (void)user_ctx;

    if (!s_data.running || s_data.transitioning || s_data.panel == NULL || rgb565 == NULL) {
        return;
    }
    if (width > USB_PREVIEW_LCD_W || height > USB_PREVIEW_LCD_H) {
        ESP_LOGW(TAG, "USB preview frame is larger than LCD: %ux%u",
                 (unsigned int)width, (unsigned int)height);
        return;
    }
    if (s_data.frame_buffer == NULL ||
        s_data.frame_buffer_size < USB_PREVIEW_LCD_W * USB_PREVIEW_LCD_H * sizeof(uint16_t)) {
        return;
    }

    int x1 = (USB_PREVIEW_LCD_W - (int)width) / 2;
    int y1 = (USB_PREVIEW_LCD_H - (int)height) / 2;
    if (s_data.last_frame_width != width || s_data.last_frame_height != height) {
        /* Clear the letterbox only when the negotiated output geometry
         * changes. The decoded frame can then be drawn directly instead of
         * copying every row into another full-screen PSRAM buffer. */
        memset(s_data.frame_buffer, 0, s_data.frame_buffer_size);
        esp_err_t clear_ret = esp_lcd_panel_draw_bitmap(s_data.panel,
                                                        0,
                                                        0,
                                                        USB_PREVIEW_LCD_W,
                                                        USB_PREVIEW_LCD_H,
                                                        s_data.frame_buffer);
        if (clear_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB preview background clear failed: %s", esp_err_to_name(clear_ret));
            return;
        }
        s_data.last_frame_width = width;
        s_data.last_frame_height = height;
        ESP_LOGI(TAG, "USB preview frame %ux%u centered at %d,%d",
                 (unsigned int)width, (unsigned int)height, x1, y1);
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_data.panel,
                                               x1,
                                               y1,
                                               x1 + (int)width,
                                               y1 + (int)height,
                                               rgb565);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB direct LCD draw failed: %s", esp_err_to_name(ret));
        return;
    }

    ++s_data.frames_drawn;
    int64_t now_us = esp_timer_get_time();
    if (s_data.last_stats_us == 0) {
        s_data.last_stats_us = now_us;
    } else if (now_us - s_data.last_stats_us >= 1000000) {
        ESP_LOGI(TAG, "USB direct preview fps=%u source=%ux%u",
                 (unsigned int)s_data.frames_drawn,
                 (unsigned int)width,
                 (unsigned int)height);
        s_data.frames_drawn = 0;
        s_data.last_stats_us = now_us;
    }
}

static void cleanup_preview_resources(void)
{
    s_data.running = false;
    usb_uvc_camera_stop_preview();
    resume_lvgl_if_needed();
    if (s_data.frame_buffer != NULL) {
        heap_caps_free(s_data.frame_buffer);
        s_data.frame_buffer = NULL;
        s_data.frame_buffer_size = 0;
    }
    s_data.last_frame_width = 0;
    s_data.last_frame_height = 0;
}

static void transition_to_operation_screen(void)
{
    if (s_data.transitioning) {
        return;
    }
    s_data.transitioning = true;
    s_data.running = false;
    s_data.start_pending = false;
    resume_lvgl_if_needed();

    if (esp_lv_adapter_lock(portMAX_DELAY) == ESP_OK) {
        screen_operation_set_read_only(true);
        screen_operation_set_operator(s_data.operator_name);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "Tool inventory complete, entering read-only operation screen");
    ui_manager_goto_screen(SCREEN_OPERATION);
}

static void show_detection_failure(const char *message)
{
    remove(USB_PREVIEW_SNAPSHOT_PATH);
    set_status_text(message != NULL && message[0] != '\0'
                        ? message
                        : "工具检测异常，请重新检测");
    set_retry_visible(true);
    if (s_data.close_btn != NULL && esp_lv_adapter_lock(pdMS_TO_TICKS(200)) == ESP_OK) {
        lv_obj_clear_flag(s_data.close_btn, LV_OBJ_FLAG_HIDDEN);
        esp_lv_adapter_unlock();
    }
}

static void preview_flow_task(void *arg)
{
    uint32_t session_id = (uint32_t)(uintptr_t)arg;

    esp_err_t ret = ESP_OK;
    bool preview_started = false;
    bool fully_open_confirmed = false;
    const char *failure_message = NULL;
    s_data.panel = lvgl_adapter_get_panel_handle();
    s_data.frames_drawn = 0;
    s_data.last_stats_us = 0;

    if (s_data.panel == NULL) {
        failure_message = "显示设备异常，请重新检测";
        goto finish_preview;
    }

    ret = s_data.start_result;
    if (ret != ESP_OK) {
        failure_message = "IMU或门锁启动失败，请重新检测";
        goto finish_preview;
    }

    ret = wait_for_fully_open(session_id);
    if (ret != ESP_OK) {
        if (!flow_cancelled(session_id)) {
            failure_message = "箱门状态检测异常，请关闭箱盖后重试";
        }
        goto finish_preview;
    }
    fully_open_confirmed = true;

    set_status_text("正在启动USB摄像头...");
    wait_for_auth_camera_idle();

    s_data.frame_buffer_size = USB_PREVIEW_LCD_W * USB_PREVIEW_LCD_H * sizeof(uint16_t);
    s_data.frame_buffer = heap_caps_aligned_alloc(64, s_data.frame_buffer_size,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_data.frame_buffer == NULL) {
        failure_message = "预览内存不足，请重新检测";
        goto finish_preview;
    }
    memset(s_data.frame_buffer, 0, s_data.frame_buffer_size);

    if (usb_uvc_camera_preview_active()) {
        usb_uvc_camera_stop_preview();
    }
    for (uint32_t attempt = 1;
         attempt <= USB_PREVIEW_START_RETRY_COUNT && !flow_cancelled(session_id);
         ++attempt) {
        ret = usb_uvc_camera_start_preview(preview_frame_cb, NULL);
        if (ret == ESP_OK) {
            preview_started = true;
            break;
        }
        ESP_LOGW(TAG, "Start USB preview %u/%u failed: %s",
                 (unsigned int)attempt,
                 (unsigned int)USB_PREVIEW_START_RETRY_COUNT,
                 esp_err_to_name(ret));
        usb_uvc_camera_stop_preview();
        if (attempt < USB_PREVIEW_START_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(USB_PREVIEW_START_RETRY_DELAY_MS));
        }
    }
    if (!preview_started) {
        failure_message = "USB摄像头启动失败，请重新检测";
        goto finish_preview;
    }

    ret = esp_lv_adapter_pause(USB_PREVIEW_LVGL_PAUSE_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Pause LVGL for USB preview failed: %s", esp_err_to_name(ret));
        failure_message = "预览显示启动失败，请重新检测";
        goto finish_preview;
    }
    s_data.lvgl_paused = true;
    s_data.running = true;

    ret = cabinet_core_begin_capture_wait();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start tool capture window failed: %s", esp_err_to_name(ret));
        failure_message = "工具取放计时启动失败，请关闭箱盖后重试";
        goto finish_preview;
    }

    uint32_t remain_ms = cabinet_core_get_capture_wait_remaining_ms();
    while (!flow_cancelled(session_id) && remain_ms > 0) {
        uint32_t wait_ms = remain_ms > 250 ? 250 : remain_ms;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        remain_ms = cabinet_core_get_capture_wait_remaining_ms();
    }

finish_preview:
    cleanup_preview_resources();

    if (flow_cancelled(session_id)) {
        (void)cabinet_door_signal_lock();
        cabinet_core_abort_operation();
        cabinet_imu_stop_open_detection();
        finish_preview_task(session_id);
        vTaskDelete(NULL);
        return;
    }
    if (failure_message != NULL) {
        if (!fully_open_confirmed) {
            (void)cabinet_door_signal_lock();
            cabinet_imu_stop_open_detection();
            cabinet_core_abort_operation();
            set_status_text(failure_message);
            set_retry_visible(true);
        } else {
            (void)cabinet_core_begin_close_grace(NULL, NULL, esp_timer_get_time(), false);
            show_detection_failure(failure_message);
        }
        finish_preview_task(session_id);
        vTaskDelete(NULL);
        return;
    }

    set_status_text("正在拍摄并盘点工具...");
    tool_detection_event_t detection = {0};
    ret = tool_detection_run_sync_with_snapshot(USB_PREVIEW_SNAPSHOT_PATH, &detection);
    if (flow_cancelled(session_id)) {
        remove(USB_PREVIEW_SNAPSHOT_PATH);
        finish_preview_task(session_id);
        vTaskDelete(NULL);
        return;
    }
    if (ret == ESP_OK) {
        ret = cabinet_core_apply_vision_inventory(&detection);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Tool inventory failed: %s message=%s",
                 esp_err_to_name(ret), detection.message);
        (void)cabinet_core_begin_close_grace(NULL, NULL, esp_timer_get_time(), false);
        show_detection_failure(detection.message);
        finish_preview_task(session_id);
        vTaskDelete(NULL);
        return;
    }

    ret = cabinet_core_begin_close_grace(detection.snapshot_local_path,
                                         detection.captured_at,
                                         detection.captured_monotonic_us,
                                         true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start door close grace failed: %s", esp_err_to_name(ret));
        show_detection_failure("关门倒计时启动失败");
        finish_preview_task(session_id);
        vTaskDelete(NULL);
        return;
    }
    transition_to_operation_screen();
    finish_preview_task(session_id);
    vTaskDelete(NULL);
}

static void start_usb_preview_task(void)
{
    if (!s_data.start_pending || s_data.preview_task != NULL || s_data.transitioning) {
        return;
    }

    s_data.start_pending = false;
    s_data.cancel_requested = false;
    s_data.transitioning = false;
    if (s_data.retry_btn != NULL) {
        lv_obj_add_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(preview_flow_task,
                                                  "usb_inventory_flow",
                                                  USB_PREVIEW_TASK_STACK_SIZE,
                                                  (void *)(uintptr_t)s_data.session_id,
                                                  USB_PREVIEW_TASK_PRIORITY,
                                                  &s_data.preview_task,
                                                  tskNO_AFFINITY);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Create USB inventory task failed");
        s_data.preview_task = NULL;
        (void)cabinet_door_signal_lock();
        cabinet_imu_stop_open_detection();
        cabinet_core_abort_operation();
        lv_label_set_text(s_data.status_label, "盘点任务启动失败，请重新检测");
        lv_obj_clear_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void retry_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_data.preview_task != NULL || s_data.transitioning || tool_detection_is_active()) {
        return;
    }
    s_data.start_pending = true;
    cabinet_core_cancel_close_grace();
    s_data.cancel_requested = false;
    prepare_operation_session();
    lv_label_set_text(s_data.status_label, "箱门已解锁，请将箱盖完全打开并保持稳定");
    lv_obj_add_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_data.close_btn != NULL) {
        lv_obj_add_flag(s_data.close_btn, LV_OBJ_FLAG_HIDDEN);
    }
    start_usb_preview_task();
}

static void close_btn_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = cabinet_core_confirm_close_async();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failure-page close ignored: %s", esp_err_to_name(ret));
    }
}

static void screen_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        start_usb_preview_task();
    } else if (code == LV_EVENT_SCREEN_UNLOADED && !s_data.transitioning) {
        s_data.cancel_requested = true;
        s_data.running = false;
        s_data.start_pending = false;
        tool_detection_cancel();
        (void)cabinet_door_signal_lock();
        cabinet_core_abort_operation();
        cabinet_imu_stop_open_detection();
        if (s_data.preview_task != NULL) {
            xTaskNotifyGive(s_data.preview_task);
        }
    }
}

void screen_usb_preview_create(void)
{
    memset(&s_data, 0, sizeof(s_data));
    s_data.auth_type = AUTH_TYPE_NONE;

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_data.screen, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_data.screen, screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_data.screen, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    s_data.status_label = lv_label_create(s_data.screen);
    lv_obj_set_width(s_data.status_label, 760);
    lv_obj_set_style_text_font(s_data.status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.status_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.status_label, "箱门已解锁，请将箱盖完全打开并保持稳定");
    lv_obj_center(s_data.status_label);

    s_data.retry_btn = ui_create_btn_primary_large(s_data.screen);
    lv_obj_set_size(s_data.retry_btn, 220, UI_BTN_H_LG);
    lv_obj_align(s_data.retry_btn, LV_ALIGN_CENTER, 0, 90);
    lv_obj_t *retry_label = lv_label_create(s_data.retry_btn);
    lv_obj_set_style_text_font(retry_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(retry_label, "重新检测");
    lv_obj_center(retry_label);
    lv_obj_add_event_cb(s_data.retry_btn, retry_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);

    s_data.close_btn = ui_create_btn_secondary_large(s_data.screen);
    lv_obj_set_size(s_data.close_btn, 220, UI_BTN_H_LG);
    lv_obj_align(s_data.close_btn, LV_ALIGN_CENTER, 0, 165);
    lv_obj_t *close_label = lv_label_create(s_data.close_btn);
    lv_obj_set_style_text_font(close_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(close_label, LV_SYMBOL_OK "  确认关门");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(s_data.close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_data.close_btn, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "USB tool inventory screen created");
}

lv_obj_t *screen_usb_preview_get_screen(void)
{
    return s_data.screen;
}

void screen_usb_preview_start(const char *operator_name, auth_type_t auth_type)
{
    bool defer_prepare = s_data.preview_task != NULL;
    if (defer_prepare) {
        s_data.cancel_requested = true;
        xTaskNotifyGive(s_data.preview_task);
    }
    s_data.session_id++;
    if (s_data.session_id == 0) {
        s_data.session_id = 1;
    }
    snprintf(s_data.operator_name, sizeof(s_data.operator_name), "%s",
             operator_name != NULL ? operator_name : "");
    s_data.auth_type = auth_type;
    s_data.start_pending = true;
    s_data.running = false;
    s_data.transitioning = false;
    s_data.cancel_requested = false;
    s_data.frames_drawn = 0;
    s_data.last_stats_us = 0;
    lv_label_set_text(s_data.status_label, "箱门已解锁，请将箱盖完全打开并保持稳定");
    lv_obj_add_flag(s_data.retry_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_data.close_btn, LV_OBJ_FLAG_HIDDEN);

    /* Freeze inventory, start open detection, and issue the bounded unlock pulse. */
    if (!defer_prepare) {
        prepare_operation_session();
    }
}

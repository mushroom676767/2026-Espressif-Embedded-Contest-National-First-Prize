/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight camera preview service for the authentication screen.
 */

#include "auth_camera_preview.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera/app_video.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_private/esp_cache_private.h"

static const char *TAG = "auth_camera_preview";

#define PREVIEW_TASK_STACK_SIZE     (1024 * 6)
#define PREVIEW_TASK_PRIORITY       4
#define PREVIEW_TASK_CORE           0
#define PREVIEW_CMD_QUEUE_LEN       4

#define ALIGN_UP_BY(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

typedef enum {
    PREVIEW_CMD_START = 0,
    PREVIEW_CMD_STOP,
} preview_cmd_type_t;

typedef struct {
    preview_cmd_type_t type;
    uint32_t generation;
    lv_obj_t *canvas;
    lv_obj_t *status_label;
    video_fmt_t format;
} preview_cmd_t;

typedef struct {
    QueueHandle_t cmd_queue;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stop_done;
    TaskHandle_t task;

    lv_obj_t *canvas;
    lv_obj_t *canvas_to_detach;
    lv_obj_t *status_label;
    uint32_t generation;
    bool active;
    bool video_hw_initialized;
    bool initialized;
    bool running;

    int video_fd;
    uint8_t *frame_buffers[EXAMPLE_CAM_BUF_NUM];
    uint32_t frame_buffer_count;
    size_t frame_buffer_size;
    video_fmt_t active_format;
    auth_camera_preview_frame_cb_t frame_cb;
    void *frame_cb_ctx;
} preview_state_t;

static preview_state_t s_preview = {
    .video_fd = -1,
    .active_format = APP_VIDEO_FMT_RGB565,
};

static void preview_control_task(void *arg);

static void lock_state(void)
{
    if (s_preview.lock != NULL) {
        xSemaphoreTake(s_preview.lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_preview.lock != NULL) {
        xSemaphoreGive(s_preview.lock);
    }
}

static bool ensure_control_task(void)
{
    if (s_preview.lock == NULL) {
        s_preview.lock = xSemaphoreCreateMutex();
        if (s_preview.lock == NULL) {
            ESP_LOGE(TAG, "Failed to create preview mutex");
            return false;
        }
    }

    if (s_preview.cmd_queue == NULL) {
        s_preview.cmd_queue = xQueueCreate(PREVIEW_CMD_QUEUE_LEN, sizeof(preview_cmd_t));
        if (s_preview.cmd_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create preview command queue");
            return false;
        }
    }

    if (s_preview.stop_done == NULL) {
        s_preview.stop_done = xSemaphoreCreateBinary();
        if (s_preview.stop_done == NULL) {
            ESP_LOGE(TAG, "Failed to create preview stop semaphore");
            return false;
        }
    }

    if (s_preview.task == NULL) {
        BaseType_t ret = xTaskCreatePinnedToCore(preview_control_task, "auth_cam_preview",
                                                 PREVIEW_TASK_STACK_SIZE, NULL,
                                                 PREVIEW_TASK_PRIORITY, &s_preview.task,
                                                 PREVIEW_TASK_CORE);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create preview control task");
            s_preview.task = NULL;
            return false;
        }
    }

    return true;
}

static void set_status_label(const char *text, bool hidden)
{
    lv_obj_t *label = NULL;

    lock_state();
    label = s_preview.status_label;
    unlock_state();

    if (label == NULL) {
        return;
    }

    if (esp_lv_adapter_lock(pdMS_TO_TICKS(100)) == ESP_OK) {
        if (hidden) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(label, text != NULL ? text : "");
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        esp_lv_adapter_unlock();
    }
}

static bool is_current_generation(uint32_t generation)
{
    bool current = false;

    lock_state();
    current = s_preview.active && s_preview.generation == generation;
    unlock_state();

    return current;
}

static void preview_frame_cb(uint8_t *camera_buf, uint8_t camera_buf_index,
                             uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                             size_t camera_buf_len)
{
    (void)camera_buf_index;
    lv_obj_t *canvas = NULL;
    lv_obj_t *status_label = NULL;
    auth_camera_preview_frame_cb_t frame_cb = NULL;
    void *frame_cb_ctx = NULL;
    bool active = false;
    video_fmt_t format = APP_VIDEO_FMT_RGB565;

    lock_state();
    active = s_preview.active;
    canvas = s_preview.canvas;
    status_label = s_preview.status_label;
    frame_cb = s_preview.frame_cb;
    frame_cb_ctx = s_preview.frame_cb_ctx;
    format = s_preview.active_format;
    unlock_state();

    if (!active || camera_buf == NULL) {
        return;
    }

    if (frame_cb != NULL) {
        frame_cb(camera_buf, camera_buf_hes, camera_buf_ves, camera_buf_len, frame_cb_ctx);
    }

    if (canvas == NULL || format != APP_VIDEO_FMT_RGB565) {
        return;
    }

    if (esp_lv_adapter_lock(100) == ESP_OK) {
        lv_canvas_set_buffer(canvas, camera_buf, camera_buf_hes, camera_buf_ves, LV_IMG_CF_TRUE_COLOR);

        if (status_label != NULL) {
            lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            lock_state();
            if (s_preview.status_label == status_label) {
                s_preview.status_label = NULL;
            }
            unlock_state();
        }

        lv_refr_now(NULL);
        esp_lv_adapter_unlock();
    }
}

static esp_err_t init_camera(video_fmt_t format)
{
    if (s_preview.initialized) {
        return s_preview.active_format == format ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (!s_preview.video_hw_initialized) {
        esp_err_t video_ret = app_video_main(bsp_i2c_get_handle());
        if (video_ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera video init failed: %s", esp_err_to_name(video_ret));
            return video_ret;
        }
        s_preview.video_hw_initialized = true;
    }

    s_preview.video_fd = app_video_open((char *)EXAMPLE_CAM_DEV_PATH, format);
    if (s_preview.video_fd < 0) {
        ESP_LOGE(TAG, "Open camera video device failed");
        return ESP_FAIL;
    }

    size_t cache_alignment = 0;
    esp_err_t ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_alignment);
    if (ret != ESP_OK || cache_alignment == 0) {
        ESP_LOGW(TAG, "Failed to get cache alignment, using 128 bytes");
        cache_alignment = 128;
    }

    const uint32_t negotiated_size = app_video_get_buf_size();
    if (negotiated_size == 0) {
        ESP_LOGE(TAG, "Camera returned an invalid frame buffer size");
        close(s_preview.video_fd);
        s_preview.video_fd = -1;
        return ESP_FAIL;
    }

    const uint32_t buffer_count =
        format == APP_VIDEO_FMT_RGB888 ? 2U : EXAMPLE_CAM_BUF_NUM;
    s_preview.frame_buffer_size = ALIGN_UP_BY(negotiated_size, cache_alignment);
    for (uint32_t i = 0; i < buffer_count; i++) {
        s_preview.frame_buffers[i] = heap_caps_aligned_alloc(cache_alignment,
                                                             s_preview.frame_buffer_size,
                                                             MALLOC_CAP_SPIRAM);
        if (s_preview.frame_buffers[i] == NULL) {
            ESP_LOGE(TAG, "Allocate camera frame buffer %u failed", (unsigned int)i);
            for (uint32_t j = 0; j < i; j++) {
                heap_caps_free(s_preview.frame_buffers[j]);
                s_preview.frame_buffers[j] = NULL;
            }
            close(s_preview.video_fd);
            s_preview.video_fd = -1;
            return ESP_ERR_NO_MEM;
        }
        memset(s_preview.frame_buffers[i], 0, s_preview.frame_buffer_size);
    }

    ret = app_video_register_frame_operation_cb(preview_frame_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register camera frame callback failed: %s", esp_err_to_name(ret));
        for (uint32_t i = 0; i < buffer_count; i++) {
            heap_caps_free(s_preview.frame_buffers[i]);
            s_preview.frame_buffers[i] = NULL;
        }
        close(s_preview.video_fd);
        s_preview.video_fd = -1;
        return ret;
    }

    ret = app_video_set_bufs(s_preview.video_fd, buffer_count,
                             (const void **)s_preview.frame_buffers);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set camera frame buffers failed: %s", esp_err_to_name(ret));
        for (uint32_t i = 0; i < buffer_count; i++) {
            heap_caps_free(s_preview.frame_buffers[i]);
            s_preview.frame_buffers[i] = NULL;
        }
        s_preview.video_fd = -1;
        return ret;
    }

    s_preview.frame_buffer_count = buffer_count;
    s_preview.active_format = format;
    s_preview.initialized = true;
    ESP_LOGI(TAG,
             "Camera preview initialized: format=0x%08x buffers=%u bytes=%u",
             (unsigned int)format,
             (unsigned int)buffer_count,
             (unsigned int)negotiated_size);
    return ESP_OK;
}

static void handle_start(const preview_cmd_t *cmd)
{
    if (cmd == NULL || !is_current_generation(cmd->generation)) {
        return;
    }

    set_status_label("正在启动摄像头...", false);

    esp_err_t ret = init_camera(cmd->format);
    if (ret != ESP_OK) {
        set_status_label("摄像头不可用", false);
        return;
    }

    if (!is_current_generation(cmd->generation)) {
        return;
    }

    lock_state();
    bool already_running = s_preview.running;
    unlock_state();

    if (already_running) {
        set_status_label(NULL, true);
        return;
    }

    ret = app_video_stream_task_start(s_preview.video_fd, PREVIEW_TASK_CORE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start camera stream failed: %s", esp_err_to_name(ret));
        set_status_label("摄像头启动失败", false);
        return;
    }

    lock_state();
    s_preview.running = true;
    unlock_state();
}

static void handle_stop(void)
{
    lock_state();
    bool running = s_preview.running;
    int video_fd = s_preview.video_fd;
    unlock_state();

    if (!running || video_fd < 0) {
        return;
    }

    app_video_stream_task_stop(video_fd);
    app_video_stream_wait_stop();

    lock_state();
    s_preview.running = false;
    unlock_state();
}

static void signal_stop_done(void)
{
    if (s_preview.stop_done != NULL) {
        xSemaphoreGive(s_preview.stop_done);
    }
}

static void drain_stop_done(void)
{
    if (s_preview.stop_done == NULL) {
        return;
    }

    while (xSemaphoreTake(s_preview.stop_done, 0) == pdTRUE) {
    }
}

static void preview_control_task(void *arg)
{
    (void)arg;
    preview_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_preview.cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd.type == PREVIEW_CMD_START) {
            handle_start(&cmd);
        } else {
            handle_stop();
            signal_stop_done();
        }
    }
}

esp_err_t auth_camera_preview_start_with_format(lv_obj_t *canvas, lv_obj_t *status_label,
                                                video_fmt_t format)
{
    if (format != APP_VIDEO_FMT_RGB565 && format != APP_VIDEO_FMT_RGB888) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (format == APP_VIDEO_FMT_RGB888 && canvas != NULL) {
        ESP_LOGE(TAG, "RGB888 capture-only mode cannot bind an RGB565 LVGL canvas");
        return ESP_ERR_INVALID_ARG;
    }
    if (canvas == NULL && status_label == NULL) {
        ESP_LOGI(TAG, "Starting camera in capture-only mode: format=0x%08x",
                 (unsigned int)format);
    }

    if (!ensure_control_task()) {
        return ESP_ERR_NO_MEM;
    }

    preview_cmd_t cmd = {
        .type = PREVIEW_CMD_START,
        .canvas = canvas,
        .status_label = status_label,
        .format = format,
    };

    lock_state();
    if (s_preview.initialized && s_preview.active_format != format) {
        unlock_state();
        ESP_LOGE(TAG, "Camera must be released before changing pixel format");
        return ESP_ERR_INVALID_STATE;
    }
    s_preview.active = true;
    s_preview.canvas = canvas;
    s_preview.canvas_to_detach = NULL;
    s_preview.status_label = status_label;
    s_preview.generation++;
    cmd.generation = s_preview.generation;
    unlock_state();

    if (xQueueSend(s_preview.cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Preview command queue full");
        lock_state();
        s_preview.active = false;
        unlock_state();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t auth_camera_preview_start(lv_obj_t *canvas, lv_obj_t *status_label)
{
    return auth_camera_preview_start_with_format(canvas, status_label,
                                                 APP_VIDEO_FMT_RGB565);
}

void auth_camera_preview_stop(void)
{
    if (s_preview.lock != NULL) {
        lock_state();
        if (s_preview.canvas != NULL) {
            s_preview.canvas_to_detach = s_preview.canvas;
        }
        s_preview.active = false;
        s_preview.canvas = NULL;
        s_preview.status_label = NULL;
        s_preview.frame_cb = NULL;
        s_preview.frame_cb_ctx = NULL;
        s_preview.generation++;
        unlock_state();
    }

    if (s_preview.cmd_queue == NULL) {
        return;
    }

    preview_cmd_t cmd = {
        .type = PREVIEW_CMD_STOP,
    };
    if (xQueueSendToFront(s_preview.cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to queue camera preview stop");
    }
}

void auth_camera_preview_stop_and_release_idle_resources(void)
{
    QueueHandle_t cmd_queue = NULL;
    TaskHandle_t task = NULL;

    if (s_preview.lock != NULL) {
        lock_state();
        if (s_preview.canvas != NULL) {
            s_preview.canvas_to_detach = s_preview.canvas;
        }
        s_preview.active = false;
        s_preview.canvas = NULL;
        s_preview.status_label = NULL;
        s_preview.frame_cb = NULL;
        s_preview.frame_cb_ctx = NULL;
        s_preview.generation++;
        cmd_queue = s_preview.cmd_queue;
        task = s_preview.task;
        unlock_state();
    }

    if (cmd_queue != NULL && task != NULL) {
        preview_cmd_t cmd = {
            .type = PREVIEW_CMD_STOP,
        };

        drain_stop_done();
        xQueueSendToFront(cmd_queue, &cmd, portMAX_DELAY);
        if (s_preview.stop_done != NULL) {
            xSemaphoreTake(s_preview.stop_done, portMAX_DELAY);
        }

        while (auth_camera_preview_is_running()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    auth_camera_preview_release_idle_resources();
}

void auth_camera_preview_release_idle_resources(void)
{
    if (s_preview.lock == NULL) {
        return;
    }

    int video_fd = -1;
    uint8_t *frame_buffers[EXAMPLE_CAM_BUF_NUM] = {0};
    uint32_t frame_buffer_count = 0;
    lv_obj_t *canvas_to_detach = NULL;

    lock_state();
    bool active = s_preview.active;
    bool running = s_preview.running;
    bool initialized = s_preview.initialized;
    if (active || running || !initialized) {
        unlock_state();
        return;
    }

    video_fd = s_preview.video_fd;
    s_preview.video_fd = -1;
    frame_buffer_count = s_preview.frame_buffer_count;
    canvas_to_detach = s_preview.canvas_to_detach;
    s_preview.canvas_to_detach = NULL;
    for (uint32_t i = 0; i < frame_buffer_count; i++) {
        frame_buffers[i] = s_preview.frame_buffers[i];
        s_preview.frame_buffers[i] = NULL;
    }
    s_preview.frame_buffer_count = 0;
    s_preview.frame_buffer_size = 0;
    s_preview.initialized = false;
    s_preview.canvas = NULL;
    s_preview.status_label = NULL;
    s_preview.frame_cb = NULL;
    s_preview.frame_cb_ctx = NULL;
    s_preview.active = false;
    s_preview.generation++;
    unlock_state();

    if (canvas_to_detach != NULL &&
        esp_lv_adapter_lock(pdMS_TO_TICKS(100)) == ESP_OK) {
        if (lv_obj_is_valid(canvas_to_detach)) {
            lv_img_set_src(canvas_to_detach, NULL);
        }
        esp_lv_adapter_unlock();
    }

    if (video_fd >= 0) {
        close(video_fd);
    }

    for (uint32_t i = 0; i < frame_buffer_count; i++) {
        if (frame_buffers[i] != NULL) {
            heap_caps_free(frame_buffers[i]);
        }
    }

    ESP_LOGI(TAG, "Released idle camera preview resources");
}

bool auth_camera_preview_is_running(void)
{
    bool running = false;

    if (s_preview.lock == NULL) {
        return false;
    }

    lock_state();
    running = s_preview.running;
    unlock_state();

    return running;
}

void auth_camera_preview_set_frame_callback(auth_camera_preview_frame_cb_t cb, void *user_ctx)
{
    if (!ensure_control_task()) {
        return;
    }

    lock_state();
    s_preview.frame_cb = cb;
    s_preview.frame_cb_ctx = user_ctx;
    unlock_state();
}

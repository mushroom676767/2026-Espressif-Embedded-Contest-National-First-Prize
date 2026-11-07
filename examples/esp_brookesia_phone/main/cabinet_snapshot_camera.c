#include "cabinet_snapshot_camera.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "auth_camera_preview.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SNAPSHOT_CAPTURE_PATH "/spiffs/cap.jpg"
#define SNAPSHOT_CAPTURE_TIMEOUT_MS 2500

typedef struct {
    SemaphoreHandle_t done;
    volatile bool active;
    volatile bool captured;
    esp_err_t result;
    char path[128];
} snapshot_capture_t;

static const char *TAG = "cabinet_snapshot_camera";
static bool s_initialized;
static snapshot_capture_t s_capture;

static void drain_done_signal(void)
{
    if (s_capture.done == NULL) {
        return;
    }
    while (xSemaphoreTake(s_capture.done, 0) == pdTRUE) {
    }
}

static esp_err_t write_unavailable_marker(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open snapshot marker failed: %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    static const char marker[] = "camera snapshot unavailable\n";
    bool ok = fwrite(marker, sizeof(marker) - 1, 1, fp) == 1;
    fclose(fp);
    return ok ? ESP_OK : ESP_FAIL;
}

static void snapshot_frame_cb(const uint8_t *data,
                              uint32_t width,
                              uint32_t height,
                              size_t len,
                              void *user_ctx)
{
    (void)width;
    (void)height;
    (void)user_ctx;

    if (!s_capture.active || s_capture.captured || data == NULL || len == 0) {
        return;
    }

    s_capture.captured = true;
    ESP_LOGI(TAG, "Snapshot frame received: width=%lu height=%lu len=%u path=%s",
             (unsigned long)width,
             (unsigned long)height,
             (unsigned int)len,
             s_capture.path);

    FILE *fp = fopen(s_capture.path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open snapshot frame failed: %s errno=%d", s_capture.path, errno);
        s_capture.result = ESP_FAIL;
        xSemaphoreGive(s_capture.done);
        return;
    }

    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);

    s_capture.result = (written == len) ? ESP_OK : ESP_FAIL;
    ESP_LOGI(TAG, "Snapshot frame write result: written=%u expected=%u ret=%s",
             (unsigned int)written,
             (unsigned int)len,
             esp_err_to_name(s_capture.result));
    xSemaphoreGive(s_capture.done);
}

esp_err_t cabinet_snapshot_camera_init(void)
{
    if (s_capture.done == NULL) {
        s_capture.done = xSemaphoreCreateBinary();
        if (s_capture.done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t cabinet_snapshot_camera_capture(const char *event_id,
                                          char *path_out,
                                          size_t path_out_size)
{
    if (path_out == NULL || path_out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        esp_err_t init_ret = cabinet_snapshot_camera_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    (void)event_id;
    int needed = snprintf(s_capture.path, sizeof(s_capture.path), "%s", SNAPSHOT_CAPTURE_PATH);
    if (needed < 0 || (size_t)needed >= sizeof(s_capture.path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (snprintf(path_out, path_out_size, "%s", s_capture.path) >= (int)path_out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_capture.active) {
        return ESP_ERR_INVALID_STATE;
    }

    drain_done_signal();

    s_capture.result = ESP_ERR_NOT_SUPPORTED;
    s_capture.captured = false;
    s_capture.active = true;

    bool was_running = auth_camera_preview_is_running();
    ESP_LOGI(TAG, "Cabinet snapshot capture start: event_id=%s path=%s was_running=%d timeout_ms=%d",
             event_id != NULL ? event_id : "(null)",
             s_capture.path,
             was_running,
             SNAPSHOT_CAPTURE_TIMEOUT_MS);
    auth_camera_preview_set_frame_callback(snapshot_frame_cb, NULL);
    esp_err_t start_ret = was_running ? ESP_OK : auth_camera_preview_start(NULL, NULL);
    if (start_ret != ESP_OK) {
        ESP_LOGW(TAG, "Start capture-only camera failed: %s", esp_err_to_name(start_ret));
        auth_camera_preview_set_frame_callback(NULL, NULL);
        s_capture.active = false;
        (void)write_unavailable_marker(s_capture.path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (xSemaphoreTake(s_capture.done, pdMS_TO_TICKS(SNAPSHOT_CAPTURE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "No cabinet snapshot frame within timeout: path=%s was_running=%d",
                 s_capture.path, was_running);
        s_capture.result = ESP_ERR_NOT_SUPPORTED;
    }

    auth_camera_preview_set_frame_callback(NULL, NULL);
    s_capture.active = false;

    if (!was_running) {
        auth_camera_preview_stop_and_release_idle_resources();
    }

    if (s_capture.result == ESP_ERR_NOT_SUPPORTED) {
        esp_err_t marker_ret = write_unavailable_marker(s_capture.path);
        ESP_LOGW(TAG, "Snapshot unavailable marker written: path=%s ret=%s",
                 s_capture.path, esp_err_to_name(marker_ret));
    }

    // auth_camera_preview owns app_video_register_frame_operation_cb; this facade consumes its frame callback API.
    ESP_LOGI(TAG, "Cabinet snapshot capture done: ret=%s captured=%d path=%s",
             esp_err_to_name(s_capture.result),
             s_capture.captured,
             s_capture.path);
    return s_capture.result;
}

#include "tool_detection_service.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "slot_constraint.hpp"
#include "tool_detect.hpp"
#include "usb_uvc_camera_service.h"

static const char *TAG = "tool_detection";

#define TOOL_TASK_STACK_SIZE       (1024 * 12)
#define TOOL_TASK_PRIORITY         4
#define TOOL_TASK_CORE             0
#define TOOL_SCORE_THRESHOLD       tool_detect::ToolDetector::kCandidateScoreThreshold
#define TOOL_NMS_THRESHOLD         tool_detect::ToolDetector::kNmsThreshold
#define TOOL_USB_CAM_TIMEOUT_MS    5000
#define TOOL_SYNC_TIMEOUT_MS       20000
#define TOOL_CAPTURE_WIDTH         1280
#define TOOL_CAPTURE_HEIGHT        720

static constexpr std::array<uint8_t, tool_detect::kToolSlotCount> TOOL_PHYSICAL_SLOTS = {
    1, 2, 3, 4, 5, 6, 7, 8, 9,
};
static constexpr std::array<uint8_t, tool_detect::kToolSlotCount> TOOL_CABINET_SLOTS = {
    5, 8, 3, 6, 4, 9, 1, 2, 7,
};
static constexpr std::array<int, tool_detect::kToolSlotCount> TOOL_SLOT_CLASSES = {
    1, 7, 2, 3, 5, 0, 6, 6, 4,
};
static constexpr int TOOL_ALWAYS_PRESENT_CLASS_ID = 4;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool active;
    bool busy;
    tool_detection_event_cb_t cb;
    void *user_ctx;
    char snapshot_path[TOOL_DETECT_SNAPSHOT_PATH_MAX];
} tool_state_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    tool_detection_event_t result;
    bool got_result;
} sync_context_t;

static tool_state_t s_state = {};
static sync_context_t s_sync = {};

static void lock_state(void)
{
    if (s_state.lock != nullptr) {
        xSemaphoreTake(s_state.lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_state.lock != nullptr) {
        xSemaphoreGive(s_state.lock);
    }
}

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "%s, free 8-bit=%u largest 8-bit=%u free internal=%u largest internal=%u free psram=%u largest psram=%u",
             stage,
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

static bool request_is_active(tool_detection_event_cb_t cb, void *user_ctx)
{
    lock_state();
    bool active = s_state.active && s_state.cb == cb && s_state.user_ctx == user_ctx;
    unlock_state();
    return active;
}

static esp_err_t capture_usb_camera_frame(uint8_t **frame_buf,
                                          size_t *frame_len,
                                          uint32_t *width,
                                          uint32_t *height,
                                          const char *snapshot_path)
{
    if (snapshot_path == nullptr || snapshot_path[0] == '\0') {
        ESP_LOGE(TAG, "RGB888 tool capture requires a same-frame JPEG snapshot path");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = usb_uvc_camera_capture_rgb888_with_jpeg(frame_buf,
                                                            frame_len,
                                                            width,
                                                            height,
                                                            snapshot_path,
                                                            TOOL_USB_CAM_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "USB UVC camera captured for tool detection: %ux%u len=%u",
                 (unsigned int)*width,
                 (unsigned int)*height,
                 (unsigned int)*frame_len);
    }
    return ret;
}

static void build_class_inventory_results(
    const tool_detect::detection_result_t &detection,
    std::array<tool_detect::SlotResult, tool_detect::kToolSlotCount> *results)
{
    std::array<std::vector<float>, tool_detect::kNumClasses> class_scores{};
    for (const auto &item : detection.detections) {
        if (item.category >= 0 && item.category < tool_detect::kNumClasses) {
            class_scores[item.category].push_back(item.score);
        }
    }
    for (auto &scores : class_scores) {
        std::stable_sort(scores.begin(), scores.end(), std::greater<float>());
    }

    std::array<size_t, tool_detect::kNumClasses> consumed{};
    for (size_t i = 0; i < results->size(); ++i) {
        const int category = TOOL_SLOT_CLASSES[i];
        const size_t score_index = consumed[category]++;
        const bool detected = score_index < class_scores[category].size();
        const bool always_present = category == TOOL_ALWAYS_PRESENT_CLASS_ID;
        const bool present = always_present || detected;
        const float confidence = always_present
                                     ? 1.0f
                                     : (detected ? class_scores[category][score_index] : 0.0f);
        (*results)[i] = {
            TOOL_PHYSICAL_SLOTS[i],
            TOOL_CABINET_SLOTS[i],
            category,
            present ? category : -1,
            confidence,
            0.0f,
            present ? tool_detect::SlotStatus::Present : tool_detect::SlotStatus::Missing,
        };
        ESP_LOGI(TAG,
                 "Class inventory physical=%u cabinet=%u status=%s class=%d score=%.3f",
                 (*results)[i].physical_slot_no,
                 (*results)[i].cabinet_slot_no,
                 tool_detect::slot_status_name((*results)[i].status),
                 category,
                 (*results)[i].confidence);
    }
}

static void fill_success_event(tool_detection_event_t *event,
                               const tool_detect::detection_result_t &detection,
                               const std::array<tool_detect::SlotResult,
                                                tool_detect::kToolSlotCount> &slot_results,
                               uint32_t width,
                               uint32_t height,
                               const char *snapshot_path,
                               const char *captured_at,
                               int64_t captured_monotonic_us)
{
    event->type = TOOL_DET_EVT_DONE;
    event->frame_width = width;
    event->frame_height = height;
    event->slot_count = TOOL_DETECT_SLOT_COUNT;
    event->quality_ok = true;

    int count = std::min(static_cast<int>(detection.detections.size()),
                         TOOL_DETECT_MAX_DETECTIONS);
    event->detection_count = count;
    for (int i = 0; i < count; ++i) {
        event->detections[i].category = detection.detections[i].category;
        event->detections[i].score = detection.detections[i].score;
        memcpy(event->detections[i].box, detection.detections[i].box, sizeof(int) * 4);
        int category = event->detections[i].category;
        if (category >= 0 && category < TOOL_DETECT_CLASS_COUNT) {
            event->class_counts[category]++;
        }
    }

    for (size_t i = 0; i < slot_results.size(); ++i) {
        const auto &src = slot_results[i];
        auto &dst = event->slots[i];
        dst.physical_slot_no = src.physical_slot_no;
        dst.cabinet_slot_no = src.cabinet_slot_no;
        dst.expected_class_id = src.expected_class_id;
        dst.detected_class_id = src.detected_class_id;
        dst.confidence = src.confidence;
        dst.iou = src.iou;
        switch (src.status) {
        case tool_detect::SlotStatus::Present:
            dst.status = TOOL_SLOT_DET_PRESENT;
            break;
        case tool_detect::SlotStatus::Mismatch:
            dst.status = TOOL_SLOT_DET_MISMATCH;
            break;
        case tool_detect::SlotStatus::Missing:
        default:
            dst.status = TOOL_SLOT_DET_MISSING;
            break;
        }
    }

    snprintf(event->snapshot_local_path, sizeof(event->snapshot_local_path), "%s",
             snapshot_path != nullptr ? snapshot_path : "");
    snprintf(event->captured_at, sizeof(event->captured_at), "%s",
             captured_at != nullptr ? captured_at : "");
    event->captured_monotonic_us = captured_monotonic_us;
    snprintf(event->message, sizeof(event->message), "检测到 %d 个工具", count);
}

static void finish_request(tool_detection_event_cb_t cb,
                           void *user_ctx,
                           const tool_detection_event_t *event,
                           const char *snapshot_path)
{
    lock_state();
    bool deliver = s_state.active && s_state.cb == cb && s_state.user_ctx == user_ctx;
    if (deliver) {
        s_state.active = false;
    }
    unlock_state();

    if (deliver) {
        cb(event, user_ctx);
    } else if (snapshot_path != nullptr && snapshot_path[0] != '\0') {
        remove(snapshot_path);
    }

    /* Keep busy set until the terminal callback has returned. This prevents a
     * timed-out synchronous request from racing the next request's context. */
    lock_state();
    s_state.busy = false;
    if (s_state.cb == cb && s_state.user_ctx == user_ctx) {
        s_state.active = false;
        s_state.cb = nullptr;
        s_state.user_ctx = nullptr;
        s_state.snapshot_path[0] = '\0';
    }
    unlock_state();
}

static void tool_detection_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        tool_detection_event_cb_t cb = nullptr;
        void *user_ctx = nullptr;
        char snapshot_path[TOOL_DETECT_SNAPSHOT_PATH_MAX] = {};

        lock_state();
        if (!s_state.active) {
            s_state.busy = false;
            unlock_state();
            continue;
        }
        cb = s_state.cb;
        user_ctx = s_state.user_ctx;
        snprintf(snapshot_path, sizeof(snapshot_path), "%s", s_state.snapshot_path);
        unlock_state();

        if (snapshot_path[0] != '\0') {
            remove(snapshot_path);
        }

        tool_detection_event_t start_event = {};
        start_event.type = TOOL_DET_EVT_STARTED;
        snprintf(start_event.message, sizeof(start_event.message), "正在检测工具...");
        cb(&start_event, user_ctx);

        log_heap_state("Tool inference start");

        uint8_t *frame_buf = nullptr;
        size_t frame_len = 0;
        uint32_t cam_w = 0;
        uint32_t cam_h = 0;
        esp_err_t capture_ret = capture_usb_camera_frame(&frame_buf, &frame_len,
                                                         &cam_w, &cam_h, snapshot_path);
        int64_t captured_monotonic_us = capture_ret == ESP_OK ? esp_timer_get_time() : 0;
        char captured_at[32] = {};
        if (capture_ret == ESP_OK) {
            std::time_t captured_now = std::time(nullptr);
            struct tm captured_tm = {};
            gmtime_r(&captured_now, &captured_tm);
            strftime(captured_at, sizeof(captured_at), "%Y-%m-%dT%H:%M:%SZ", &captured_tm);
        }
        if (capture_ret == ESP_OK &&
            (cam_w != TOOL_CAPTURE_WIDTH || cam_h != TOOL_CAPTURE_HEIGHT ||
             frame_len != (size_t)TOOL_CAPTURE_WIDTH * TOOL_CAPTURE_HEIGHT * 3)) {
            ESP_LOGE(TAG,
                     "Tool detection requires exact 1280x720 RGB888: got=%ux%u len=%u",
                     (unsigned int)cam_w,
                     (unsigned int)cam_h,
                     (unsigned int)frame_len);
            capture_ret = ESP_ERR_INVALID_SIZE;
        }

        tool_detect::detection_result_t detection = {};
        std::array<tool_detect::SlotResult, tool_detect::kToolSlotCount> slot_results{};
        esp_err_t inference_ret = capture_ret;
        if (capture_ret == ESP_OK && request_is_active(cb, user_ctx)) {
            tool_detection_event_t frame_event = {};
            frame_event.type = TOOL_DET_EVT_FRAME_READY;
            snprintf(frame_event.message, sizeof(frame_event.message), "正在识别工具...");
            cb(&frame_event, user_ctx);

            dl::image::img_t image = {};
            image.data = frame_buf;
            image.width = cam_w;
            image.height = cam_h;
            image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

            tool_detect::ToolDetector detector(TOOL_SCORE_THRESHOLD, TOOL_NMS_THRESHOLD);
            inference_ret = detector.run(image, &detection);
            if (inference_ret == ESP_OK) {
                build_class_inventory_results(detection, &slot_results);
            }
        }

        if (frame_buf != nullptr) {
            heap_caps_free(frame_buf);
        }
        log_heap_state("Tool inference done");

        tool_detection_event_t event = {};
        if (capture_ret == ESP_OK && inference_ret == ESP_OK) {
            fill_success_event(&event, detection, slot_results, cam_w, cam_h, snapshot_path,
                               captured_at, captured_monotonic_us);
        } else {
            event.type = TOOL_DET_EVT_ERROR;
            if (capture_ret == ESP_ERR_INVALID_SIZE) {
                snprintf(event.message, sizeof(event.message), "摄像头必须支持1280x720");
            } else if (capture_ret != ESP_OK) {
                ESP_LOGE(TAG, "USB camera capture failed: %s", esp_err_to_name(capture_ret));
                snprintf(event.message, sizeof(event.message), "USB摄像头抓帧失败");
            } else if (inference_ret != ESP_OK) {
                ESP_LOGE(TAG, "Tool detector run failed: %s", esp_err_to_name(inference_ret));
                snprintf(event.message, sizeof(event.message), "工具模型或输出张量异常");
            }
            if (snapshot_path[0] != '\0') {
                remove(snapshot_path);
            }
        }

        finish_request(cb, user_ctx, &event, snapshot_path);
        log_heap_state("Tool cleanup complete");
    }
}

static esp_err_t ensure_task(void)
{
    if (s_state.lock == nullptr) {
        s_state.lock = xSemaphoreCreateMutex();
        if (s_state.lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_state.task == nullptr) {
        BaseType_t ret = xTaskCreatePinnedToCore(tool_detection_task,
                                                 "tool_detection",
                                                 TOOL_TASK_STACK_SIZE,
                                                 nullptr,
                                                 TOOL_TASK_PRIORITY,
                                                 &s_state.task,
                                                 TOOL_TASK_CORE);
        if (ret != pdPASS) {
            s_state.task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t tool_detection_start_with_snapshot(tool_detection_event_cb_t cb,
                                             void *user_ctx,
                                             const char *snapshot_path)
{
    if (cb == nullptr ||
        (snapshot_path != nullptr && strlen(snapshot_path) >= TOOL_DETECT_SNAPSHOT_PATH_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    lock_state();
    if (s_state.active || s_state.busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    s_state.active = true;
    s_state.busy = true;
    s_state.cb = cb;
    s_state.user_ctx = user_ctx;
    snprintf(s_state.snapshot_path, sizeof(s_state.snapshot_path), "%s",
             snapshot_path != nullptr ? snapshot_path : "");
    unlock_state();

    xTaskNotifyGive(s_state.task);
    return ESP_OK;
}

esp_err_t tool_detection_start(tool_detection_event_cb_t cb, void *user_ctx)
{
    return tool_detection_start_with_snapshot(cb, user_ctx, nullptr);
}

void tool_detection_cancel(void)
{
    lock_state();
    s_state.active = false;
    s_state.cb = nullptr;
    s_state.user_ctx = nullptr;
    s_state.snapshot_path[0] = '\0';
    unlock_state();
}

bool tool_detection_is_active(void)
{
    lock_state();
    bool active = s_state.active || s_state.busy;
    unlock_state();
    return active;
}

static void sync_cb(const tool_detection_event_t *event, void *user_ctx)
{
    sync_context_t *ctx = static_cast<sync_context_t *>(user_ctx);
    if (ctx == nullptr || event == nullptr ||
        (event->type != TOOL_DET_EVT_DONE && event->type != TOOL_DET_EVT_ERROR)) {
        return;
    }
    memcpy(&ctx->result, event, sizeof(ctx->result));
    ctx->got_result = true;
    if (ctx->done != nullptr) {
        xSemaphoreGive(ctx->done);
    }
}

esp_err_t tool_detection_run_sync_with_snapshot(const char *snapshot_path,
                                                tool_detection_event_t *result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    if (s_sync.lock == nullptr) {
        s_sync.lock = xSemaphoreCreateMutex();
        if (s_sync.lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_sync.lock, portMAX_DELAY);

    if (s_sync.done == nullptr) {
        s_sync.done = xSemaphoreCreateBinary();
        if (s_sync.done == nullptr) {
            xSemaphoreGive(s_sync.lock);
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_sync.done, 0) == pdTRUE) {
    }
    memset(&s_sync.result, 0, sizeof(s_sync.result));
    s_sync.got_result = false;

    esp_err_t ret = tool_detection_start_with_snapshot(sync_cb, &s_sync, snapshot_path);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_sync.lock);
        return ret;
    }

    if (xSemaphoreTake(s_sync.done, pdMS_TO_TICKS(TOOL_SYNC_TIMEOUT_MS)) != pdTRUE) {
        tool_detection_cancel();
        xSemaphoreGive(s_sync.lock);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_sync.got_result) {
        xSemaphoreGive(s_sync.lock);
        return ESP_FAIL;
    }

    memcpy(result, &s_sync.result, sizeof(*result));
    ret = result->type == TOOL_DET_EVT_DONE ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_sync.lock);
    return ret;
}

esp_err_t tool_detection_run_sync(tool_detection_event_t *result)
{
    return tool_detection_run_sync_with_snapshot(nullptr, result);
}

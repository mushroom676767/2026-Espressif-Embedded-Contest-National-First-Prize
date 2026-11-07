#include "ppe_detection_service.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>

#include "auth_camera_preview.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ppe_detect.hpp"

static const char *TAG = "ppe_detection";

#define PPE_TASK_STACK_SIZE       (1024 * 10)
#define PPE_TASK_PRIORITY         4
#define PPE_TASK_CORE             0
#define PPE_HELMET_THRESHOLD      ppe_detect::PpeDetector::kHelmetPresenceThreshold
#define PPE_GOGGLES_THRESHOLD     ppe_detect::PpeDetector::kGogglesPresenceThreshold
#define PPE_SCORE_THRESHOLD       ppe_detect::PpeDetector::kCandidateScoreThreshold
#define PPE_NMS_THRESHOLD         ppe_detect::PpeDetector::kNmsThreshold

typedef struct {
    uint32_t generation;
    uint8_t *data;
    size_t len;
    uint32_t width;
    uint32_t height;
} processing_frame_t;

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

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    uint32_t generation;
    bool active;
    bool frame_ready;
    bool event_emitting;
    uint32_t event_generation;
    TaskHandle_t event_task;
    SemaphoreHandle_t event_done;
    bool processing;
    uint32_t processing_generation;
    SemaphoreHandle_t processing_done;
    bool cleanup_in_progress;
    SemaphoreHandle_t cleanup_done;
    bool cleanup_requested;
    uint32_t cleanup_request_generation;
    uint32_t callback_generation;
    ppe_detection_event_cb_t cb;
    void *user_ctx;
    uint8_t *frame_buf;
    size_t frame_len;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_generation;
    bool pending_error;
    uint32_t pending_error_generation;
    ppe_detection_event_t pending_error_event;
} ppe_state_t;

static ppe_state_t s_state = {};

static void ppe_detection_task(void *arg);
static void finish_attempt_locked(uint32_t generation);

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

static uint32_t next_generation_locked(void)
{
    s_state.generation += 1;
    if (s_state.generation == 0) {
        s_state.generation += 1;
    }
    return s_state.generation;
}

static void clear_frame_locked(void)
{
    if (s_state.frame_buf != nullptr) {
        heap_caps_free(s_state.frame_buf);
    }
    s_state.frame_buf = nullptr;
    s_state.frame_len = 0;
    s_state.frame_width = 0;
    s_state.frame_height = 0;
    s_state.frame_generation = 0;
    s_state.frame_ready = false;
}

static void signal_waiter(SemaphoreHandle_t sem)
{
    if (sem == nullptr) {
        return;
    }

    xSemaphoreGive(sem);
}

static void wait_for_event_idle(bool any_generation, uint32_t generation)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    while (true) {
        bool busy = false;
        SemaphoreHandle_t done = nullptr;

        lock_state();
        busy = s_state.event_emitting &&
               (any_generation || s_state.event_generation == generation) &&
               s_state.event_task != self;
        done = s_state.event_done;
        unlock_state();

        if (!busy) {
            return;
        }

        if (done != nullptr) {
            xSemaphoreTake(done, pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void finish_preview_cleanup(void)
{
    lock_state();
    if (s_state.cleanup_in_progress) {
        s_state.cleanup_in_progress = false;
        signal_waiter(s_state.cleanup_done);
    }
    unlock_state();
}

static bool emit_event_if_current(uint32_t generation, const ppe_detection_event_t *event,
                                  bool finish_attempt)
{
    ppe_detection_event_cb_t cb = nullptr;
    void *user_ctx = nullptr;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    if (event == nullptr) {
        return false;
    }

    while (true) {
        SemaphoreHandle_t done = nullptr;

        lock_state();
        if (!s_state.active || s_state.generation != generation) {
            unlock_state();
            return false;
        }
        if (s_state.cb == nullptr) {
            if (finish_attempt) {
                finish_attempt_locked(generation);
            }
            unlock_state();
            return false;
        }
        if (!s_state.event_emitting) {
            s_state.event_emitting = true;
            s_state.event_generation = generation;
            s_state.event_task = self;
            cb = s_state.cb;
            user_ctx = s_state.user_ctx;
            if (finish_attempt) {
                finish_attempt_locked(generation);
            }
            unlock_state();
            break;
        }
        if (s_state.event_task == self) {
            unlock_state();
            return false;
        }
        done = s_state.event_done;
        unlock_state();

        if (done != nullptr) {
            xSemaphoreTake(done, pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (cb != nullptr) {
        cb(event, user_ctx);
    }

    lock_state();
    if (s_state.event_emitting &&
        s_state.event_generation == generation &&
        s_state.event_task == self) {
        s_state.event_emitting = false;
        s_state.event_generation = 0;
        s_state.event_task = nullptr;
        signal_waiter(s_state.event_done);
    }
    unlock_state();

    return cb != nullptr;
}

static ppe_detection_event_t classify_scores(const ppe_detect::detection_scores_t &scores)
{
    ppe_detection_event_t event = {};
    event.type = PPE_EVT_MISSING_BOTH;
    event.helmet_score = scores.helmet_score;
    event.goggles_score = scores.goggles_score;

    event.helmet_present = event.helmet_score >= PPE_HELMET_THRESHOLD;
    event.goggles_present = event.goggles_score >= PPE_GOGGLES_THRESHOLD;

    if (event.helmet_present && event.goggles_present) {
        event.type = PPE_EVT_PASS;
        snprintf(event.message, sizeof(event.message), "安全检测通过");
    } else if (!event.helmet_present && !event.goggles_present) {
        event.type = PPE_EVT_MISSING_BOTH;
        snprintf(event.message, sizeof(event.message), "请佩戴安全帽和护目镜");
    } else if (!event.helmet_present) {
        event.type = PPE_EVT_MISSING_HELMET;
        snprintf(event.message, sizeof(event.message), "请佩戴安全帽");
    } else {
        event.type = PPE_EVT_MISSING_GOGGLES;
        snprintf(event.message, sizeof(event.message), "请佩戴护目镜");
    }

    return event;
}

static esp_err_t ensure_task(void)
{
    if (s_state.lock == nullptr) {
        s_state.lock = xSemaphoreCreateMutex();
        if (s_state.lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_state.event_done == nullptr) {
        s_state.event_done = xSemaphoreCreateBinary();
        if (s_state.event_done == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_state.processing_done == nullptr) {
        s_state.processing_done = xSemaphoreCreateBinary();
        if (s_state.processing_done == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_state.cleanup_done == nullptr) {
        s_state.cleanup_done = xSemaphoreCreateBinary();
        if (s_state.cleanup_done == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_state.task == nullptr) {
        BaseType_t ret = xTaskCreatePinnedToCore(ppe_detection_task,
                                                 "ppe_detection",
                                                 PPE_TASK_STACK_SIZE,
                                                 nullptr,
                                                 PPE_TASK_PRIORITY,
                                                 &s_state.task,
                                                 PPE_TASK_CORE);
        if (ret != pdPASS) {
            s_state.task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void finish_attempt_locked(uint32_t generation)
{
    if (s_state.generation != generation) {
        return;
    }

    s_state.active = false;
    s_state.cb = nullptr;
    s_state.user_ctx = nullptr;
}

static void cleanup_attempt(uint32_t generation, bool release_frame_storage,
                            const ppe_detection_event_t *terminal_event)
{
    bool cleanup_preview = false;

    lock_state();
    if (s_state.callback_generation == generation) {
        auth_camera_preview_set_frame_callback(nullptr, nullptr);
        s_state.callback_generation = 0;
    }
    if (release_frame_storage && s_state.frame_generation == generation) {
        clear_frame_locked();
    }
    if (s_state.generation == generation && !s_state.cleanup_in_progress) {
        s_state.cleanup_in_progress = true;
        cleanup_preview = true;
    }
    unlock_state();

    if (cleanup_preview) {
        auth_camera_preview_stop_and_release_idle_resources();
    }

    if (terminal_event != nullptr) {
        emit_event_if_current(generation, terminal_event, true);
    }

    if (cleanup_preview) {
        finish_preview_cleanup();
    }

    lock_state();
    finish_attempt_locked(generation);
    unlock_state();
}

static void preview_frame_cb(const uint8_t *data, uint32_t width, uint32_t height,
                             size_t len, void *user_ctx)
{
    uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user_ctx));
    uint8_t *frame_copy = nullptr;
    size_t expected_len = 0;

    if (data == nullptr || width == 0 || height == 0) {
        return;
    }
    if (generation == 0) {
        return;
    }
    if (width > SIZE_MAX / height ||
        static_cast<size_t>(width) * height > SIZE_MAX / 3U) {
        ESP_LOGE(TAG, "PPE RGB888 frame dimensions overflow: %ux%u",
                 static_cast<unsigned int>(width),
                 static_cast<unsigned int>(height));
        return;
    }
    expected_len = static_cast<size_t>(width) * height * 3U;

    if (xSemaphoreTake(s_state.lock, 0) != pdTRUE) {
        return;
    }
    if (!s_state.active || s_state.generation != generation || s_state.frame_ready) {
        xSemaphoreGive(s_state.lock);
        return;
    }

    if (len < expected_len) {
        ppe_detection_event_t event = {};
        TaskHandle_t task = s_state.task;

        ESP_LOGE(TAG,
                 "PPE RGB888 frame is truncated: %ux%u expected=%u actual=%u",
                 static_cast<unsigned int>(width),
                 static_cast<unsigned int>(height),
                 static_cast<unsigned int>(expected_len),
                 static_cast<unsigned int>(len));
        event.type = PPE_EVT_ERROR;
        snprintf(event.message, sizeof(event.message), "安全检测异常, 请重试");
        s_state.pending_error = true;
        s_state.pending_error_generation = generation;
        s_state.pending_error_event = event;
        if (s_state.callback_generation == generation) {
            auth_camera_preview_set_frame_callback(nullptr, nullptr);
            s_state.callback_generation = 0;
        }
        xSemaphoreGive(s_state.lock);
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
        return;
    }

    frame_copy = static_cast<uint8_t *>(
        heap_caps_malloc(expected_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_copy == nullptr) {
        ppe_detection_event_t event = {};
        TaskHandle_t task = s_state.task;

        ESP_LOGE(TAG, "Allocate PPE frame buffer failed: %u bytes",
                 static_cast<unsigned int>(expected_len));
        event.type = PPE_EVT_ERROR;
        snprintf(event.message, sizeof(event.message), "安全检测异常, 请重试");
        s_state.pending_error = true;
        s_state.pending_error_generation = generation;
        s_state.pending_error_event = event;
        if (s_state.callback_generation == generation) {
            auth_camera_preview_set_frame_callback(nullptr, nullptr);
            s_state.callback_generation = 0;
        }
        xSemaphoreGive(s_state.lock);
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
        return;
    }

    memcpy(frame_copy, data, expected_len);

    s_state.frame_buf = frame_copy;
    s_state.frame_len = expected_len;
    s_state.frame_width = width;
    s_state.frame_height = height;
    s_state.frame_generation = generation;
    s_state.frame_ready = true;
    TaskHandle_t task = s_state.task;

    ESP_LOGI(TAG,
             "PPE captured RGB888 frame: %ux%u bytes=%u",
             static_cast<unsigned int>(width),
             static_cast<unsigned int>(height),
             static_cast<unsigned int>(expected_len));

    if (s_state.callback_generation == generation) {
        auth_camera_preview_set_frame_callback(nullptr, nullptr);
        s_state.callback_generation = 0;
    }
    xSemaphoreGive(s_state.lock);

    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

static bool get_frame(processing_frame_t *frame)
{
    bool ready = false;

    if (frame == nullptr) {
        return false;
    }

    memset(frame, 0, sizeof(*frame));

    lock_state();
    if (s_state.active &&
        s_state.frame_ready &&
        s_state.frame_buf != nullptr &&
        s_state.frame_generation == s_state.generation) {
        frame->generation = s_state.frame_generation;
        frame->data = s_state.frame_buf;
        frame->len = s_state.frame_len;
        frame->width = s_state.frame_width;
        frame->height = s_state.frame_height;
        s_state.frame_buf = nullptr;
        s_state.frame_len = 0;
        s_state.frame_width = 0;
        s_state.frame_height = 0;
        s_state.frame_generation = 0;
        s_state.frame_ready = false;
        s_state.processing = true;
        s_state.processing_generation = frame->generation;
        ready = true;
    }
    unlock_state();

    return ready;
}

static bool get_pending_error(uint32_t *generation, ppe_detection_event_t *event)
{
    bool ready = false;

    if (generation == nullptr || event == nullptr) {
        return false;
    }

    lock_state();
    if (s_state.pending_error &&
        s_state.pending_error_generation == s_state.generation) {
        *generation = s_state.pending_error_generation;
        *event = s_state.pending_error_event;
        s_state.pending_error = false;
        s_state.pending_error_generation = 0;
        memset(&s_state.pending_error_event, 0, sizeof(s_state.pending_error_event));
        ready = true;
    }
    unlock_state();

    return ready;
}

static bool get_cleanup_request(uint32_t *generation)
{
    bool ready = false;

    if (generation == nullptr) {
        return false;
    }

    lock_state();
    if (s_state.cleanup_requested) {
        *generation = s_state.cleanup_request_generation;
        s_state.cleanup_requested = false;
        s_state.cleanup_request_generation = 0;
        ready = true;
    }
    unlock_state();

    return ready;
}

static void free_processing_frame(processing_frame_t *frame)
{
    if (frame != nullptr && frame->data != nullptr) {
        heap_caps_free(frame->data);
        frame->data = nullptr;
    }
}

static void clear_processing(uint32_t generation)
{
    lock_state();
    if (s_state.processing && s_state.processing_generation == generation) {
        s_state.processing = false;
        s_state.processing_generation = 0;
        signal_waiter(s_state.processing_done);
    }
    unlock_state();
}

static void ppe_detection_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t cleanup_generation = 0;
        if (get_cleanup_request(&cleanup_generation)) {
            auth_camera_preview_stop_and_release_idle_resources();
            finish_preview_cleanup();
            ESP_LOGI(TAG, "PPE cancel cleanup complete, generation=%u",
                     static_cast<unsigned int>(cleanup_generation));
            log_heap_state("PPE cancel cleanup heap");
            continue;
        }

        uint32_t pending_error_generation = 0;
        ppe_detection_event_t pending_error_event = {};
        if (get_pending_error(&pending_error_generation, &pending_error_event)) {
            cleanup_attempt(pending_error_generation, true, &pending_error_event);
            log_heap_state("PPE cleanup complete");
            continue;
        }

        processing_frame_t frame = {};
        if (!get_frame(&frame)) {
            continue;
        }

        auth_camera_preview_stop_and_release_idle_resources();
        log_heap_state("PPE inference start after camera release");

        ppe_detection_event_t frame_event = {};
        frame_event.type = PPE_EVT_FRAME_READY;
        snprintf(frame_event.message, sizeof(frame_event.message), "正在检测头盔和护目镜...");
        if (!emit_event_if_current(frame.generation, &frame_event, false)) {
            free_processing_frame(&frame);
            clear_processing(frame.generation);
            continue;
        }

        ppe_detection_event_t event = {};
        dl::image::img_t img = {};
        img.data = frame.data;
        img.width = frame.width;
        img.height = frame.height;
        // Although the CSI device negotiates V4L2_PIX_FMT_RGB24, the observed
        // byte order on this board is B, G, R. Let the official ESP-DL image
        // transformer convert BGR888 to the model's RGB tensor.
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888;

        ppe_detect::PpeDetector detector(PPE_SCORE_THRESHOLD, PPE_NMS_THRESHOLD);
        ppe_detect::detection_scores_t scores = {};
        esp_err_t run_ret = detector.run(img, &scores);
        if (run_ret == ESP_OK) {
            event = classify_scores(scores);
        } else {
            ESP_LOGE(TAG, "PPE detector run failed: %s", esp_err_to_name(run_ret));
            event.type = PPE_EVT_ERROR;
            snprintf(event.message, sizeof(event.message), "安全检测异常, 请重试");
        }

        free_processing_frame(&frame);
        clear_processing(frame.generation);
        cleanup_attempt(frame.generation, true, &event);

        log_heap_state("PPE cleanup complete");
    }
}

esp_err_t ppe_detection_start(ppe_detection_event_cb_t cb, void *user_ctx,
                              lv_obj_t *preview_canvas)
{
    if (cb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    lock_state();
    if (s_state.event_emitting &&
        s_state.event_task == self) {
        // Synchronous restart from the PPE callback would wait on this same
        // callback stack. Task 5 retries through the UI event loop instead.
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    unlock_state();

    wait_for_event_idle(true, 0);

    lock_state();
    const bool unavailable = s_state.cleanup_in_progress || s_state.active;
    unlock_state();
    if (unavailable) {
        return ESP_ERR_INVALID_STATE;
    }

    // The safety screen normally owns an RGB565 preview. The verified QAT v3
    // pipeline consumes an uncompressed RGB888 frame, so fully release the
    // preview before reopening the MIPI-CSI device in capture-only mode.
    auth_camera_preview_stop_and_release_idle_resources();

    lock_state();
    if (s_state.cleanup_in_progress) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.active) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = next_generation_locked();
    s_state.active = true;
    s_state.frame_ready = false;
    s_state.pending_error = false;
    s_state.pending_error_generation = 0;
    memset(&s_state.pending_error_event, 0, sizeof(s_state.pending_error_event));
    s_state.cb = cb;
    s_state.user_ctx = user_ctx;
    auth_camera_preview_set_frame_callback(preview_frame_cb,
                                           reinterpret_cast<void *>(static_cast<uintptr_t>(generation)));
    s_state.callback_generation = generation;

    (void)preview_canvas;
    ret = auth_camera_preview_start_with_format(nullptr, nullptr,
                                                APP_VIDEO_FMT_RGB888);
    if (ret != ESP_OK) {
        ppe_detection_event_t event = {};
        event.type = PPE_EVT_ERROR;
        snprintf(event.message, sizeof(event.message), "安全检测异常, 请重试");

        unlock_state();
        cleanup_attempt(generation, true, &event);
        return ret;
    }
    unlock_state();

    ppe_detection_event_t event = {};
    event.type = PPE_EVT_STARTED;
    snprintf(event.message, sizeof(event.message), "正在启动安全检测...");
    emit_event_if_current(generation, &event, false);

    return ESP_OK;
}

void ppe_detection_cancel(void)
{
    if (s_state.lock == nullptr) {
        auth_camera_preview_stop();
        return;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    lock_state();
    if (s_state.event_emitting && s_state.event_task == self) {
        // PPE callbacks must schedule cancel/retry asynchronously; synchronous
        // cancel from this callback would wait on the same callback stack.
        unlock_state();
        ESP_LOGW(TAG, "Ignoring synchronous PPE cancel from PPE callback");
        return;
    }
    unlock_state();

    bool unregister_callback = false;
    bool notify_cleanup = false;
    uint32_t generation = 0;
    TaskHandle_t task = nullptr;

    lock_state();
    generation = s_state.generation;
    task = s_state.task;
    bool has_attempt_work = s_state.active ||
                            s_state.callback_generation == generation ||
                            s_state.frame_generation == generation ||
                            s_state.pending_error_generation == generation ||
                            (s_state.processing && s_state.processing_generation == generation);

    if (!has_attempt_work) {
        unlock_state();
        return;
    }

    if (s_state.callback_generation == generation) {
        unregister_callback = true;
        s_state.callback_generation = 0;
    }
    if (s_state.frame_generation == generation) {
        clear_frame_locked();
    }
    if (s_state.pending_error_generation == generation) {
        s_state.pending_error = false;
        s_state.pending_error_generation = 0;
        memset(&s_state.pending_error_event, 0, sizeof(s_state.pending_error_event));
    }
    s_state.active = false;
    s_state.cb = nullptr;
    s_state.user_ctx = nullptr;
    next_generation_locked();
    if (!s_state.cleanup_in_progress) {
        s_state.cleanup_in_progress = true;
        s_state.cleanup_requested = true;
        s_state.cleanup_request_generation = generation;
        notify_cleanup = true;
    }
    unlock_state();

    if (unregister_callback) {
        auth_camera_preview_set_frame_callback(nullptr, nullptr);
    }
    auth_camera_preview_stop();
    if (notify_cleanup && task != nullptr) {
        xTaskNotifyGive(task);
    }
}

bool ppe_detection_is_active(void)
{
    bool active = false;

    lock_state();
    active = s_state.active;
    unlock_state();

    return active;
}

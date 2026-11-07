#include "usb_uvc_camera_service.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif
#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "esp_private/uvc_esp_video.h"

static const char *TAG = "usb_uvc_camera";

#define USB_UVC_DEFAULT_H_RES       1280
#define USB_UVC_DEFAULT_V_RES       720
#define USB_UVC_DEFAULT_FPS         30
#define USB_UVC_PREVIEW_SOURCE_H_RES 1280
#define USB_UVC_PREVIEW_SOURCE_V_RES 720
#define USB_UVC_PREVIEW_OUTPUT_H_RES 1024
#define USB_UVC_PREVIEW_OUTPUT_V_RES 600
#define USB_UVC_FRAME_QUEUE_LEN     2
#define USB_UVC_TASK_STACK_SIZE     (1024 * 8)
#define USB_UVC_TASK_PRIORITY       8
#define USB_UVC_PREVIEW_TASK_CORE   1
#define USB_UVC_PREVIEW_IDLE_MS     2
#define USB_UVC_STREAM_OPEN_MS      5000
#define USB_UVC_WAIT_DEVICE_MS      5000
#define USB_UVC_PREVIEW_STOP_MS     3000
#define USB_UVC_CLOSE_RETRY_COUNT   10
#define USB_UVC_CLOSE_RETRY_MS      50
#define USB_UVC_CAPTURE_WARMUP_FRAMES 30
#define USB_UVC_FALLBACK_ALIGN      64
#define USB_UVC_JPEG_HW_TIMEOUT_MS  100
#define USB_UVC_URB_COUNT           4

#if CONFIG_SPIRAM
#define USB_UVC_DRIVER_FB_COUNT     4
#define USB_UVC_FRAME_HEAP_CAPS     MALLOC_CAP_SPIRAM
#else
#define USB_UVC_DRIVER_FB_COUNT     3
#define USB_UVC_FRAME_HEAP_CAPS     0
#endif

#define USB_UVC_EVENT_CONNECTED     BIT0
#define USB_UVC_EVENT_PREVIEW_DONE  BIT1
#define USB_UVC_EVENT_SNAPSHOT_DONE BIT2

#define UVC_INTERVAL_TO_FPS(interval) (((interval) != 0) ? (10000000.0f / (float)(interval)) : 0.0f)

static uint32_t align_up_to_16(uint32_t value)
{
    return (value + 15U) & ~15U;
}

static size_t rgb565_decode_buffer_size(uint32_t width, uint32_t height)
{
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    return (size_t)align_up_to_16(width) * align_up_to_16(height) * sizeof(uint16_t);
#else
    return (size_t)width * height * sizeof(uint16_t);
#endif
}

static size_t rgb888_decode_buffer_size(uint32_t width, uint32_t height)
{
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    return (size_t)align_up_to_16(width) * align_up_to_16(height) * 3U;
#else
    return (size_t)width * height * 3U;
#endif
}

static uint8_t *alloc_jpeg_decode_buffer(size_t requested_size, size_t *allocated_size)
{
    if (allocated_size == NULL) {
        return NULL;
    }
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    return (uint8_t *)jpeg_alloc_decoder_mem(requested_size, &mem_cfg, allocated_size);
#else
    *allocated_size = requested_size;
    return (uint8_t *)heap_caps_aligned_alloc(USB_UVC_FALLBACK_ALIGN,
                                               requested_size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

typedef enum {
    USB_UVC_MODE_IDLE = 0,
    USB_UVC_MODE_PREVIEW,
    USB_UVC_MODE_CAPTURE,
} usb_uvc_mode_t;

typedef struct {
    uvc_host_stream_hdl_t stream;
    uvc_host_frame_t *frame;
} uvc_frame_queue_item_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t frame_return_lock;
    EventGroupHandle_t events;
    QueueHandle_t frame_queue;
    TaskHandle_t usb_lib_task;
    TaskHandle_t preview_task;
    uvc_host_stream_hdl_t stream;
    uint8_t dev_addr;
    uint8_t uvc_stream_index;
    uvc_host_frame_info_t *frame_info;
    size_t frame_info_num;
    size_t active_frame_index;
    bool usb_host_ready;
    bool uvc_ready;
    bool active;
    bool streaming;
    usb_uvc_mode_t mode;
    usb_uvc_camera_frame_cb_t frame_cb;
    void *frame_cb_ctx;
    uint8_t *preview_decode_rgb565[2];
    size_t preview_decode_rgb565_size[2];
    uint8_t *preview_rgb565[2];
    size_t preview_rgb565_size[2];
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    jpeg_decoder_handle_t preview_jpeg_decoder;
#endif
#if CONFIG_SOC_PPA_SUPPORTED
    ppa_client_handle_t preview_ppa;
#endif
    int preview_buf_index;
    uint32_t preview_stat_frames;
    uint32_t preview_stat_last_jpeg;
    int64_t preview_stat_total_us;
    int64_t preview_stat_last_us;
    uint32_t overflow_count;
    int64_t overflow_log_last_us;
    bool jpeg_format_logged;
    bool snapshot_pending;
    esp_err_t snapshot_ret;
    char snapshot_path[128];
} usb_uvc_state_t;

static usb_uvc_state_t s_uvc = {};

static void lock_state(void)
{
    if (s_uvc.lock != NULL) {
        xSemaphoreTake(s_uvc.lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_uvc.lock != NULL) {
        xSemaphoreGive(s_uvc.lock);
    }
}

static esp_err_t ensure_primitives(void)
{
    if (s_uvc.lock == NULL) {
        s_uvc.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uvc.lock != NULL, ESP_ERR_NO_MEM, TAG, "Create state mutex failed");
    }
    if (s_uvc.frame_return_lock == NULL) {
        s_uvc.frame_return_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uvc.frame_return_lock != NULL, ESP_ERR_NO_MEM, TAG, "Create frame return mutex failed");
    }
    if (s_uvc.events == NULL) {
        s_uvc.events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_uvc.events != NULL, ESP_ERR_NO_MEM, TAG, "Create event group failed");
    }
    if (s_uvc.frame_queue == NULL) {
        s_uvc.frame_queue = xQueueCreate(USB_UVC_FRAME_QUEUE_LEN, sizeof(uvc_frame_queue_item_t));
        ESP_RETURN_ON_FALSE(s_uvc.frame_queue != NULL, ESP_ERR_NO_MEM, TAG, "Create frame queue failed");
    }
    return ESP_OK;
}

static void usb_lib_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host devices freed");
        }
    }
}

static esp_err_t ensure_usb_host(void)
{
    if (s_uvc.usb_host_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret == ESP_OK && s_uvc.usb_lib_task == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(usb_lib_task,
                                                      "usb_lib",
                                                      4096,
                                                      NULL,
                                                      15,
                                                      &s_uvc.usb_lib_task,
                                                      tskNO_AFFINITY);
        ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "Create USB host lib task failed");
    }

    s_uvc.usb_host_ready = true;
    return ESP_OK;
}

static void clear_frame_list_locked(void)
{
    if (s_uvc.frame_info != NULL) {
        free(s_uvc.frame_info);
        s_uvc.frame_info = NULL;
    }
    s_uvc.frame_info_num = 0;
    s_uvc.active_frame_index = 0;
    s_uvc.dev_addr = 0;
    s_uvc.uvc_stream_index = 0;
}

static void return_queued_frames(uvc_host_stream_hdl_t stream)
{
    if (s_uvc.frame_queue == NULL) {
        return;
    }

    uvc_frame_queue_item_t item = {};
    while (xQueueReceive(s_uvc.frame_queue, &item, 0) == pdTRUE) {
        if (item.frame == NULL) {
            continue;
        }
        uvc_host_stream_hdl_t return_stream = item.stream != NULL ? item.stream : stream;
        if (return_stream == NULL) {
            ESP_LOGW(TAG, "Drop queued UVC frame because stream is NULL");
            continue;
        }
        esp_err_t ret = uvc_host_frame_return(return_stream, item.frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Return queued UVC frame failed: %s", esp_err_to_name(ret));
        }
    }
}

static void disconnected_close_task(void *arg)
{
    uvc_host_stream_hdl_t stream = (uvc_host_stream_hdl_t)arg;
    if (stream != NULL) {
        for (int i = 0; i < USB_UVC_CLOSE_RETRY_COUNT; i++) {
            /* Let the processing task acquire this lock between retries so it
             * can return the last frame retained before disconnection. */
            xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
            esp_err_t ret = uvc_host_stream_close(stream);
            xSemaphoreGive(s_uvc.frame_return_lock);
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "Disconnected stream close retry %d failed: %s", i + 1, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(USB_UVC_CLOSE_RETRY_MS));
        }
    }
    vTaskDelete(NULL);
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "UVC transfer error: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED: {
        ESP_LOGW(TAG, "UVC device disconnected");
        uvc_host_stream_hdl_t disconnected_stream = event->device_disconnected.stream_hdl;
        xEventGroupClearBits(s_uvc.events, USB_UVC_EVENT_CONNECTED);

        lock_state();
        s_uvc.active = false;
        s_uvc.streaming = false;
        if (s_uvc.stream == disconnected_stream) {
            s_uvc.stream = NULL;
        }
        clear_frame_list_locked();
        TaskHandle_t preview_task = s_uvc.preview_task;
        unlock_state();

        return_queued_frames(disconnected_stream);
        if (preview_task != NULL) {
            xTaskNotifyGive(preview_task);
        }
        if (disconnected_stream != NULL) {
            BaseType_t task_ret = xTaskCreate(disconnected_close_task,
                                              "uvc_disconnect",
                                              4096,
                                              disconnected_stream,
                                              5,
                                              NULL);
            if (task_ret != pdPASS) {
                ESP_LOGW(TAG, "Create disconnected close task failed");
            }
        }
        break;
    }
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        s_uvc.overflow_count++;
        if (s_uvc.overflow_log_last_us == 0 ||
            esp_timer_get_time() - s_uvc.overflow_log_last_us >= 1000000) {
            ESP_LOGW(TAG,
                     "UVC frame buffer overflow: dropped=%u in current interval",
                     (unsigned int)s_uvc.overflow_count);
            s_uvc.overflow_count = 0;
            s_uvc.overflow_log_last_us = esp_timer_get_time();
        }
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGD(TAG, "UVC frame buffer underflow");
        break;
    default:
        ESP_LOGW(TAG, "Unhandled UVC stream event: %d", event->type);
        break;
    }
}

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    if (frame == NULL) {
        return true;
    }
    if (frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        ESP_LOGW(TAG, "Drop unsupported UVC frame format: %d", frame->vs_format.format);
        return true;
    }

    lock_state();
    bool active = s_uvc.active;
    QueueHandle_t queue = s_uvc.frame_queue;
    uvc_host_stream_hdl_t stream = s_uvc.stream;
    unlock_state();

    if (!active || queue == NULL || stream == NULL) {
        return true;
    }

    uvc_frame_queue_item_t item = {
        .stream = stream,
        .frame = (uvc_host_frame_t *)frame,
    };
    if (xQueueSend(queue, &item, 0) != pdPASS) {
        return true;
    }
    return false;
}

static void driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED: {
        ESP_LOGI(TAG,
                 "UVC device connected: addr=%u stream=%u frames=%u",
                 (unsigned int)event->device_connected.dev_addr,
                 (unsigned int)event->device_connected.uvc_stream_index,
                 (unsigned int)event->device_connected.frame_info_num);

        bool replacing_stale_device = false;
        if ((xEventGroupGetBits(s_uvc.events) & USB_UVC_EVENT_CONNECTED) != 0) {
            lock_state();
            const uint8_t old_dev_addr = s_uvc.dev_addr;
            const uint8_t old_stream_index = s_uvc.uvc_stream_index;
            const bool same_device = s_uvc.frame_info != NULL &&
                                     old_dev_addr == event->device_connected.dev_addr &&
                                     old_stream_index == event->device_connected.uvc_stream_index;
            const bool idle_for_reenumeration = !s_uvc.active && !s_uvc.streaming &&
                                                s_uvc.stream == NULL;
            unlock_state();

            if (same_device) {
                ESP_LOGI(TAG,
                         "Ignore duplicate UVC connection event: addr=%u stream=%u",
                         (unsigned int)old_dev_addr,
                         (unsigned int)old_stream_index);
                return;
            }
            if (!idle_for_reenumeration) {
                ESP_LOGW(TAG,
                         "Only one active UVC camera is supported: old=%u/%u new=%u/%u",
                         (unsigned int)old_dev_addr,
                         (unsigned int)old_stream_index,
                         (unsigned int)event->device_connected.dev_addr,
                         (unsigned int)event->device_connected.uvc_stream_index);
                return;
            }

            /* Some UVC cameras reset and enumerate with a new USB address after
             * a stream containing malformed/missing EOF packets is stopped.
             * There is no stream-disconnect callback when the old stream was
             * already closed, so the connected event bit and frame list still
             * describe the old address. Block new opens until the replacement
             * frame list is ready, then atomically publish the new address. */
            ESP_LOGW(TAG,
                     "Replacing stale idle UVC device: old=%u/%u new=%u/%u",
                     (unsigned int)old_dev_addr,
                     (unsigned int)old_stream_index,
                     (unsigned int)event->device_connected.dev_addr,
                     (unsigned int)event->device_connected.uvc_stream_index);
            replacing_stale_device = true;
            xEventGroupClearBits(s_uvc.events, USB_UVC_EVENT_CONNECTED);
        }

        size_t frame_info_num = event->device_connected.frame_info_num;
        uvc_host_frame_info_t *all_frames = (uvc_host_frame_info_t *)calloc(frame_info_num, sizeof(uvc_host_frame_info_t));
        if (all_frames == NULL) {
            ESP_LOGE(TAG, "Allocate UVC frame list failed");
            return;
        }

        esp_err_t ret = uvc_host_get_frame_list(event->device_connected.dev_addr,
                                                event->device_connected.uvc_stream_index,
                                                (uvc_host_frame_info_t (*)[])all_frames,
                                                &frame_info_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Get UVC frame list failed: %s", esp_err_to_name(ret));
            free(all_frames);
            return;
        }

        uvc_host_frame_info_t *mjpeg_frames =
            (uvc_host_frame_info_t *)calloc(frame_info_num, sizeof(uvc_host_frame_info_t));
        if (mjpeg_frames == NULL) {
            ESP_LOGE(TAG, "Allocate MJPEG frame list failed");
            free(all_frames);
            return;
        }

        size_t pick_count = 0;
        for (size_t i = 0; i < frame_info_num; i++) {
            float fps = UVC_INTERVAL_TO_FPS(all_frames[i].default_interval);
            if (all_frames[i].format == UVC_VS_FORMAT_MJPEG) {
                mjpeg_frames[pick_count++] = all_frames[i];
                ESP_LOGI(TAG,
                         "  pick MJPEG frame[%u] %ux%u@%.1ffps",
                         (unsigned int)i,
                         (unsigned int)all_frames[i].h_res,
                         (unsigned int)all_frames[i].v_res,
                         fps);
            } else {
                ESP_LOGI(TAG,
                         "  drop frame[%u] %ux%u format=%d",
                         (unsigned int)i,
                         (unsigned int)all_frames[i].h_res,
                         (unsigned int)all_frames[i].v_res,
                         all_frames[i].format);
            }
        }
        free(all_frames);

        if (pick_count == 0) {
            ESP_LOGE(TAG, "No MJPEG UVC frame is supported");
            free(mjpeg_frames);
            return;
        }

        lock_state();
        if (replacing_stale_device &&
            (s_uvc.active || s_uvc.streaming || s_uvc.stream != NULL)) {
            unlock_state();
            ESP_LOGW(TAG, "UVC became busy while replacing the stale device");
            free(mjpeg_frames);
            return;
        }
        clear_frame_list_locked();
        s_uvc.dev_addr = event->device_connected.dev_addr;
        s_uvc.uvc_stream_index = event->device_connected.uvc_stream_index;
        s_uvc.frame_info = mjpeg_frames;
        s_uvc.frame_info_num = pick_count;
        unlock_state();

        xEventGroupSetBits(s_uvc.events, USB_UVC_EVENT_CONNECTED);
        break;
    }
    default:
        ESP_LOGW(TAG, "Unhandled UVC driver event: %d", event->type);
        break;
    }
}

static esp_err_t ensure_uvc_host(void)
{
    if (s_uvc.uvc_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Installing UVC driver");
    const uvc_host_driver_config_t uvc_config = {
        .driver_task_stack_size = 5 * 1024,
        .driver_task_priority = configMAX_PRIORITIES - 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = driver_event_cb,
        .user_ctx = NULL,
    };
    esp_err_t ret = uvc_host_install(&uvc_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UVC driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_uvc.uvc_ready = true;
    return ESP_OK;
}

static esp_err_t wait_for_device(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_uvc.events,
                                           USB_UVC_EVENT_CONNECTED,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & USB_UVC_EVENT_CONNECTED) == 0) {
        ESP_LOGE(TAG, "No UVC device within %u ms", (unsigned int)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static size_t select_frame_index(uint32_t preferred_width,
                                 uint32_t preferred_height,
                                 bool allow_smaller)
{
    size_t selected = SIZE_MAX;
    uint32_t selected_area = 0;

    lock_state();
    if (s_uvc.frame_info == NULL || s_uvc.frame_info_num == 0) {
        unlock_state();
        return SIZE_MAX;
    }

    for (size_t i = 0; i < s_uvc.frame_info_num; i++) {
        const uvc_host_frame_info_t *info = &s_uvc.frame_info[i];
        if (info->h_res == preferred_width && info->v_res == preferred_height) {
            selected = i;
            break;
        }
    }

    if (selected == SIZE_MAX && allow_smaller) {
        for (size_t i = 0; i < s_uvc.frame_info_num; i++) {
            const uvc_host_frame_info_t *info = &s_uvc.frame_info[i];
            if (info->h_res > preferred_width || info->v_res > preferred_height) {
                continue;
            }
            uint32_t area = (uint32_t)info->h_res * (uint32_t)info->v_res;
            if (area > selected_area) {
                selected = i;
                selected_area = area;
            }
        }
    }

    if (selected == SIZE_MAX && !allow_smaller) {
        selected = 0;
    }
    unlock_state();
    return selected;
}

static esp_err_t copy_frame_info(size_t frame_index, uvc_host_frame_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    if (s_uvc.frame_info == NULL || frame_index >= s_uvc.frame_info_num) {
        unlock_state();
        return ESP_ERR_INVALID_ARG;
    }
    *out_info = s_uvc.frame_info[frame_index];
    unlock_state();
    return ESP_OK;
}

static esp_err_t close_stream(void)
{
    lock_state();
    uvc_host_stream_hdl_t stream = s_uvc.stream;
    bool streaming = s_uvc.streaming;
    unlock_state();

    if (stream == NULL) {
        lock_state();
        s_uvc.streaming = false;
        unlock_state();
        return ESP_OK;
    }

    xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
    if (streaming) {
        esp_err_t stop_ret = uvc_host_stream_stop(stream);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "UVC stream stop failed: %s", esp_err_to_name(stop_ret));
        }
    }

    return_queued_frames(stream);

    esp_err_t close_ret = ESP_FAIL;
    for (int i = 0; i < USB_UVC_CLOSE_RETRY_COUNT; i++) {
        close_ret = uvc_host_stream_close(stream);
        if (close_ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "UVC stream close retry %d failed: %s", i + 1, esp_err_to_name(close_ret));
        vTaskDelay(pdMS_TO_TICKS(USB_UVC_CLOSE_RETRY_MS));
    }
    xSemaphoreGive(s_uvc.frame_return_lock);

    if (close_ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC stream close failed: %s", esp_err_to_name(close_ret));
        return close_ret;
    }

    lock_state();
    if (s_uvc.stream == stream) {
        s_uvc.stream = NULL;
    }
    s_uvc.streaming = false;
    unlock_state();
    return ESP_OK;
}

static esp_err_t open_stream_with_index(size_t frame_index, uvc_host_frame_info_t *selected_info)
{
    ESP_RETURN_ON_ERROR(close_stream(), TAG, "Close previous UVC stream failed");

    s_uvc.overflow_count = 0;
    s_uvc.overflow_log_last_us = 0;

    uvc_host_frame_info_t frame_info = {};
    ESP_RETURN_ON_ERROR(copy_frame_info(frame_index, &frame_info), TAG, "Copy UVC frame info failed");

    lock_state();
    uint8_t dev_addr = s_uvc.dev_addr;
    uint8_t stream_index = s_uvc.uvc_stream_index;
    unlock_state();

    uvc_host_stream_config_t cfg = {
        .event_cb = stream_event_cb,
        .frame_cb = frame_callback,
        .user_ctx = NULL,
        .usb = {
            .dev_addr = dev_addr,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = stream_index,
        },
        .vs_format = {
            .h_res = frame_info.h_res,
            .v_res = frame_info.v_res,
            .fps = UVC_INTERVAL_TO_FPS(frame_info.default_interval),
            .format = frame_info.format,
        },
        .advanced = {
            .number_of_frame_buffers = USB_UVC_DRIVER_FB_COUNT,
            /* Zero makes usb_host_uvc allocate the camera-negotiated
             * dwMaxVideoFrameSize. A fixed JPEG compression estimate is not
             * safe: detailed or noisy scenes can produce much larger MJPEG
             * frames and otherwise overflow every frame buffer. */
            .frame_size = 0,
            .frame_heap_caps = USB_UVC_FRAME_HEAP_CAPS,
            /* Espressif's basic UVC example and driver tests use four URBs.
             * This camera negotiates one 3060-byte ISOC packet per URB, so
             * keeping four transfers in flight gives the host enough margin
             * while JPEG/PPA/LCD work runs on the other core. */
            .number_of_urbs = USB_UVC_URB_COUNT,
            .urb_size = 10 * 1024,
            .user_frame_buffers = NULL,
        },
    };

    ESP_LOGI(TAG,
             "Opening UVC stream %ux%u@%.1ffps",
             (unsigned int)frame_info.h_res,
             (unsigned int)frame_info.v_res,
             cfg.vs_format.fps);

    uvc_host_stream_hdl_t stream = NULL;
    esp_err_t ret = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(USB_UVC_STREAM_OPEN_MS), &stream);
    ESP_RETURN_ON_ERROR(ret, TAG, "Open UVC stream failed");

    uvc_host_buf_info_t buf_info = {};
    ret = uvc_host_buf_info_get(stream, &buf_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "UVC negotiated frame buffer: %u bytes x %u buffers",
                 (unsigned int)buf_info.dwMaxVideoFrameSize,
                 (unsigned int)USB_UVC_DRIVER_FB_COUNT);
    } else {
        ESP_LOGW(TAG, "Read negotiated UVC frame size failed: %s", esp_err_to_name(ret));
    }

    /* Keep ownership in the shared state immediately. If format selection or
     * stream start fails, close_stream() can still reclaim the opened handle. */
    lock_state();
    s_uvc.stream = stream;
    s_uvc.streaming = false;
    unlock_state();

    ret = uvc_host_stream_format_select(stream, &cfg.vs_format);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC format select failed: %s", esp_err_to_name(ret));
        (void)close_stream();
        return ret;
    }

    ret = uvc_host_stream_start(stream);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC stream start failed: %s", esp_err_to_name(ret));
        (void)close_stream();
        return ret;
    }

    lock_state();
    s_uvc.streaming = true;
    s_uvc.active_frame_index = frame_index;
    unlock_state();

    if (selected_info != NULL) {
        *selected_info = frame_info;
    }
    return ESP_OK;
}

static const char *jpeg_error_to_name(jpeg_error_t err)
{
    switch (err) {
    case JPEG_ERR_OK:
        return "JPEG_ERR_OK";
    case JPEG_ERR_FAIL:
        return "JPEG_ERR_FAIL";
    case JPEG_ERR_NO_MEM:
        return "JPEG_ERR_NO_MEM";
    case JPEG_ERR_NO_MORE_DATA:
        return "JPEG_ERR_NO_MORE_DATA";
    case JPEG_ERR_INVALID_PARAM:
        return "JPEG_ERR_INVALID_PARAM";
    case JPEG_ERR_BAD_DATA:
        return "JPEG_ERR_BAD_DATA";
    case JPEG_ERR_UNSUPPORT_FMT:
        return "JPEG_ERR_UNSUPPORT_FMT";
    case JPEG_ERR_UNSUPPORT_STD:
        return "JPEG_ERR_UNSUPPORT_STD";
    default:
        return "JPEG_ERR_UNKNOWN";
    }
}

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
static const char *jpeg_sampling_name(jpeg_down_sampling_type_t sampling)
{
    switch (sampling) {
    case JPEG_DOWN_SAMPLING_YUV444:
        return "YUV444";
    case JPEG_DOWN_SAMPLING_YUV422:
        return "YUV422";
    case JPEG_DOWN_SAMPLING_YUV420:
        return "YUV420";
    case JPEG_DOWN_SAMPLING_GRAY:
        return "GRAY";
    default:
        return "UNKNOWN";
    }
}

static void log_jpeg_format_once(const uvc_host_frame_t *frame)
{
    lock_state();
    bool logged = s_uvc.jpeg_format_logged;
    unlock_state();
    if (logged) {
        return;
    }

    jpeg_decode_picture_info_t info = {};
    esp_err_t ret = jpeg_decoder_get_info(frame->data, frame->data_len, &info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read USB MJPEG header failed: %s", esp_err_to_name(ret));
        return;
    }

    lock_state();
    if (s_uvc.jpeg_format_logged) {
        unlock_state();
        return;
    }
    s_uvc.jpeg_format_logged = true;
    unlock_state();

    ESP_LOGI(TAG,
             "USB MJPEG source: %ux%u sampling=%s; decoder=esp_new_jpeg",
             (unsigned int)info.width,
             (unsigned int)info.height,
             jpeg_sampling_name(info.sample_method));
}
#else
static void log_jpeg_format_once(const uvc_host_frame_t *frame)
{
    (void)frame;
}
#endif

static esp_err_t decode_mjpeg_to_rgb565(const uvc_host_frame_t *frame,
                                        uint8_t *out_buf,
                                        size_t out_size,
                                        uint32_t *out_width,
                                        uint32_t *out_height)
{
    if (frame == NULL || frame->data == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        ESP_LOGE(TAG, "Unsupported UVC frame format: %d", frame->vs_format.format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    log_jpeg_format_once(frame);

    // Decode MJPEG through the standards-compliant codec instead of the P4
    // hardware YUV420 RGB conversion path. USB cameras commonly emit YUV422
    // JPEGs; esp_new_jpeg reads the sampling factors from each JPEG header and
    // produces explicit little-endian RGB565 for both the LCD and ESP-DL.
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_error_t jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG decoder open failed: %s", jpeg_error_to_name(jpeg_ret));
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = {
        .inbuf = frame->data,
        .inbuf_len = (int)frame->data_len,
        .outbuf = out_buf,
    };
    jpeg_dec_header_info_t header = {};

    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &io, &header);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG parse header failed: %s len=%u",
                 jpeg_error_to_name(jpeg_ret),
                 (unsigned int)frame->data_len);
        jpeg_dec_close(jpeg_dec);
        return ESP_FAIL;
    }
    if (header.width != frame->vs_format.h_res || header.height != frame->vs_format.v_res) {
        ESP_LOGE(TAG,
                 "MJPEG header does not match negotiated frame: header=%ux%u negotiated=%ux%u",
                 (unsigned int)header.width,
                 (unsigned int)header.height,
                 (unsigned int)frame->vs_format.h_res,
                 (unsigned int)frame->vs_format.v_res);
        jpeg_dec_close(jpeg_dec);
        return ESP_ERR_INVALID_SIZE;
    }

    int required_len = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &required_len);
    if (jpeg_ret != JPEG_ERR_OK || required_len <= 0) {
        ESP_LOGW(TAG, "MJPEG output length failed: %s", jpeg_error_to_name(jpeg_ret));
        jpeg_dec_close(jpeg_dec);
        return ESP_FAIL;
    }
    if ((size_t)required_len > out_size) {
        ESP_LOGE(TAG,
                 "MJPEG output too large: required=%d buffer=%u header=%ux%u",
                 required_len,
                 (unsigned int)out_size,
                 (unsigned int)header.width,
                 (unsigned int)header.height);
        jpeg_dec_close(jpeg_dec);
        return ESP_ERR_NO_MEM;
    }

    jpeg_ret = jpeg_dec_process(jpeg_dec, &io);
    jpeg_dec_close(jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG decode failed: %s len=%u",
                 jpeg_error_to_name(jpeg_ret),
                 (unsigned int)frame->data_len);
        return ESP_FAIL;
    }

    if (out_width != NULL) {
        *out_width = header.width;
    }
    if (out_height != NULL) {
        *out_height = header.height;
    }
    return ESP_OK;
}

static esp_err_t decode_mjpeg_to_rgb888(const uvc_host_frame_t *frame,
                                        uint8_t *out_buf,
                                        size_t out_size,
                                        uint32_t *out_width,
                                        uint32_t *out_height)
{
    if (frame == NULL || frame->data == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        ESP_LOGE(TAG, "Unsupported UVC frame format: %d", frame->vs_format.format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    log_jpeg_format_once(frame);

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_error_t jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "RGB888 JPEG decoder open failed: %s", jpeg_error_to_name(jpeg_ret));
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = {
        .inbuf = frame->data,
        .inbuf_len = (int)frame->data_len,
        .outbuf = out_buf,
    };
    jpeg_dec_header_info_t header = {};

    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &io, &header);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG RGB888 parse header failed: %s len=%u",
                 jpeg_error_to_name(jpeg_ret), (unsigned int)frame->data_len);
        jpeg_dec_close(jpeg_dec);
        return ESP_FAIL;
    }
    if (header.width != frame->vs_format.h_res || header.height != frame->vs_format.v_res) {
        ESP_LOGE(TAG,
                 "MJPEG header does not match negotiated frame: header=%ux%u negotiated=%ux%u",
                 (unsigned int)header.width, (unsigned int)header.height,
                 (unsigned int)frame->vs_format.h_res, (unsigned int)frame->vs_format.v_res);
        jpeg_dec_close(jpeg_dec);
        return ESP_ERR_INVALID_SIZE;
    }

    int required_len = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &required_len);
    if (jpeg_ret != JPEG_ERR_OK || required_len <= 0 || (size_t)required_len > out_size) {
        ESP_LOGE(TAG, "MJPEG RGB888 output buffer mismatch: ret=%s required=%d available=%u",
                 jpeg_error_to_name(jpeg_ret), required_len, (unsigned int)out_size);
        jpeg_dec_close(jpeg_dec);
        return jpeg_ret == JPEG_ERR_OK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    jpeg_ret = jpeg_dec_process(jpeg_dec, &io);
    jpeg_dec_close(jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG RGB888 decode failed: %s len=%u",
                 jpeg_error_to_name(jpeg_ret), (unsigned int)frame->data_len);
        return ESP_FAIL;
    }

    if (out_width != NULL) {
        *out_width = header.width;
    }
    if (out_height != NULL) {
        *out_height = header.height;
    }
    return ESP_OK;
}

static void save_preview_jpeg_if_requested(const uvc_host_frame_t *frame)
{
    if (frame == NULL || frame->data == NULL || frame->data_len == 0) {
        return;
    }

    char path[sizeof(s_uvc.snapshot_path)] = {0};
    lock_state();
    bool pending = s_uvc.snapshot_pending;
    if (pending) {
        snprintf(path, sizeof(path), "%s", s_uvc.snapshot_path);
    }
    unlock_state();

    if (!pending || path[0] == '\0') {
        return;
    }

    esp_err_t ret = ESP_FAIL;
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open USB preview snapshot failed: %s errno=%d", path, errno);
    } else {
        size_t written = fwrite(frame->data, 1, frame->data_len, fp);
        if (written == frame->data_len) {
            ret = ESP_OK;
            ESP_LOGI(TAG,
                     "USB preview snapshot saved: path=%s bytes=%u",
                     path,
                     (unsigned int)frame->data_len);
        } else {
            ESP_LOGE(TAG,
                     "Write USB preview snapshot failed: path=%s written=%u expected=%u errno=%d",
                     path,
                     (unsigned int)written,
                     (unsigned int)frame->data_len,
                     errno);
        }
        fclose(fp);
    }

    lock_state();
    if (s_uvc.snapshot_pending && strcmp(s_uvc.snapshot_path, path) == 0) {
        s_uvc.snapshot_pending = false;
        s_uvc.snapshot_ret = ret;
    }
    unlock_state();

    if (s_uvc.events != NULL) {
        xEventGroupSetBits(s_uvc.events, USB_UVC_EVENT_SNAPSHOT_DONE);
    }
}

static void free_preview_buffers(void)
{
#if CONFIG_SOC_PPA_SUPPORTED
    if (s_uvc.preview_ppa != NULL) {
        esp_err_t ret = ppa_unregister_client(s_uvc.preview_ppa);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Unregister USB preview PPA client failed: %s", esp_err_to_name(ret));
        }
        s_uvc.preview_ppa = NULL;
    }
#endif
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    if (s_uvc.preview_jpeg_decoder != NULL) {
        esp_err_t ret = jpeg_del_decoder_engine(s_uvc.preview_jpeg_decoder);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Delete USB preview JPEG engine failed: %s", esp_err_to_name(ret));
        }
        s_uvc.preview_jpeg_decoder = NULL;
    }
#endif
    for (int i = 0; i < 2; i++) {
        if (s_uvc.preview_decode_rgb565[i] != NULL) {
            heap_caps_free(s_uvc.preview_decode_rgb565[i]);
            s_uvc.preview_decode_rgb565[i] = NULL;
        }
        s_uvc.preview_decode_rgb565_size[i] = 0;
        if (s_uvc.preview_rgb565[i] != NULL) {
            heap_caps_free(s_uvc.preview_rgb565[i]);
            s_uvc.preview_rgb565[i] = NULL;
        }
        s_uvc.preview_rgb565_size[i] = 0;
    }
    s_uvc.preview_buf_index = 0;
}

static esp_err_t allocate_preview_buffers(void)
{
#if !CONFIG_SOC_JPEG_DECODE_SUPPORTED || !CONFIG_SOC_PPA_SUPPORTED
    ESP_LOGE(TAG, "1280x720 USB preview requires ESP32-P4 JPEG and PPA hardware");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const size_t decode_buf_size = rgb565_decode_buffer_size(USB_UVC_PREVIEW_SOURCE_H_RES,
                                                              USB_UVC_PREVIEW_SOURCE_V_RES);
    const size_t output_buf_size = (size_t)USB_UVC_PREVIEW_OUTPUT_H_RES *
                                   USB_UVC_PREVIEW_OUTPUT_V_RES * sizeof(uint16_t);

    jpeg_decode_engine_cfg_t jpeg_cfg = {
        .intr_priority = 0,
        .timeout_ms = USB_UVC_JPEG_HW_TIMEOUT_MS,
    };
    esp_err_t ret = jpeg_new_decoder_engine(&jpeg_cfg, &s_uvc.preview_jpeg_decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create ESP32-P4 JPEG decoder failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ret = ppa_register_client(&ppa_cfg, &s_uvc.preview_ppa);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register ESP32-P4 PPA preview client failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    for (int i = 0; i < 2; i++) {
        s_uvc.preview_decode_rgb565[i] = alloc_jpeg_decode_buffer(
            decode_buf_size, &s_uvc.preview_decode_rgb565_size[i]);
        s_uvc.preview_rgb565[i] = alloc_jpeg_decode_buffer(
            output_buf_size, &s_uvc.preview_rgb565_size[i]);
        if (s_uvc.preview_decode_rgb565[i] == NULL || s_uvc.preview_rgb565[i] == NULL ||
            s_uvc.preview_decode_rgb565_size[i] < decode_buf_size ||
            s_uvc.preview_rgb565_size[i] < output_buf_size) {
            ESP_LOGE(TAG, "Allocate hardware USB preview buffers %d failed", i);
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }
        memset(s_uvc.preview_rgb565[i], 0, output_buf_size);
        ESP_LOGI(TAG,
                 "USB preview HW buffers %d: decode=%u display=%u",
                 i,
                 (unsigned int)s_uvc.preview_decode_rgb565_size[i],
                 (unsigned int)s_uvc.preview_rgb565_size[i]);
    }

    ESP_LOGI(TAG, "USB preview pipeline: ESP32-P4 JPEG HW -> PPA HW scale -> RGB565 LCD");
    return ESP_OK;

fail:
    free_preview_buffers();
    return ret;
#endif
}

static esp_err_t decode_and_scale_preview_frame(const uvc_host_frame_t *frame,
                                                int buf_index,
                                                uint32_t *out_width,
                                                uint32_t *out_height)
{
#if !CONFIG_SOC_JPEG_DECODE_SUPPORTED || !CONFIG_SOC_PPA_SUPPORTED
    (void)frame;
    (void)buf_index;
    (void)out_width;
    (void)out_height;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (frame == NULL || frame->data == NULL || buf_index < 0 || buf_index >= 2 ||
        s_uvc.preview_jpeg_decoder == NULL || s_uvc.preview_ppa == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decode_picture_info_t info = {};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(frame->data, frame->data_len, &info),
                        TAG, "Read USB MJPEG header for HW decode failed");
    if (info.width != frame->vs_format.h_res || info.height != frame->vs_format.v_res ||
        info.width > USB_UVC_PREVIEW_SOURCE_H_RES ||
        info.height > USB_UVC_PREVIEW_SOURCE_V_RES) {
        ESP_LOGE(TAG,
                 "USB preview JPEG dimensions invalid: header=%ux%u negotiated=%ux%u max=%ux%u",
                 (unsigned int)info.width,
                 (unsigned int)info.height,
                 (unsigned int)frame->vs_format.h_res,
                 (unsigned int)frame->vs_format.v_res,
                 USB_UVC_PREVIEW_SOURCE_H_RES,
                 USB_UVC_PREVIEW_SOURCE_V_RES);
        return ESP_ERR_INVALID_SIZE;
    }

    lock_state();
    bool log_format = !s_uvc.jpeg_format_logged;
    s_uvc.jpeg_format_logged = true;
    unlock_state();
    if (log_format) {
        ESP_LOGI(TAG,
                 "USB MJPEG source: %ux%u sampling=%s; decoder=ESP32-P4 JPEG HW; scaler=PPA HW",
                 (unsigned int)info.width,
                 (unsigned int)info.height,
                 jpeg_sampling_name(info.sample_method));
    }

    const uint32_t aligned_width = align_up_to_16(info.width);
    const uint32_t aligned_height = align_up_to_16(info.height);
    const size_t required_decode_size = (size_t)aligned_width * aligned_height * sizeof(uint16_t);
    if (required_decode_size > s_uvc.preview_decode_rgb565_size[buf_index]) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* The IDF JPEG driver documents BGR order as the little-endian
         * RGB565 byte layout used by LVGL and the BSP LCD. */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    int64_t decode_start_us = esp_timer_get_time();
    esp_err_t ret = jpeg_decoder_process(s_uvc.preview_jpeg_decoder,
                                         &decode_cfg,
                                         frame->data,
                                         frame->data_len,
                                         s_uvc.preview_decode_rgb565[buf_index],
                                         s_uvc.preview_decode_rgb565_size[buf_index],
                                         &decoded_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP32-P4 JPEG hardware decode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    float scale_x = (float)USB_UVC_PREVIEW_OUTPUT_H_RES / (float)info.width;
    float scale_y = (float)USB_UVC_PREVIEW_OUTPUT_V_RES / (float)info.height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    uint32_t scaled_width = (uint32_t)((float)info.width * scale);
    uint32_t scaled_height = (uint32_t)((float)info.height * scale);
    scaled_width &= ~1U;
    scaled_height &= ~1U;
    if (scaled_width == 0 || scaled_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    ppa_srm_oper_config_t ppa_cfg = {
        .in = {
            .buffer = s_uvc.preview_decode_rgb565[buf_index],
            .pic_w = aligned_width,
            .pic_h = aligned_height,
            .block_w = info.width,
            .block_h = info.height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_uvc.preview_rgb565[buf_index],
            .buffer_size = s_uvc.preview_rgb565_size[buf_index],
            .pic_w = scaled_width,
            .pic_h = scaled_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)scaled_width / (float)info.width,
        .scale_y = (float)scaled_height / (float)info.height,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ret = ppa_do_scale_rotate_mirror(s_uvc.preview_ppa, &ppa_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP32-P4 PPA preview scale failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (out_width != NULL) {
        *out_width = scaled_width;
    }
    if (out_height != NULL) {
        *out_height = scaled_height;
    }
    ESP_LOGD(TAG,
             "USB preview HW decode+scale=%lldus source=%ux%u output=%ux%u bytes=%u",
             (long long)(esp_timer_get_time() - decode_start_us),
             (unsigned int)info.width,
             (unsigned int)info.height,
             (unsigned int)scaled_width,
             (unsigned int)scaled_height,
             (unsigned int)decoded_size);
    return ESP_OK;
#endif
}

static void preview_processing_task(void *arg)
{
    (void)arg;

    while (true) {
        lock_state();
        bool active = s_uvc.active;
        usb_uvc_mode_t mode = s_uvc.mode;
        usb_uvc_camera_frame_cb_t cb = s_uvc.frame_cb;
        void *cb_ctx = s_uvc.frame_cb_ctx;
        uvc_host_stream_hdl_t stream = s_uvc.stream;
        unlock_state();

        if (!active || mode != USB_UVC_MODE_PREVIEW) {
            break;
        }

        uvc_frame_queue_item_t item = {};
        if (xQueueReceive(s_uvc.frame_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        int buf_index = s_uvc.preview_buf_index;
        uint8_t *rgb565 = s_uvc.preview_rgb565[buf_index];
        s_uvc.preview_buf_index = (s_uvc.preview_buf_index + 1) % 2;

        if (item.frame != NULL) {
            save_preview_jpeg_if_requested(item.frame);
        }

        if (item.frame != NULL && rgb565 != NULL) {
            uint32_t width = 0;
            uint32_t height = 0;
            size_t jpeg_size = item.frame->data_len;
            int64_t start_us = esp_timer_get_time();
            esp_err_t ret = decode_and_scale_preview_frame(item.frame,
                                                           buf_index,
                                                           &width,
                                                           &height);

            /* The decoded RGB buffer no longer depends on the compressed UVC
             * frame. Return it before LCD drawing so USB reception is never
             * blocked by a display refresh. */
            uvc_host_stream_hdl_t return_stream = item.stream != NULL ? item.stream : stream;
            if (return_stream != NULL) {
                xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
                esp_err_t frame_ret = uvc_host_frame_return(return_stream, item.frame);
                xSemaphoreGive(s_uvc.frame_return_lock);
                if (frame_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Return decoded UVC frame failed: %s", esp_err_to_name(frame_ret));
                }
            }
            item.frame = NULL;

            if (ret == ESP_OK && cb != NULL) {
                cb(rgb565, width, height, cb_ctx);
                int64_t total_us = esp_timer_get_time() - start_us;
                s_uvc.preview_stat_frames++;
                s_uvc.preview_stat_total_us += total_us;
                s_uvc.preview_stat_last_jpeg = jpeg_size;

                int64_t now_us = esp_timer_get_time();
                if (s_uvc.preview_stat_last_us == 0) {
                    s_uvc.preview_stat_last_us = now_us;
                } else if (now_us - s_uvc.preview_stat_last_us >= 1000000) {
                    int64_t avg_us = s_uvc.preview_stat_frames > 0
                                         ? s_uvc.preview_stat_total_us / s_uvc.preview_stat_frames
                                         : 0;
                    ESP_LOGI(TAG,
                             "USB preview fps=%u avg_hw_pipeline_draw=%lldus jpeg_bytes=%u out=%ux%u",
                             (unsigned int)s_uvc.preview_stat_frames,
                             (long long)avg_us,
                             (unsigned int)s_uvc.preview_stat_last_jpeg,
                             (unsigned int)width,
                             (unsigned int)height);
                    s_uvc.preview_stat_frames = 0;
                    s_uvc.preview_stat_total_us = 0;
                    s_uvc.preview_stat_last_us = now_us;
                }
            }
        }

        if (item.frame != NULL) {
            uvc_host_stream_hdl_t return_stream = item.stream != NULL ? item.stream : stream;
            if (return_stream != NULL) {
                xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
                esp_err_t ret = uvc_host_frame_return(return_stream, item.frame);
                xSemaphoreGive(s_uvc.frame_return_lock);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Return UVC frame failed: %s", esp_err_to_name(ret));
                }
            }
        }

        /* Yield between hardware pipeline iterations so the idle task and
         * lower-priority UI work always get scheduled. */
        vTaskDelay(pdMS_TO_TICKS(USB_UVC_PREVIEW_IDLE_MS));
    }

    lock_state();
    s_uvc.preview_task = NULL;
    unlock_state();
    if (s_uvc.events != NULL) {
        xEventGroupSetBits(s_uvc.events, USB_UVC_EVENT_PREVIEW_DONE);
    }
    vTaskDelete(NULL);
}

static esp_err_t wait_for_preview_task_exit(uint32_t timeout_ms)
{
    lock_state();
    TaskHandle_t preview_task = s_uvc.preview_task;
    unlock_state();

    if (preview_task == NULL) {
        return ESP_OK;
    }

    xTaskNotifyGive(preview_task);
    EventBits_t bits = xEventGroupWaitBits(s_uvc.events,
                                           USB_UVC_EVENT_PREVIEW_DONE,
                                           pdTRUE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));

    lock_state();
    bool task_finished = s_uvc.preview_task == NULL;
    unlock_state();
    if ((bits & USB_UVC_EVENT_PREVIEW_DONE) == 0 || !task_finished) {
        ESP_LOGE(TAG, "USB preview task did not stop within %u ms", (unsigned int)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t usb_uvc_camera_start_preview(usb_uvc_camera_frame_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_ERROR(ensure_primitives(), TAG, "Ensure USB UVC primitives failed");
    ESP_RETURN_ON_ERROR(ensure_usb_host(), TAG, "Ensure USB host failed");
    ESP_RETURN_ON_ERROR(ensure_uvc_host(), TAG, "Ensure UVC host failed");
    ESP_RETURN_ON_ERROR(wait_for_device(USB_UVC_WAIT_DEVICE_MS), TAG, "Wait UVC device failed");

    lock_state();
    if (s_uvc.active) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    bool stale_preview_task = s_uvc.preview_task != NULL;
    unlock_state();

    if (stale_preview_task) {
        ESP_LOGW(TAG, "Waiting for stale USB preview task before restart");
        ESP_RETURN_ON_ERROR(wait_for_preview_task_exit(USB_UVC_PREVIEW_STOP_MS),
                            TAG, "Wait stale USB preview task failed");
    }

    lock_state();
    if (s_uvc.active || s_uvc.preview_task != NULL) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    s_uvc.mode = USB_UVC_MODE_PREVIEW;
    s_uvc.frame_cb = cb;
    s_uvc.frame_cb_ctx = user_ctx;
    s_uvc.preview_stat_frames = 0;
    s_uvc.preview_stat_last_jpeg = 0;
    s_uvc.preview_stat_total_us = 0;
    s_uvc.preview_stat_last_us = 0;
    s_uvc.jpeg_format_logged = false;
    s_uvc.preview_buf_index = 0;
    xEventGroupClearBits(s_uvc.events, USB_UVC_EVENT_PREVIEW_DONE);
    unlock_state();

    esp_err_t ret = allocate_preview_buffers();
    if (ret != ESP_OK) {
        free_preview_buffers();
        lock_state();
        s_uvc.mode = USB_UVC_MODE_IDLE;
        s_uvc.frame_cb = NULL;
        s_uvc.frame_cb_ctx = NULL;
        unlock_state();
        return ret;
    }

    size_t frame_index = select_frame_index(USB_UVC_PREVIEW_SOURCE_H_RES,
                                            USB_UVC_PREVIEW_SOURCE_V_RES,
                                            true);
    if (frame_index == SIZE_MAX) {
        free_preview_buffers();
        lock_state();
        s_uvc.mode = USB_UVC_MODE_IDLE;
        s_uvc.frame_cb = NULL;
        s_uvc.frame_cb_ctx = NULL;
        unlock_state();
        return ESP_ERR_NOT_FOUND;
    }

    uvc_host_frame_info_t selected = {};
    ret = open_stream_with_index(frame_index, &selected);
    if (ret != ESP_OK) {
        free_preview_buffers();
        lock_state();
        s_uvc.mode = USB_UVC_MODE_IDLE;
        s_uvc.frame_cb = NULL;
        s_uvc.frame_cb_ctx = NULL;
        unlock_state();
        return ret;
    }

    lock_state();
    s_uvc.active = true;
    unlock_state();

    BaseType_t task_ret = xTaskCreatePinnedToCore(preview_processing_task,
                                                  "usb_uvc_preview",
                                                  USB_UVC_TASK_STACK_SIZE,
                                                  NULL,
                                                  USB_UVC_TASK_PRIORITY,
                                                  &s_uvc.preview_task,
                                                  USB_UVC_PREVIEW_TASK_CORE);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Create USB UVC preview task failed");
        usb_uvc_camera_stop_preview();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "USB preview started at %ux%u",
             (unsigned int)selected.h_res,
             (unsigned int)selected.v_res);
    return ESP_OK;
}

void usb_uvc_camera_stop_preview(void)
{
    lock_state();
    s_uvc.active = false;
    s_uvc.frame_cb = NULL;
    s_uvc.frame_cb_ctx = NULL;
    if (s_uvc.snapshot_pending) {
        s_uvc.snapshot_pending = false;
        s_uvc.snapshot_ret = ESP_ERR_INVALID_STATE;
        if (s_uvc.events != NULL) {
            xEventGroupSetBits(s_uvc.events, USB_UVC_EVENT_SNAPSHOT_DONE);
        }
    }
    unlock_state();

    esp_err_t task_ret = wait_for_preview_task_exit(USB_UVC_PREVIEW_STOP_MS);
    if (task_ret != ESP_OK) {
        /* The task may still be decoding into a preview buffer. Preserve both
         * the stream and buffers so the next stop/retry can recover safely. */
        ESP_LOGE(TAG, "USB preview cleanup deferred until processing task exits");
        return;
    }

    esp_err_t close_ret = close_stream();
    if (close_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB preview stream cleanup deferred: %s", esp_err_to_name(close_ret));
        return;
    }

    lock_state();
    s_uvc.mode = USB_UVC_MODE_IDLE;
    unlock_state();

    free_preview_buffers();
    ESP_LOGI(TAG, "USB preview stopped");
}

bool usb_uvc_camera_preview_active(void)
{
    bool active = false;
    lock_state();
    active = s_uvc.active && s_uvc.mode == USB_UVC_MODE_PREVIEW;
    unlock_state();
    return active;
}

esp_err_t usb_uvc_camera_request_jpeg_snapshot(const char *path, uint32_t timeout_ms)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_uvc.events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    lock_state();
    if (!s_uvc.active || s_uvc.mode != USB_UVC_MODE_PREVIEW || s_uvc.snapshot_pending) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_uvc.snapshot_path, sizeof(s_uvc.snapshot_path), "%s", path);
    s_uvc.snapshot_pending = true;
    s_uvc.snapshot_ret = ESP_ERR_TIMEOUT;
    xEventGroupClearBits(s_uvc.events, USB_UVC_EVENT_SNAPSHOT_DONE);
    unlock_state();

    EventBits_t bits = xEventGroupWaitBits(s_uvc.events,
                                           USB_UVC_EVENT_SNAPSHOT_DONE,
                                           pdTRUE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & USB_UVC_EVENT_SNAPSHOT_DONE) == 0) {
        lock_state();
        if (s_uvc.snapshot_pending) {
            s_uvc.snapshot_pending = false;
            s_uvc.snapshot_ret = ESP_ERR_TIMEOUT;
        }
        unlock_state();
        return ESP_ERR_TIMEOUT;
    }

    lock_state();
    esp_err_t ret = s_uvc.snapshot_ret;
    unlock_state();
    return ret;
}

static esp_err_t capture_frame_internal(uint8_t **out_buf,
                                        size_t *out_len,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        const char *jpeg_path,
                                        bool output_rgb888,
                                        uint32_t timeout_ms)
{
    if (out_buf == NULL || out_len == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;
    *out_width = 0;
    *out_height = 0;

    ESP_RETURN_ON_ERROR(ensure_primitives(), TAG, "Ensure USB UVC primitives failed");
    ESP_RETURN_ON_ERROR(ensure_usb_host(), TAG, "Ensure USB host failed");
    ESP_RETURN_ON_ERROR(ensure_uvc_host(), TAG, "Ensure UVC host failed");
    ESP_RETURN_ON_ERROR(wait_for_device(USB_UVC_WAIT_DEVICE_MS), TAG, "Wait UVC device failed");

    lock_state();
    if (s_uvc.active) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    bool stale_preview_task = s_uvc.preview_task != NULL;
    unlock_state();

    if (stale_preview_task) {
        ESP_LOGW(TAG, "Waiting for USB preview task before still capture");
        ESP_RETURN_ON_ERROR(wait_for_preview_task_exit(USB_UVC_PREVIEW_STOP_MS),
                            TAG, "Wait USB preview task before capture failed");
    }

    lock_state();
    if (s_uvc.active || s_uvc.preview_task != NULL) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    s_uvc.mode = USB_UVC_MODE_CAPTURE;
    s_uvc.active = true;
    s_uvc.jpeg_format_logged = false;
    unlock_state();

    size_t frame_index = select_frame_index(USB_UVC_DEFAULT_H_RES, USB_UVC_DEFAULT_V_RES, false);
    if (frame_index == SIZE_MAX) {
        lock_state();
        s_uvc.active = false;
        s_uvc.mode = USB_UVC_MODE_IDLE;
        unlock_state();
        return ESP_ERR_NOT_FOUND;
    }

    uvc_host_frame_info_t selected = {};
    esp_err_t ret = open_stream_with_index(frame_index, &selected);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    /* A resolution change restarts this camera's AE/AWB loop. Do not use the
     * first unstable frame for either inference or the saved evidence image. */
    ESP_LOGI(TAG, "Warming up UVC auto exposure/white balance: discard=%u frames",
             (unsigned int)USB_UVC_CAPTURE_WARMUP_FRAMES);
    for (uint32_t i = 0; i < USB_UVC_CAPTURE_WARMUP_FRAMES; ++i) {
        uvc_frame_queue_item_t warmup = {};
        if (xQueueReceive(s_uvc.frame_queue, &warmup, pdMS_TO_TICKS(timeout_ms)) != pdTRUE ||
            warmup.frame == NULL) {
            ESP_LOGW(TAG, "No USB UVC warmup frame %u/%u within %u ms",
                     (unsigned int)(i + 1),
                     (unsigned int)USB_UVC_CAPTURE_WARMUP_FRAMES,
                     (unsigned int)timeout_ms);
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
        uvc_host_stream_hdl_t warmup_stream = warmup.stream != NULL ? warmup.stream : s_uvc.stream;
        if (warmup_stream != NULL) {
            xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
            esp_err_t frame_ret = uvc_host_frame_return(warmup_stream, warmup.frame);
            xSemaphoreGive(s_uvc.frame_return_lock);
            if (frame_ret != ESP_OK) {
                ESP_LOGW(TAG, "Return UVC warmup frame failed: %s", esp_err_to_name(frame_ret));
                ret = frame_ret;
                goto cleanup;
            }
        }
    }

    uvc_frame_queue_item_t item = {};
    if (xQueueReceive(s_uvc.frame_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE || item.frame == NULL) {
        ESP_LOGW(TAG, "No USB UVC capture frame within %u ms", (unsigned int)timeout_ms);
        ret = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    size_t decoded_len = output_rgb888
                             ? rgb888_decode_buffer_size(selected.h_res, selected.v_res)
                             : rgb565_decode_buffer_size(selected.h_res, selected.v_res);
    size_t decoded_allocated_size = 0;
    uint8_t *decoded = alloc_jpeg_decode_buffer(decoded_len, &decoded_allocated_size);
    if (decoded == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto return_frame;
    }
    if (decoded_allocated_size < decoded_len) {
        ESP_LOGE(TAG,
                 "USB capture JPEG output buffer is too small: allocated=%u required=%u",
                 (unsigned int)decoded_allocated_size,
                 (unsigned int)decoded_len);
        heap_caps_free(decoded);
        ret = ESP_ERR_NO_MEM;
        goto return_frame;
    }
    ESP_LOGI(TAG,
             "USB capture JPEG %s output buffer: addr=%p bytes=%u valid_bytes=%u",
             output_rgb888 ? "RGB888" : "RGB565LE",
             decoded,
             (unsigned int)decoded_allocated_size,
             (unsigned int)decoded_len);

    uint32_t width = 0;
    uint32_t height = 0;
    ret = output_rgb888
              ? decode_mjpeg_to_rgb888(item.frame, decoded, decoded_allocated_size, &width, &height)
              : decode_mjpeg_to_rgb565(item.frame, decoded, decoded_allocated_size, &width, &height);
    if (ret != ESP_OK) {
        heap_caps_free(decoded);
        goto return_frame;
    }

    if (jpeg_path != NULL && jpeg_path[0] != '\0') {
        FILE *fp = fopen(jpeg_path, "wb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "Open exact UVC capture failed: %s errno=%d", jpeg_path, errno);
            heap_caps_free(decoded);
            ret = ESP_FAIL;
            goto return_frame;
        }
        size_t written = fwrite(item.frame->data, 1, item.frame->data_len, fp);
        int close_ret = fclose(fp);
        if (written != item.frame->data_len || close_ret != 0) {
            ESP_LOGE(TAG,
                     "Write exact UVC capture failed: path=%s written=%u expected=%u errno=%d",
                     jpeg_path,
                     (unsigned int)written,
                     (unsigned int)item.frame->data_len,
                     errno);
            heap_caps_free(decoded);
            remove(jpeg_path);
            ret = ESP_FAIL;
            goto return_frame;
        }
        ESP_LOGI(TAG,
                 "Exact UVC JPEG saved from inference frame: path=%s bytes=%u",
                 jpeg_path,
                 (unsigned int)item.frame->data_len);
    }

    *out_buf = decoded;
    *out_len = (size_t)width * height * (output_rgb888 ? 3U : 2U);
    *out_width = width;
    *out_height = height;
    ESP_LOGI(TAG, "USB UVC captured %s frame: %ux%u",
             output_rgb888 ? "RGB888" : "RGB565LE",
             (unsigned int)width, (unsigned int)height);

return_frame:
    if (item.frame != NULL) {
        uvc_host_stream_hdl_t return_stream = item.stream != NULL ? item.stream : s_uvc.stream;
        if (return_stream != NULL) {
            xSemaphoreTake(s_uvc.frame_return_lock, portMAX_DELAY);
            esp_err_t frame_ret = uvc_host_frame_return(return_stream, item.frame);
            xSemaphoreGive(s_uvc.frame_return_lock);
            if (frame_ret != ESP_OK) {
                ESP_LOGW(TAG, "Return capture frame failed: %s", esp_err_to_name(frame_ret));
            }
        }
    }

cleanup:
    lock_state();
    s_uvc.active = false;
    unlock_state();
    esp_err_t close_ret = close_stream();
    if (close_ret == ESP_OK) {
        lock_state();
        s_uvc.mode = USB_UVC_MODE_IDLE;
        unlock_state();
    } else {
        ESP_LOGE(TAG, "USB capture stream cleanup failed: %s", esp_err_to_name(close_ret));
    }
    if (ret == ESP_OK && close_ret != ESP_OK) {
        if (*out_buf != NULL) {
            heap_caps_free(*out_buf);
            *out_buf = NULL;
        }
        *out_len = 0;
        *out_width = 0;
        *out_height = 0;
        if (jpeg_path != NULL && jpeg_path[0] != '\0') {
            remove(jpeg_path);
        }
        ret = close_ret;
    }
    return ret;
}

esp_err_t usb_uvc_camera_capture_rgb565(uint8_t **out_buf,
                                        size_t *out_len,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint32_t timeout_ms)
{
    return capture_frame_internal(out_buf, out_len, out_width, out_height, NULL, false, timeout_ms);
}

esp_err_t usb_uvc_camera_capture_rgb565_with_jpeg(uint8_t **out_buf,
                                                  size_t *out_len,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  const char *jpeg_path,
                                                  uint32_t timeout_ms)
{
    if (jpeg_path == NULL || jpeg_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return capture_frame_internal(out_buf, out_len, out_width, out_height,
                                  jpeg_path, false, timeout_ms);
}

esp_err_t usb_uvc_camera_capture_rgb888_with_jpeg(uint8_t **out_buf,
                                                  size_t *out_len,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  const char *jpeg_path,
                                                  uint32_t timeout_ms)
{
    if (jpeg_path == NULL || jpeg_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return capture_frame_internal(out_buf, out_len, out_width, out_height,
                                  jpeg_path, true, timeout_ms);
}

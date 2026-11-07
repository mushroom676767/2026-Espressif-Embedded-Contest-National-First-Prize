#include "cabinet_core.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cabinet_door_signal.h"
#include "cloud_uploader.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tool_detection_service.h"
#include "tool_record_store.h"
#include "ui_manager.h"

#define CABINET_CLOSE_TASK_STACK (12 * 1024)
#define CABINET_CLOSE_TASK_PRIO 4
#define CABINET_STATE_SOURCE "touch"
#define CABINET_STATE_SOURCE_VISION "vision"
#define CABINET_VISION_MIN_CONFIDENCE 0.25f
#define CABINET_DEFAULT_OPERATOR "操作员"
#define CABINET_CAPTURE_WAIT_MS 15000
#define CABINET_CLOSE_GRACE_MS 15000

typedef enum {
    CABINET_SESSION_IDLE = 0,
    CABINET_SESSION_OPEN_WAIT,
    CABINET_SESSION_CLOSE_GRACE,
    CABINET_SESSION_NORMAL_POSTING,
    CABINET_SESSION_ALERT_POSTING,
    CABINET_SESSION_ALERTED,
    CABINET_SESSION_RECOVERY_POSTING,
    CABINET_SESSION_CLOSED,
} cabinet_session_state_t;

typedef enum {
    CABINET_SYNC_NORMAL = 0,
    CABINET_SYNC_ALERT,
    CABINET_SYNC_RECOVERY,
} cabinet_sync_mode_t;

static const char *TAG = "cabinet_core";

static tool_slot_runtime_t s_before[TOOL_CABINET_SLOT_COUNT];
static tool_slot_runtime_t s_current[TOOL_CABINET_SLOT_COUNT];
static SemaphoreHandle_t s_lock;
static char s_operator_name[UI_OPERATOR_NAME_MAX_LEN];
static char s_latest_image_url[192];
static auth_type_t s_auth_type = AUTH_TYPE_NONE;
static char s_opened_at[32];
static char s_state_source[16] = CABINET_STATE_SOURCE;
static uint32_t s_event_seq = 1;
static bool s_close_in_progress;
static cabinet_session_state_t s_session_state = CABINET_SESSION_IDLE;
static uint32_t s_session_generation;
static int64_t s_grace_deadline_us;
static int64_t s_capture_deadline_us;
static bool s_close_requested;
static bool s_vision_complete;
static char s_operation_id[64];
static char s_alert_event_id[64];
static char s_inventory_snapshot_path[128];
static char s_inventory_captured_at[32];
static uint32_t s_local_record_id;

static void lock_core(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_core(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static const char *auth_type_to_string(auth_type_t auth_type)
{
    switch (auth_type) {
        case AUTH_TYPE_NFC:
            return "nfc";
        case AUTH_TYPE_FACE:
            return "face";
        case AUTH_TYPE_SAFETY_CHECK:
            return "safety_check";
        case AUTH_TYPE_ADMIN:
            return "admin";
        case AUTH_TYPE_NONE:
        default:
            return "none";
    }
}

static result_type_t result_type_to_ui(const tool_operation_diff_t *diff)
{
    if (diff == NULL) {
        return RESULT_OPERATION_OK;
    }
    if (diff->anomaly_count > 0) {
        return RESULT_ANOMALY;
    }
    if ((diff->borrowed_count == 0 && diff->returned_count == 0) ||
        (diff->borrowed_count > 0 && diff->returned_count > 0)) {
        return RESULT_OPERATION_OK;
    }
    if (diff->borrowed_count > 0) {
        return RESULT_BORROW_OK;
    }
    return RESULT_RETURN_OK;
}

static void fill_cloud_change(cloud_tool_change_t *dst, const tool_change_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->slot_no = src->slot_no;
    snprintf(dst->tool_code, sizeof(dst->tool_code), "%s", src->tool_code);
    snprintf(dst->tool_name, sizeof(dst->tool_name), "%s", src->tool_name);
    snprintf(dst->tool_class, sizeof(dst->tool_class), "%s", src->tool_class);
    dst->quantity = 1;
}

static int fill_cloud_changes(cloud_tool_change_t *dst,
                              const tool_change_t *src,
                              int count)
{
    int out_count = 0;
    int screwdriver_quantity = 0;
    for (int i = 0; i < count && out_count < CLOUD_UPLOADER_MAX_CHANGES; ++i) {
        if (strcmp(src[i].tool_class, "screwdriver") == 0) {
            screwdriver_quantity++;
            continue;
        }
        fill_cloud_change(&dst[out_count++], &src[i]);
    }
    if (screwdriver_quantity > 0 && out_count < CLOUD_UPLOADER_MAX_CHANGES) {
        cloud_tool_change_t *item = &dst[out_count++];
        memset(item, 0, sizeof(*item));
        item->slot_no = 0;
        snprintf(item->tool_code, sizeof(item->tool_code), "%s", "SCREWDRIVER");
        snprintf(item->tool_name, sizeof(item->tool_name), "%s", "螺丝刀");
        snprintf(item->tool_class, sizeof(item->tool_class), "%s", "screwdriver");
        item->quantity = screwdriver_quantity;
    }
    return out_count;
}

static void fill_result_tool(tool_info_t *dst, const tool_change_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->slot_id = src->slot_no;
    snprintf(dst->name, sizeof(dst->name), "%s", src->tool_name);
    snprintf(dst->id, sizeof(dst->id), "%s", src->tool_code);
    dst->status = src->to_status;
}

static void append_record_detail(char *dst, size_t dst_size, const char *text)
{
    if (dst == NULL || dst_size == 0 || text == NULL || text[0] == '\0') {
        return;
    }

    size_t used = strlen(dst);
    if (used + 1 >= dst_size) {
        return;
    }
    snprintf(dst + used, dst_size - used, "%s", text);
}

static void append_change_detail(char *dst,
                                 size_t dst_size,
                                 const char *prefix,
                                 const tool_change_t *changes,
                                 uint8_t count)
{
    if (dst == NULL || dst_size == 0 || prefix == NULL || changes == NULL || count == 0) {
        return;
    }

    if (dst[0] != '\0') {
        append_record_detail(dst, dst_size, "; ");
    }
    append_record_detail(dst, dst_size, prefix);
    append_record_detail(dst, dst_size, ": ");
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) {
            append_record_detail(dst, dst_size, ", ");
        }
        append_record_detail(dst, dst_size, changes[i].tool_name);
    }
}

static void fill_record_detail(char *dst, size_t dst_size, const tool_operation_diff_t *diff)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (diff == NULL || !diff->changed) {
        append_record_detail(dst, dst_size, "无变化");
        return;
    }

    append_change_detail(dst, dst_size, "借出", diff->borrowed, diff->borrowed_count);
    append_change_detail(dst, dst_size, "归还", diff->returned, diff->returned_count);
    append_change_detail(dst, dst_size, "异常", diff->anomalies, diff->anomaly_count);
}

static int count_available(const tool_slot_runtime_t *states)
{
    int available = 0;
    for (size_t i = 0; i < TOOL_CABINET_SLOT_COUNT; i++) {
        if (states[i].status == TOOL_SLOT_AVAILABLE) {
            available++;
        }
    }
    return available;
}

static void fill_snapshot(cloud_snapshot_t *snapshot,
                          const char *event_id,
                          const char *timestamp,
                          const char *captured_at,
                          const tool_slot_runtime_t *states,
                          const char *state_source,
                          bool vision_complete)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snprintf(snapshot->device_code, sizeof(snapshot->device_code), "%s", CONFIG_CLOUD_UPLOADER_DEVICE_CODE);
    snprintf(snapshot->timestamp, sizeof(snapshot->timestamp), "%s", timestamp);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(snapshot->firmware_version, sizeof(snapshot->firmware_version), "%s",
             app_desc != NULL ? app_desc->version : "p4-demo");
    snapshot->total = TOOL_CABINET_SLOT_COUNT;
    snapshot->available = count_available(states);
    snprintf(snapshot->snapshot_id, sizeof(snapshot->snapshot_id), "%s", event_id);
    snprintf(snapshot->captured_at, sizeof(snapshot->captured_at), "%s", captured_at);
    snprintf(snapshot->state_source, sizeof(snapshot->state_source), "%s",
             state_source != NULL ? state_source : CABINET_STATE_SOURCE);
    snapshot->vision_complete = vision_complete;
    snapshot->screwdriver_present =
        (states[0].status == TOOL_SLOT_AVAILABLE ? 1 : 0) +
        (states[1].status == TOOL_SLOT_AVAILABLE ? 1 : 0);

    const tool_catalog_entry_t *catalog = tool_cabinet_catalog();
    snapshot->slot_count = TOOL_CABINET_SLOT_COUNT;
    for (size_t i = 0; i < TOOL_CABINET_SLOT_COUNT; i++) {
        snapshot->slots[i].slot_no = catalog[i].slot_no;
        snprintf(snapshot->slots[i].tool_code, sizeof(snapshot->slots[i].tool_code), "%s", catalog[i].tool_code);
        snprintf(snapshot->slots[i].tool_name, sizeof(snapshot->slots[i].tool_name), "%s", catalog[i].tool_name);
        snprintf(snapshot->slots[i].tool_class, sizeof(snapshot->slots[i].tool_class), "%s", catalog[i].tool_class);
        snprintf(snapshot->slots[i].status, sizeof(snapshot->slots[i].status), "%s",
                 tool_cabinet_status_to_backend(states[i].status));
        lock_core();
        snprintf(snapshot->slots[i].image_url, sizeof(snapshot->slots[i].image_url), "%s", s_latest_image_url);
        unlock_core();
    }
}

static void fill_event(cloud_event_t *event,
                       const char *event_id,
                       const char *operator_name,
                       auth_type_t auth_type,
                       const char *opened_at,
                       const char *closed_at,
                       const char *inspection_image_url,
                       const char *snapshot_local_path,
                       uint32_t local_record_id,
                       const tool_operation_diff_t *diff,
                       const char *state_source,
                       const char *event_type,
                       const char *operation_id,
                       const char *related_event_id,
                       const char *occurred_at,
                       bool door_confirmed)
{
    memset(event, 0, sizeof(*event));
    snprintf(event->event_id, sizeof(event->event_id), "%s", event_id);
    snprintf(event->device_code, sizeof(event->device_code), "%s", CONFIG_CLOUD_UPLOADER_DEVICE_CODE);
    snprintf(event->operator_info.name, sizeof(event->operator_info.name), "%s", operator_name);
    snprintf(event->operator_info.auth_type, sizeof(event->operator_info.auth_type), "%s", auth_type_to_string(auth_type));
    snprintf(event->event_type, sizeof(event->event_type), "%s",
             event_type != NULL ? event_type : "operation");
    snprintf(event->operation_id, sizeof(event->operation_id), "%s",
             operation_id != NULL ? operation_id : "");
    snprintf(event->related_event_id, sizeof(event->related_event_id), "%s",
             related_event_id != NULL ? related_event_id : "");
    snprintf(event->occurred_at, sizeof(event->occurred_at), "%s",
             occurred_at != NULL ? occurred_at : "");
    event->door_confirmed = door_confirmed;
    event->reminder_count = 0;
    fill_record_detail(event->result_type, sizeof(event->result_type), diff);
    if (event->result_type[0] == '\0') {
        snprintf(event->result_type, sizeof(event->result_type), "%s", tool_cabinet_result_type(diff));
    }
    snprintf(event->opened_at, sizeof(event->opened_at), "%s", opened_at);
    snprintf(event->closed_at, sizeof(event->closed_at), "%s", closed_at);
    event->local_record_id = local_record_id;
    snprintf(event->state_source, sizeof(event->state_source), "%s",
             state_source != NULL ? state_source : CABINET_STATE_SOURCE);
    snprintf(event->inspection_image_url, sizeof(event->inspection_image_url), "%s",
             inspection_image_url != NULL ? inspection_image_url : "");
    snprintf(event->snapshot_local_path, sizeof(event->snapshot_local_path), "%s",
             snapshot_local_path != NULL ? snapshot_local_path : "");
    snprintf(event->captured_at, sizeof(event->captured_at), "%s", closed_at);

    if (diff != NULL) {
        event->borrowed_count = fill_cloud_changes(event->borrowed, diff->borrowed,
                                                   diff->borrowed_count);
        event->returned_count = fill_cloud_changes(event->returned, diff->returned,
                                                   diff->returned_count);
        event->anomaly_count = fill_cloud_changes(event->anomalies, diff->anomalies,
                                                  diff->anomaly_count);
    }
}

static void fill_result(operation_result_t *result,
                        const char *operator_name,
                        const char *closed_at,
                        bool cloud_pushed,
                        const tool_operation_diff_t *diff)
{
    memset(result, 0, sizeof(*result));
    snprintf(result->operator_name, sizeof(result->operator_name), "%s", operator_name);
    snprintf(result->timestamp, sizeof(result->timestamp), "%s", closed_at);
    result->result_type = result_type_to_ui(diff);
    result->cloud_pushed = cloud_pushed;

    result->borrowed_count = diff->borrowed_count;
    for (uint8_t i = 0; i < result->borrowed_count && i < TOOL_CABINET_SLOT_COUNT; i++) {
        fill_result_tool(&result->borrowed[i], &diff->borrowed[i]);
    }
    result->returned_count = diff->returned_count;
    for (uint8_t i = 0; i < result->returned_count && i < TOOL_CABINET_SLOT_COUNT; i++) {
        fill_result_tool(&result->returned[i], &diff->returned[i]);
    }
    result->anomaly_count = diff->anomaly_count;
    for (uint8_t i = 0; i < result->anomaly_count && i < TOOL_CABINET_SLOT_COUNT; i++) {
        fill_result_tool(&result->anomalous[i], &diff->anomalies[i]);
    }
}

typedef struct {
    cabinet_sync_mode_t mode;
    uint32_t generation;
} cabinet_sync_job_t;

typedef struct {
    tool_slot_runtime_t before[TOOL_CABINET_SLOT_COUNT];
    tool_slot_runtime_t after[TOOL_CABINET_SLOT_COUNT];
    char operator_name[UI_OPERATOR_NAME_MAX_LEN];
    char opened_at[32];
    char operation_id[64];
    char snapshot_path[128];
    char captured_at[32];
    char state_source[sizeof(s_state_source)];
    char occurred_at[32];
    char event_id[64];
    char image_url[192];
    tool_operation_diff_t diff;
    tool_record_t record;
    cloud_event_t event;
    cloud_snapshot_t snapshot;
    operation_result_t result;
} cabinet_cloud_work_t;

static cabinet_cloud_work_t *allocate_cloud_work(void)
{
    cabinet_cloud_work_t *work = heap_caps_calloc(
        1, sizeof(*work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work == NULL) {
        work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_8BIT);
    }
    return work;
}

static esp_err_t post_recovery_event(uint32_t generation)
{
    char operation_id[64];
    char alert_event_id[64];
    char operator_name[UI_OPERATOR_NAME_MAX_LEN];
    char opened_at[32];
    auth_type_t auth_type;
    uint32_t seq;

    lock_core();
    if (generation != s_session_generation || s_alert_event_id[0] == '\0') {
        unlock_core();
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(operation_id, sizeof(operation_id), "%s", s_operation_id);
    snprintf(alert_event_id, sizeof(alert_event_id), "%s", s_alert_event_id);
    snprintf(operator_name, sizeof(operator_name), "%s", s_operator_name);
    snprintf(opened_at, sizeof(opened_at), "%s", s_opened_at);
    auth_type = s_auth_type;
    seq = s_event_seq++;
    unlock_core();

    time_t now = time(NULL);
    char occurred_at[32];
    char event_id[64];
    tool_cabinet_format_time_iso(occurred_at, sizeof(occurred_at), now);
    tool_cabinet_make_event_id(CONFIG_CLOUD_UPLOADER_DEVICE_CODE, now, seq,
                               event_id, sizeof(event_id));

    cloud_event_t *event = heap_caps_calloc(1, sizeof(*event),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event == NULL) {
        event = heap_caps_calloc(1, sizeof(*event), MALLOC_CAP_8BIT);
    }

    esp_err_t ret = ESP_ERR_NO_MEM;
    if (event != NULL) {
        fill_event(event, event_id, operator_name, auth_type, opened_at, occurred_at,
                   "", "", 0, NULL, CABINET_STATE_SOURCE_VISION,
                   "door_closed", operation_id, alert_event_id, occurred_at, true);
        snprintf(event->result_type, sizeof(event->result_type), "%s", "door_closed");
        ESP_LOGI(TAG, "Door recovery event allocated on heap: bytes=%u stack_free=%u",
                 (unsigned int)sizeof(*event),
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
        ret = cloud_uploader_post_event(event);
        heap_caps_free(event);
    } else {
        ESP_LOGE(TAG, "Allocate door recovery event failed: bytes=%u",
                 (unsigned int)sizeof(*event));
    }
    ESP_LOGI(TAG, "Door recovery post: operation_id=%s related=%s ret=%s",
             operation_id, alert_event_id, esp_err_to_name(ret));

    lock_core();
    if (generation == s_session_generation) {
        s_session_state = CABINET_SESSION_CLOSED;
        s_close_in_progress = false;
        if (s_local_record_id != 0) {
            (void)tool_record_store_update_door(s_local_record_id,
                                                TOOL_RECORD_DOOR_CLOSED_AFTER_ALERT,
                                                alert_event_id,
                                                occurred_at);
            if (ret != ESP_OK) {
                (void)tool_record_store_update_sync(s_local_record_id,
                                                    TOOL_RECORD_SYNC_FAILED,
                                                    NULL);
            }
        }
    }
    unlock_core();
    return ret;
}

static void cabinet_cloud_sync_task(void *arg)
{
    cabinet_sync_job_t *job = (cabinet_sync_job_t *)arg;
    if (job == NULL) {
        vTaskDelete(NULL);
        return;
    }
    cabinet_sync_mode_t mode = job->mode;
    uint32_t generation = job->generation;
    free(job);

    if (mode == CABINET_SYNC_RECOVERY) {
        (void)post_recovery_event(generation);
        vTaskDelete(NULL);
        return;
    }

    cabinet_cloud_work_t *work = allocate_cloud_work();
    if (work == NULL) {
        ESP_LOGE(TAG,
                 "Cloud sync work allocation failed: bytes=%u internal_free=%u internal_largest=%u",
                 (unsigned)sizeof(*work),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        lock_core();
        if (generation == s_session_generation) {
            s_session_state = mode == CABINET_SYNC_ALERT
                                  ? CABINET_SESSION_ALERTED
                                  : CABINET_SESSION_CLOSED;
            s_close_in_progress = false;
        }
        unlock_core();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Cabinet cloud task started: stack=%u work=%u stack_free=%u",
             (unsigned)CABINET_CLOSE_TASK_STACK, (unsigned)sizeof(*work),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    auth_type_t auth_type;
    bool vision_complete;
    uint32_t seq;

    lock_core();
    if (generation != s_session_generation) {
        unlock_core();
        heap_caps_free(work);
        vTaskDelete(NULL);
        return;
    }
    memcpy(work->before, s_before, sizeof(work->before));
    memcpy(work->after, s_current, sizeof(work->after));
    snprintf(work->operator_name, sizeof(work->operator_name), "%s", s_operator_name);
    snprintf(work->opened_at, sizeof(work->opened_at), "%s", s_opened_at);
    snprintf(work->operation_id, sizeof(work->operation_id), "%s", s_operation_id);
    snprintf(work->snapshot_path, sizeof(work->snapshot_path), "%s", s_inventory_snapshot_path);
    snprintf(work->captured_at, sizeof(work->captured_at), "%s", s_inventory_captured_at);
    snprintf(work->state_source, sizeof(work->state_source), "%s", s_state_source);
    auth_type = s_auth_type;
    vision_complete = s_vision_complete;
    seq = s_event_seq++;
    unlock_core();

    time_t now = time(NULL);
    tool_cabinet_format_time_iso(work->occurred_at, sizeof(work->occurred_at), now);
    tool_cabinet_make_event_id(CONFIG_CLOUD_UPLOADER_DEVICE_CODE, now, seq,
                               work->event_id, sizeof(work->event_id));

    tool_cabinet_diff(work->before, work->after, TOOL_CABINET_SLOT_COUNT, &work->diff);

    esp_err_t image_ret = work->snapshot_path[0] != '\0'
                              ? cloud_uploader_upload_image_file(work->snapshot_path,
                                                                 work->image_url,
                                                                 sizeof(work->image_url))
                              : ESP_ERR_NOT_FOUND;
    if (image_ret == ESP_OK && work->image_url[0] != '\0') {
        cabinet_core_set_latest_image_url(work->image_url);
    }

    snprintf(work->record.event_id, sizeof(work->record.event_id), "%s", work->event_id);
    snprintf(work->record.device_code, sizeof(work->record.device_code), "%s",
             CONFIG_CLOUD_UPLOADER_DEVICE_CODE);
    snprintf(work->record.operator_name, sizeof(work->record.operator_name), "%s",
             work->operator_name);
    snprintf(work->record.result_type, sizeof(work->record.result_type), "%s",
             tool_cabinet_result_type(&work->diff));
    snprintf(work->record.opened_at, sizeof(work->record.opened_at), "%s", work->opened_at);
    snprintf(work->record.closed_at, sizeof(work->record.closed_at), "%s",
             mode == CABINET_SYNC_NORMAL ? work->occurred_at : "");
    snprintf(work->record.snapshot_local_path, sizeof(work->record.snapshot_local_path), "%s",
             work->snapshot_path);
    snprintf(work->record.inspection_image_url, sizeof(work->record.inspection_image_url), "%s",
             work->image_url);
    snprintf(work->record.state_source, sizeof(work->record.state_source), "%s",
             work->state_source);
    work->record.sync_status = TOOL_RECORD_SYNC_PENDING;
    work->record.borrowed_count = work->diff.borrowed_count;
    work->record.returned_count = work->diff.returned_count;
    work->record.anomaly_count = work->diff.anomaly_count;
    snprintf(work->record.operation_id, sizeof(work->record.operation_id), "%s",
             work->operation_id);
    work->record.vision_complete = vision_complete;
    work->record.door_state = mode == CABINET_SYNC_ALERT
                                 ? TOOL_RECORD_DOOR_ALERTED
                                 : TOOL_RECORD_DOOR_NORMAL;
    uint32_t local_record_id = 0;
    (void)tool_record_store_append(&work->record, &local_record_id);
    lock_core();
    if (generation == s_session_generation) {
        s_local_record_id = local_record_id;
    }
    unlock_core();

    const char *event_type = mode == CABINET_SYNC_ALERT ? "door_unconfirmed" : "operation";
    fill_event(&work->event, work->event_id, work->operator_name, auth_type, work->opened_at,
               mode == CABINET_SYNC_NORMAL ? work->occurred_at : "",
               work->image_url, work->snapshot_path, local_record_id, &work->diff,
               work->state_source, event_type, work->operation_id, "", work->occurred_at,
               mode == CABINET_SYNC_NORMAL);
    esp_err_t event_ret = cloud_uploader_post_event(&work->event);

    fill_snapshot(&work->snapshot, work->event_id, work->occurred_at, work->captured_at,
                  work->after, work->state_source, vision_complete);
    esp_err_t snapshot_ret = cloud_uploader_post_snapshot(&work->snapshot);
    bool cloud_ok = event_ret == ESP_OK && snapshot_ret == ESP_OK &&
                    (work->snapshot_path[0] == '\0' || image_ret == ESP_OK);
    if (local_record_id != 0) {
        (void)tool_record_store_update_sync(local_record_id,
                                            cloud_ok ? TOOL_RECORD_SYNC_SYNCED : TOOL_RECORD_SYNC_FAILED,
                                            work->image_url);
    }
    (void)tool_record_store_save_states(work->after, TOOL_CABINET_SLOT_COUNT);

    bool send_recovery = false;
    lock_core();
    if (generation == s_session_generation) {
        if (mode == CABINET_SYNC_ALERT) {
            if (event_ret == ESP_OK) {
                snprintf(s_alert_event_id, sizeof(s_alert_event_id), "%s", work->event_id);
                if (s_local_record_id != 0) {
                    (void)tool_record_store_update_door(s_local_record_id,
                                                        TOOL_RECORD_DOOR_ALERTED,
                                                        work->event_id,
                                                        NULL);
                }
                s_session_state = CABINET_SESSION_ALERTED;
                send_recovery = s_close_requested;
                if (send_recovery) {
                    s_session_state = CABINET_SESSION_RECOVERY_POSTING;
                }
            } else {
                s_session_state = s_close_requested ? CABINET_SESSION_CLOSED : CABINET_SESSION_ALERTED;
                s_close_in_progress = false;
            }
        } else {
            s_session_state = CABINET_SESSION_CLOSED;
            s_close_in_progress = false;
        }
    }
    unlock_core();

    ESP_LOGI(TAG,
             "Cabinet cloud sync mode=%s image=%s event=%s snapshot=%s recovery=%d",
             mode == CABINET_SYNC_ALERT ? "alert" : "normal",
             esp_err_to_name(image_ret), esp_err_to_name(event_ret),
             esp_err_to_name(snapshot_ret), send_recovery);
    if (mode == CABINET_SYNC_NORMAL && ui_manager_get_current_screen() == SCREEN_RESULT) {
        fill_result(&work->result, work->operator_name, work->occurred_at,
                    cloud_ok, &work->diff);
        result_show(&work->result);
    }
    standby_update_tool_count(TOOL_CABINET_SLOT_COUNT,
                              (uint16_t)count_available(work->after));
    heap_caps_free(work);
    if (send_recovery) {
        (void)post_recovery_event(generation);
    }
    vTaskDelete(NULL);
}

static esp_err_t start_cloud_sync(cabinet_sync_mode_t mode, uint32_t generation)
{
    cabinet_sync_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }
    job->mode = mode;
    job->generation = generation;
    BaseType_t task_ret = xTaskCreate(cabinet_cloud_sync_task, "cabinet_cloud",
                                      CABINET_CLOSE_TASK_STACK, job,
                                      CABINET_CLOSE_TASK_PRIO, NULL);
    if (task_ret != pdPASS) {
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void close_grace_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    int64_t deadline_us;
    lock_core();
    deadline_us = generation == s_session_generation ? s_grace_deadline_us : 0;
    unlock_core();
    int64_t remain_us = deadline_us - esp_timer_get_time();
    if (remain_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((remain_us + 999) / 1000)));
    }

    bool should_alert = false;
    lock_core();
    if (generation == s_session_generation &&
        s_session_state == CABINET_SESSION_CLOSE_GRACE) {
        s_session_state = CABINET_SESSION_ALERT_POSTING;
        should_alert = true;
    }
    unlock_core();

    if (should_alert) {
        ESP_LOGW(TAG, "Door close grace expired: operation_id=%s", s_operation_id);
        esp_err_t ret = start_cloud_sync(CABINET_SYNC_ALERT, generation);
        if (ret != ESP_OK) {
            lock_core();
            if (generation == s_session_generation) {
                s_session_state = CABINET_SESSION_ALERTED;
            }
            unlock_core();
        }
    }
    vTaskDelete(NULL);
}

esp_err_t cabinet_core_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    tool_slot_runtime_t persisted[TOOL_CABINET_SLOT_COUNT];
    size_t persisted_count = 0;
    esp_err_t load_ret = tool_record_store_load_states(persisted, TOOL_CABINET_SLOT_COUNT, &persisted_count);
    lock_core();
    if (load_ret == ESP_OK && persisted_count == TOOL_CABINET_SLOT_COUNT) {
        memcpy(s_before, persisted, sizeof(s_before));
        memcpy(s_current, persisted, sizeof(s_current));
    } else {
        tool_cabinet_init_states(s_before, TOOL_CABINET_SLOT_COUNT);
        tool_cabinet_init_states(s_current, TOOL_CABINET_SLOT_COUNT);
        (void)tool_record_store_save_states(s_current, TOOL_CABINET_SLOT_COUNT);
    }
    snprintf(s_operator_name, sizeof(s_operator_name), "%s", CABINET_DEFAULT_OPERATOR);
    time_t now = time(NULL);
    tool_cabinet_format_time_iso(s_opened_at, sizeof(s_opened_at), now);
    s_auth_type = AUTH_TYPE_NONE;
    snprintf(s_state_source, sizeof(s_state_source), "%s", CABINET_STATE_SOURCE);
    s_close_in_progress = false;
    s_session_generation++;
    s_session_state = CABINET_SESSION_IDLE;
    s_grace_deadline_us = 0;
    s_capture_deadline_us = 0;
    s_close_requested = false;
    unlock_core();

    ESP_LOGI(TAG, "Cabinet core initialized");
    return ESP_OK;
}

void cabinet_core_start_operation(const char *operator_name, auth_type_t auth_type)
{
    lock_core();
    memcpy(s_before, s_current, sizeof(s_before));
    snprintf(s_operator_name, sizeof(s_operator_name), "%s",
             operator_name != NULL && operator_name[0] != '\0' ? operator_name : CABINET_DEFAULT_OPERATOR);
    s_auth_type = auth_type;
    snprintf(s_state_source, sizeof(s_state_source), "%s", CABINET_STATE_SOURCE);
    time_t now = time(NULL);
    tool_cabinet_format_time_iso(s_opened_at, sizeof(s_opened_at), now);
    uint32_t seq = s_event_seq++;
    tool_cabinet_make_event_id(CONFIG_CLOUD_UPLOADER_DEVICE_CODE, now, seq,
                               s_operation_id, sizeof(s_operation_id));
    s_alert_event_id[0] = '\0';
    s_latest_image_url[0] = '\0';
    s_inventory_snapshot_path[0] = '\0';
    s_inventory_captured_at[0] = '\0';
    s_local_record_id = 0;
    s_vision_complete = false;
    s_close_requested = false;
    s_close_in_progress = false;
    s_grace_deadline_us = 0;
    /* The 15-second tool window starts only after the IMU confirms fully open. */
    s_capture_deadline_us = 0;
    s_session_generation++;
    s_session_state = CABINET_SESSION_OPEN_WAIT;
    unlock_core();
}

esp_err_t cabinet_core_begin_capture_wait(void)
{
    lock_core();
    if (s_session_state != CABINET_SESSION_OPEN_WAIT || s_capture_deadline_us != 0) {
        unlock_core();
        return ESP_ERR_INVALID_STATE;
    }
    s_capture_deadline_us = esp_timer_get_time() +
                            (int64_t)CABINET_CAPTURE_WAIT_MS * 1000;
    unlock_core();
    ESP_LOGI(TAG, "Tool capture window started: %u ms", (unsigned int)CABINET_CAPTURE_WAIT_MS);
    return ESP_OK;
}

void cabinet_core_abort_operation(void)
{
    lock_core();
    if (s_session_state == CABINET_SESSION_OPEN_WAIT) {
        memcpy(s_current, s_before, sizeof(s_current));
        s_session_generation++;
        s_session_state = CABINET_SESSION_IDLE;
        s_capture_deadline_us = 0;
        s_grace_deadline_us = 0;
        s_close_requested = false;
        s_close_in_progress = false;
    }
    unlock_core();
    ESP_LOGI(TAG, "Open operation aborted; inventory rolled back without cloud sync");
}

uint32_t cabinet_core_get_capture_wait_remaining_ms(void)
{
    lock_core();
    int64_t deadline_us = s_capture_deadline_us;
    cabinet_session_state_t state = s_session_state;
    unlock_core();
    if (state != CABINET_SESSION_OPEN_WAIT) {
        return 0;
    }
    int64_t remain_us = deadline_us - esp_timer_get_time();
    return remain_us > 0 ? (uint32_t)((remain_us + 999) / 1000) : 0;
}

esp_err_t cabinet_core_get_states(tool_slot_runtime_t *states, size_t max_count, size_t *out_count)
{
    if ((states == NULL && max_count > 0) || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = max_count < TOOL_CABINET_SLOT_COUNT ? max_count : TOOL_CABINET_SLOT_COUNT;
    lock_core();
    if (count > 0) {
        memcpy(states, s_current, count * sizeof(states[0]));
    }
    unlock_core();

    *out_count = count;
    return ESP_OK;
}

esp_err_t cabinet_core_set_slot_status(uint8_t slot_no, tool_slot_status_t status)
{
    if (slot_no == 0 || slot_no > TOOL_CABINET_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_core();
    s_current[slot_no - 1].slot_no = slot_no;
    s_current[slot_no - 1].status = status;
    snprintf(s_state_source, sizeof(s_state_source), "%s", CABINET_STATE_SOURCE);
    unlock_core();
    return ESP_OK;
}

esp_err_t cabinet_core_apply_vision_inventory(const tool_detection_event_t *detection)
{
    if (detection == NULL || detection->type != TOOL_DET_EVT_DONE ||
        !detection->quality_ok || detection->frame_width != 1280 ||
        detection->frame_height != 720 || detection->slot_count != TOOL_CABINET_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    tool_slot_runtime_t next[TOOL_CABINET_SLOT_COUNT] = {0};
    bool physical_seen[TOOL_CABINET_SLOT_COUNT] = {false};
    bool cabinet_seen[TOOL_CABINET_SLOT_COUNT] = {false};
    for (size_t i = 0; i < TOOL_CABINET_SLOT_COUNT; ++i) {
        const tool_slot_detection_result_t *slot = &detection->slots[i];
        if (slot->physical_slot_no == 0 || slot->physical_slot_no > TOOL_CABINET_SLOT_COUNT ||
            slot->cabinet_slot_no == 0 || slot->cabinet_slot_no > TOOL_CABINET_SLOT_COUNT ||
            physical_seen[slot->physical_slot_no - 1] || cabinet_seen[slot->cabinet_slot_no - 1] ||
            slot->expected_class_id < 0 || slot->expected_class_id >= TOOL_DETECT_CLASS_COUNT) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        tool_slot_status_t status;
        switch (slot->status) {
        case TOOL_SLOT_DET_PRESENT:
            if (slot->detected_class_id != slot->expected_class_id ||
                !isfinite(slot->confidence) ||
                slot->confidence < CABINET_VISION_MIN_CONFIDENCE ||
                slot->confidence > 1.0f) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            status = TOOL_SLOT_AVAILABLE;
            break;
        case TOOL_SLOT_DET_MISMATCH:
            /* Position-independent inventory cannot produce a mismatch. */
            return ESP_ERR_INVALID_RESPONSE;
        case TOOL_SLOT_DET_MISSING:
            if (slot->detected_class_id != -1 || slot->confidence != 0.0f) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            status = TOOL_SLOT_BORROWED;
            break;
        default:
            return ESP_ERR_INVALID_RESPONSE;
        }

        physical_seen[slot->physical_slot_no - 1] = true;
        cabinet_seen[slot->cabinet_slot_no - 1] = true;
        next[slot->cabinet_slot_no - 1].slot_no = slot->cabinet_slot_no;
        next[slot->cabinet_slot_no - 1].status = status;
    }

    lock_core();
    memcpy(s_current, next, sizeof(s_current));
    snprintf(s_state_source, sizeof(s_state_source), "%s", CABINET_STATE_SOURCE_VISION);
    unlock_core();

    ESP_LOGI(TAG,
             "Vision inventory applied atomically: frame=%ux%u slots=%u detections=%d",
             (unsigned int)detection->frame_width,
             (unsigned int)detection->frame_height,
             (unsigned int)detection->slot_count,
             detection->detection_count);
    return ESP_OK;
}

esp_err_t cabinet_core_begin_close_grace(const char *snapshot_path,
                                         const char *captured_at,
                                         int64_t captured_monotonic_us,
                                         bool vision_complete)
{
    int64_t started_us = captured_monotonic_us > 0 ? captured_monotonic_us : esp_timer_get_time();
    uint32_t generation;
    lock_core();
    if (s_session_state != CABINET_SESSION_OPEN_WAIT &&
        s_session_state != CABINET_SESSION_CLOSE_GRACE) {
        unlock_core();
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_inventory_snapshot_path, sizeof(s_inventory_snapshot_path), "%s",
             snapshot_path != NULL ? snapshot_path : "");
    snprintf(s_inventory_captured_at, sizeof(s_inventory_captured_at), "%s",
             captured_at != NULL ? captured_at : "");
    s_vision_complete = vision_complete;
    s_close_requested = false;
    s_grace_deadline_us = started_us + (int64_t)CABINET_CLOSE_GRACE_MS * 1000;
    s_session_state = CABINET_SESSION_CLOSE_GRACE;
    generation = ++s_session_generation;
    unlock_core();

    BaseType_t task_ret = xTaskCreate(close_grace_task, "door_grace", 4096,
                                      (void *)(uintptr_t)generation, 3, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Create door grace task failed; manual close remains available");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Door close grace started: operation_id=%s vision=%d path=%s",
             s_operation_id, vision_complete,
             snapshot_path != NULL && snapshot_path[0] != '\0' ? snapshot_path : "(none)");
    return ESP_OK;
}

void cabinet_core_cancel_close_grace(void)
{
    lock_core();
    if (s_session_state == CABINET_SESSION_CLOSE_GRACE) {
        s_session_generation++;
        s_session_state = CABINET_SESSION_OPEN_WAIT;
        s_grace_deadline_us = 0;
        s_capture_deadline_us = esp_timer_get_time() +
                                (int64_t)CABINET_CAPTURE_WAIT_MS * 1000;
    }
    unlock_core();
}

uint32_t cabinet_core_get_close_grace_remaining_ms(bool *alerted_out)
{
    int64_t deadline_us;
    cabinet_session_state_t state;
    lock_core();
    deadline_us = s_grace_deadline_us;
    state = s_session_state;
    unlock_core();

    if (alerted_out != NULL) {
        *alerted_out = state == CABINET_SESSION_ALERT_POSTING ||
                       state == CABINET_SESSION_ALERTED ||
                       state == CABINET_SESSION_RECOVERY_POSTING;
    }
    if (state != CABINET_SESSION_CLOSE_GRACE) {
        return 0;
    }
    int64_t remain_us = deadline_us - esp_timer_get_time();
    return remain_us > 0 ? (uint32_t)((remain_us + 999) / 1000) : 0;
}

void cabinet_core_set_latest_image_url(const char *image_url)
{
    lock_core();
    snprintf(s_latest_image_url, sizeof(s_latest_image_url), "%s",
             image_url != NULL ? image_url : "");
    unlock_core();
    ESP_LOGI(TAG, "Latest device image url updated: %s",
             image_url != NULL && image_url[0] != '\0' ? image_url : "(empty)");
}

esp_err_t cabinet_core_confirm_close_async(void)
{
    cabinet_sync_mode_t sync_mode = CABINET_SYNC_NORMAL;
    bool start_sync = false;
    uint32_t generation;
    cabinet_session_state_t previous_state;

    lock_core();
    if (s_close_in_progress || s_session_state == CABINET_SESSION_CLOSED ||
        s_session_state == CABINET_SESSION_IDLE ||
        s_session_state == CABINET_SESSION_OPEN_WAIT) {
        unlock_core();
        return ESP_ERR_INVALID_STATE;
    }
    generation = s_session_generation;
    previous_state = s_session_state;
    s_close_requested = true;
    s_close_in_progress = true;
    if (s_session_state == CABINET_SESSION_CLOSE_GRACE) {
        s_session_state = CABINET_SESSION_NORMAL_POSTING;
        sync_mode = CABINET_SYNC_NORMAL;
        start_sync = true;
    } else if (s_session_state == CABINET_SESSION_ALERTED) {
        if (s_alert_event_id[0] == '\0') {
            s_session_state = CABINET_SESSION_CLOSED;
            start_sync = false;
        } else {
            s_session_state = CABINET_SESSION_RECOVERY_POSTING;
            sync_mode = CABINET_SYNC_RECOVERY;
            start_sync = true;
        }
    } else if (s_session_state != CABINET_SESSION_ALERT_POSTING) {
        s_close_in_progress = false;
        unlock_core();
        return ESP_ERR_INVALID_STATE;
    }
    unlock_core();

    esp_err_t signal_ret = cabinet_door_signal_close_pulse();
    if (signal_ret != ESP_OK) {
        ESP_LOGE(TAG, "Start two-second door close pulse failed: %s", esp_err_to_name(signal_ret));
        lock_core();
        s_close_in_progress = false;
        s_close_requested = false;
        s_session_state = previous_state;
        unlock_core();
        return signal_ret;
    }

    tool_slot_runtime_t before[TOOL_CABINET_SLOT_COUNT];
    tool_slot_runtime_t after[TOOL_CABINET_SLOT_COUNT];
    char operator_name[UI_OPERATOR_NAME_MAX_LEN];
    lock_core();
    memcpy(before, s_before, sizeof(before));
    memcpy(after, s_current, sizeof(after));
    snprintf(operator_name, sizeof(operator_name), "%s", s_operator_name);
    unlock_core();
    tool_operation_diff_t diff;
    tool_cabinet_diff(before, after, TOOL_CABINET_SLOT_COUNT, &diff);
    char closed_at[32];
    tool_cabinet_format_time_iso(closed_at, sizeof(closed_at), time(NULL));
    operation_result_t result;
    fill_result(&result, operator_name, closed_at, false, &diff);
    result_show(&result);
    ui_manager_goto_screen(SCREEN_RESULT);

    if (start_sync) {
        esp_err_t ret = start_cloud_sync(sync_mode, generation);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Door is physically closed but cloud sync task could not start: %s",
                 esp_err_to_name(ret));
        lock_core();
        s_close_in_progress = false;
        s_close_requested = false;
        s_session_state = CABINET_SESSION_CLOSED;
        unlock_core();
        return ret;
    }
    if (previous_state == CABINET_SESSION_ALERTED && !start_sync) {
        lock_core();
        s_close_in_progress = false;
        unlock_core();
    }
    /* Either alert upload is already in progress and will send recovery, or
     * the alert failed and only the physical close can be completed. */
    return ESP_OK;
}

esp_err_t cabinet_core_reset_all(void)
{
    esp_err_t ret = tool_record_store_clear_all();

    lock_core();
    tool_cabinet_init_states(s_before, TOOL_CABINET_SLOT_COUNT);
    tool_cabinet_init_states(s_current, TOOL_CABINET_SLOT_COUNT);
    snprintf(s_state_source, sizeof(s_state_source), "%s", CABINET_STATE_SOURCE);
    time_t now = time(NULL);
    tool_cabinet_format_time_iso(s_opened_at, sizeof(s_opened_at), now);
    s_close_in_progress = false;
    s_session_generation++;
    s_session_state = CABINET_SESSION_IDLE;
    s_close_requested = false;
    s_local_record_id = 0;
    s_capture_deadline_us = 0;
    s_grace_deadline_us = 0;
    unlock_core();

    esp_err_t state_ret = tool_record_store_save_states(s_current, TOOL_CABINET_SLOT_COUNT);
    if (ret == ESP_OK && state_ret != ESP_OK) {
        ret = state_ret;
    }
    return ret;
}

void cabinet_core_get_tool_counts(uint16_t *total_out, uint16_t *available_out)
{
    if (total_out != NULL) {
        *total_out = TOOL_CABINET_SLOT_COUNT;
    }
    if (available_out == NULL) {
        return;
    }

    lock_core();
    *available_out = (uint16_t)count_available(s_current);
    unlock_core();
}

const char *cabinet_core_get_operator_name(void)
{
    return s_operator_name;
}

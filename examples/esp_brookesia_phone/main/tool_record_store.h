#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tool_cabinet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_RECORD_STORE_MAX_RECORDS 100
#define TOOL_RECORD_EVENT_ID_LEN 64
#define TOOL_RECORD_DEVICE_CODE_LEN 32
#define TOOL_RECORD_OPERATOR_LEN 64
#define TOOL_RECORD_TIME_LEN 32
#define TOOL_RECORD_PATH_LEN 128
#define TOOL_RECORD_URL_LEN 192
#define TOOL_RECORD_RESULT_LEN 24

typedef enum {
    TOOL_RECORD_SYNC_PENDING = 0,
    TOOL_RECORD_SYNC_SYNCED,
    TOOL_RECORD_SYNC_FAILED,
} tool_record_sync_t;

typedef enum {
    TOOL_RECORD_DOOR_NORMAL = 0,
    TOOL_RECORD_DOOR_ALERTED,
    TOOL_RECORD_DOOR_CLOSED_AFTER_ALERT,
} tool_record_door_state_t;

typedef struct {
    uint32_t record_id;
    char event_id[TOOL_RECORD_EVENT_ID_LEN];
    char device_code[TOOL_RECORD_DEVICE_CODE_LEN];
    char operator_name[TOOL_RECORD_OPERATOR_LEN];
    char result_type[TOOL_RECORD_RESULT_LEN];
    char opened_at[TOOL_RECORD_TIME_LEN];
    char closed_at[TOOL_RECORD_TIME_LEN];
    tool_record_sync_t sync_status;
    char snapshot_local_path[TOOL_RECORD_PATH_LEN];
    char inspection_image_url[TOOL_RECORD_URL_LEN];
    char state_source[16];
    uint8_t borrowed_count;
    uint8_t returned_count;
    uint8_t anomaly_count;
    char operation_id[TOOL_RECORD_EVENT_ID_LEN];
    char alert_event_id[TOOL_RECORD_EVENT_ID_LEN];
    bool vision_complete;
    tool_record_door_state_t door_state;
} tool_record_t;

esp_err_t tool_record_store_init(void);
esp_err_t tool_record_store_append(const tool_record_t *record, uint32_t *record_id_out);
esp_err_t tool_record_store_update_sync(uint32_t record_id,
                                        tool_record_sync_t sync,
                                        const char *inspection_image_url);
esp_err_t tool_record_store_update_door(uint32_t record_id,
                                        tool_record_door_state_t door_state,
                                        const char *alert_event_id,
                                        const char *closed_at);
esp_err_t tool_record_store_list_latest(tool_record_t *records,
                                        size_t max_count,
                                        size_t *out_count);
esp_err_t tool_record_store_clear_all(void);
esp_err_t tool_record_store_save_states(const tool_slot_runtime_t *states, size_t count);
esp_err_t tool_record_store_load_states(tool_slot_runtime_t *states,
                                        size_t max_count,
                                        size_t *out_count);
const char *tool_record_sync_label(tool_record_sync_t sync);

#ifdef __cplusplus
}
#endif

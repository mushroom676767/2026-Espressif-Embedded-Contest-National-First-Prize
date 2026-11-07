#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tool_cabinet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_UPLOADER_MAX_SLOTS TOOL_CABINET_SLOT_COUNT
#define CLOUD_UPLOADER_MAX_CHANGES TOOL_CABINET_SLOT_COUNT
#define CLOUD_UPLOADER_EVENT_TYPE_LEN 32

typedef struct {
    uint8_t slot_no;
    char tool_code[32];
    char tool_name[32];
    char tool_class[32];
    char status[16];
    char image_url[192];
} cloud_slot_t;

typedef struct {
    char device_code[32];
    char timestamp[32];
    char firmware_version[24];
    int total;
    int available;
    char snapshot_id[64];
    char captured_at[32];
    char state_source[16];
    int screwdriver_present;
    bool vision_complete;
    cloud_slot_t slots[CLOUD_UPLOADER_MAX_SLOTS];
    int slot_count;
} cloud_snapshot_t;

typedef struct {
    uint8_t slot_no;
    char tool_code[32];
    char tool_name[32];
    char tool_class[32];
    int quantity;
} cloud_tool_change_t;

typedef struct {
    char event_id[64];
    char device_code[32];
    struct {
        char user_code[32];
        char name[64];
        char auth_type[16];
    } operator_info;
    char event_type[CLOUD_UPLOADER_EVENT_TYPE_LEN];
    char operation_id[64];
    char related_event_id[64];
    char occurred_at[32];
    bool door_confirmed;
    int reminder_count;
    char result_type[192];
    char opened_at[32];
    char closed_at[32];
    cloud_tool_change_t borrowed[CLOUD_UPLOADER_MAX_CHANGES];
    int borrowed_count;
    cloud_tool_change_t returned[CLOUD_UPLOADER_MAX_CHANGES];
    int returned_count;
    cloud_tool_change_t anomalies[CLOUD_UPLOADER_MAX_CHANGES];
    int anomaly_count;
    uint32_t local_record_id;
    char state_source[16];
    char inspection_image_url[192];
    char snapshot_local_path[128];
    char captured_at[32];
} cloud_event_t;

typedef struct {
    char device_code[32];
    char snapshot_id[64];
    char captured_at[32];
    char state_source[16];
    char local_path[128];
    char tool_code[32];
    char tool_name[64];
    char tool_class[32];
} cloud_inspection_image_t;

esp_err_t cloud_uploader_init(void);
bool cloud_uploader_is_ready(void);
esp_err_t cloud_uploader_upload_inspection_image(const cloud_inspection_image_t *image,
                                                 char *image_url_out,
                                                 size_t image_url_out_size);
esp_err_t cloud_uploader_upload_image_file(const char *local_path,
                                           char *image_url_out,
                                           size_t image_url_out_size);
esp_err_t cloud_uploader_post_event(const cloud_event_t *event);
esp_err_t cloud_uploader_post_snapshot(const cloud_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

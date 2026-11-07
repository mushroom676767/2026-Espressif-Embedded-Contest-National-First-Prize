#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "ui_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_CABINET_SLOT_COUNT 9
#define TOOL_CABINET_MAX_CHANGES TOOL_CABINET_SLOT_COUNT

typedef struct {
    uint8_t slot_no;
    const char *tool_code;
    const char *tool_name;
    const char *tool_class;
} tool_catalog_entry_t;

typedef struct {
    uint8_t slot_no;
    tool_slot_status_t status;
} tool_slot_runtime_t;

typedef struct {
    uint8_t slot_no;
    char tool_code[32];
    char tool_name[32];
    char tool_class[32];
    tool_slot_status_t from_status;
    tool_slot_status_t to_status;
} tool_change_t;

typedef struct {
    tool_change_t borrowed[TOOL_CABINET_MAX_CHANGES];
    uint8_t borrowed_count;
    tool_change_t returned[TOOL_CABINET_MAX_CHANGES];
    uint8_t returned_count;
    tool_change_t anomalies[TOOL_CABINET_MAX_CHANGES];
    uint8_t anomaly_count;
    bool changed;
} tool_operation_diff_t;

const tool_catalog_entry_t *tool_cabinet_catalog(void);
size_t tool_cabinet_catalog_count(void);
const tool_catalog_entry_t *tool_cabinet_get_slot(uint8_t slot_no);
void tool_cabinet_init_states(tool_slot_runtime_t *states, size_t count);
tool_slot_status_t tool_cabinet_next_touch_status(tool_slot_status_t status);
const char *tool_cabinet_status_to_backend(tool_slot_status_t status);
const char *tool_cabinet_status_label(tool_slot_status_t status);
void tool_cabinet_diff(const tool_slot_runtime_t *before,
                       const tool_slot_runtime_t *after,
                       size_t count,
                       tool_operation_diff_t *diff);
const char *tool_cabinet_result_type(const tool_operation_diff_t *diff);
void tool_cabinet_format_time_iso(char *out, size_t out_size, time_t now);
void tool_cabinet_make_event_id(const char *device_code,
                                time_t now,
                                uint32_t seq,
                                char *out,
                                size_t out_size);

#ifdef __cplusplus
}
#endif

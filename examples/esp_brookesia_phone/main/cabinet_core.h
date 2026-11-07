#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tool_cabinet.h"
#include "tool_detection_service.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cabinet_core_init(void);
void cabinet_core_start_operation(const char *operator_name, auth_type_t auth_type);
esp_err_t cabinet_core_begin_capture_wait(void);
void cabinet_core_abort_operation(void);
uint32_t cabinet_core_get_capture_wait_remaining_ms(void);
esp_err_t cabinet_core_get_states(tool_slot_runtime_t *states, size_t max_count, size_t *out_count);
esp_err_t cabinet_core_set_slot_status(uint8_t slot_no, tool_slot_status_t status);
esp_err_t cabinet_core_apply_vision_inventory(const tool_detection_event_t *detection);
esp_err_t cabinet_core_begin_close_grace(const char *snapshot_path,
                                         const char *captured_at,
                                         int64_t captured_monotonic_us,
                                         bool vision_complete);
void cabinet_core_cancel_close_grace(void);
uint32_t cabinet_core_get_close_grace_remaining_ms(bool *alerted_out);
void cabinet_core_set_latest_image_url(const char *image_url);
esp_err_t cabinet_core_confirm_close_async(void);
esp_err_t cabinet_core_reset_all(void);
void cabinet_core_get_tool_counts(uint16_t *total_out, uint16_t *available_out);
const char *cabinet_core_get_operator_name(void);

#ifdef __cplusplus
}
#endif

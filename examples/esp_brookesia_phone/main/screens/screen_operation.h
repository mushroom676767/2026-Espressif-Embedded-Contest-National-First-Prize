/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_OPERATION_H
#define SCREEN_OPERATION_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_operation_data_s;

void screen_operation_create(void);
lv_obj_t *screen_operation_get_screen(void);
void screen_operation_init(tool_info_t *tools, uint8_t count);
void screen_operation_update_slot(uint8_t slot_id, tool_slot_status_t status);
void screen_operation_set_operator(const char *name);
void screen_operation_set_read_only(bool read_only);
struct screen_operation_data_s *screen_operation_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_OPERATION_H

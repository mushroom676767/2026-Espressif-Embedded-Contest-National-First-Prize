/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_RESULT_H
#define SCREEN_RESULT_H

#include <stdint.h>
#include "lvgl.h"
#include "ui_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_result_data_s;

void screen_result_create(void);
lv_obj_t *screen_result_get_screen(void);
void screen_result_show(operation_result_t *result);
void screen_result_clear(void);
struct screen_result_data_s *screen_result_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_RESULT_H
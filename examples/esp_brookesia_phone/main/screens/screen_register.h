/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_REGISTER_H
#define SCREEN_REGISTER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_register_data_s;

void screen_register_create(void);
lv_obj_t *screen_register_get_screen(void);
void screen_register_start(void);
void screen_register_cancel_wait(void);
struct screen_register_data_s *screen_register_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_REGISTER_H

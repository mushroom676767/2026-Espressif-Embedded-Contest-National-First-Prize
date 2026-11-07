/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_AUTH_H
#define SCREEN_AUTH_H

#include <stdint.h>
#include "lvgl.h"
#include "ui_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_auth_data_s;

void screen_auth_create(void);
lv_obj_t *screen_auth_get_screen(void);
void screen_auth_set_step(uint8_t step, auth_step_state_t state, const char *info);
void screen_auth_reset(void);
void screen_auth_set_operator(const char *name);
void screen_auth_start_wait(auth_type_t type);
void screen_auth_cancel_wait(void);
struct screen_auth_data_s *screen_auth_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_AUTH_H

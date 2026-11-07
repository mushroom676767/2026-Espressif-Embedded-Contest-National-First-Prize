/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_ADMIN_H
#define SCREEN_ADMIN_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void screen_admin_create(void);
lv_obj_t *screen_admin_get_screen(void);
void screen_admin_start(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_ADMIN_H

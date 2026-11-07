/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_WIFI_H
#define SCREEN_WIFI_H

#include "lvgl.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_wifi_data_s;

void screen_wifi_create(void);
lv_obj_t *screen_wifi_get_screen(void);
void screen_wifi_handle_wifi_event(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info);
struct screen_wifi_data_s *screen_wifi_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_WIFI_H

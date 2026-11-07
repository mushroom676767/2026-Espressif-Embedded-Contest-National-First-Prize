/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SCREEN_STANDBY_H
#define SCREEN_STANDBY_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_standby_data_s;

void screen_standby_create(void);
lv_obj_t *screen_standby_get_screen(void);
void screen_standby_update_count(uint16_t total, uint16_t available);
void screen_standby_set_online(bool online);
void screen_standby_set_time_synced(bool synced);
void screen_standby_trigger_pulse(void);
struct screen_standby_data_s *screen_standby_get_data(void);

/**
 * @brief Register button callbacks for the standby screen
 * These are called by ui_manager wrappers.
 */
void screen_standby_set_nfc_callback(void (*cb)(void));
void screen_standby_set_face_callback(void (*cb)(void));
void screen_standby_set_register_callback(void (*cb)(void));
void screen_standby_set_admin_callback(void (*cb)(void));
void screen_standby_set_settings_callback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif // SCREEN_STANDBY_H

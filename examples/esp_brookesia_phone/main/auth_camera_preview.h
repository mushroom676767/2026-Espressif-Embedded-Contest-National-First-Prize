/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUTH_CAMERA_PREVIEW_H
#define AUTH_CAMERA_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera/app_video.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*auth_camera_preview_frame_cb_t)(const uint8_t *data, uint32_t width,
                                               uint32_t height, size_t len,
                                               void *user_ctx);

esp_err_t auth_camera_preview_start(lv_obj_t *canvas, lv_obj_t *status_label);
esp_err_t auth_camera_preview_start_with_format(lv_obj_t *canvas, lv_obj_t *status_label,
                                                video_fmt_t format);
void auth_camera_preview_stop(void);
void auth_camera_preview_stop_and_release_idle_resources(void);
void auth_camera_preview_release_idle_resources(void);
bool auth_camera_preview_is_running(void);
void auth_camera_preview_set_frame_callback(auth_camera_preview_frame_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif // AUTH_CAMERA_PREVIEW_H

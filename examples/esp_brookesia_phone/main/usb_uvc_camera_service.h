#ifndef USB_UVC_CAMERA_SERVICE_H
#define USB_UVC_CAMERA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usb_uvc_camera_frame_cb_t)(const uint8_t *rgb565,
                                          uint32_t width,
                                          uint32_t height,
                                          void *user_ctx);

esp_err_t usb_uvc_camera_start_preview(usb_uvc_camera_frame_cb_t cb, void *user_ctx);
void usb_uvc_camera_stop_preview(void);
bool usb_uvc_camera_preview_active(void);
esp_err_t usb_uvc_camera_request_jpeg_snapshot(const char *path, uint32_t timeout_ms);

esp_err_t usb_uvc_camera_capture_rgb565(uint8_t **out_buf,
                                        size_t *out_len,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint32_t timeout_ms);

/**
 * Capture one UVC MJPEG frame, save that exact compressed frame, and decode
 * the same frame to caller-owned RGB565LE memory.
 */
esp_err_t usb_uvc_camera_capture_rgb565_with_jpeg(uint8_t **out_buf,
                                                  size_t *out_len,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  const char *jpeg_path,
                                                  uint32_t timeout_ms);

/**
 * Capture one stabilized UVC MJPEG frame, save that exact compressed frame,
 * and decode the same frame to caller-owned RGB888 memory.
 */
esp_err_t usb_uvc_camera_capture_rgb888_with_jpeg(uint8_t **out_buf,
                                                  size_t *out_len,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  const char *jpeg_path,
                                                  uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // USB_UVC_CAMERA_SERVICE_H

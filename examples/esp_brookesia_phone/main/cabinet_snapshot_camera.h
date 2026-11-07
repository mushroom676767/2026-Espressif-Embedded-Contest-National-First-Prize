#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cabinet_snapshot_camera_init(void);
esp_err_t cabinet_snapshot_camera_capture(const char *event_id,
                                          char *path_out,
                                          size_t path_out_size);

#ifdef __cplusplus
}
#endif

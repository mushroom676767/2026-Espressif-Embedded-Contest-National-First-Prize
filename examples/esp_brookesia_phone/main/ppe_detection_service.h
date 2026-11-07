#ifndef PPE_DETECTION_SERVICE_H
#define PPE_DETECTION_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PPE_EVT_STARTED = 0,
    PPE_EVT_FRAME_READY,
    PPE_EVT_PASS,
    PPE_EVT_MISSING_HELMET,
    PPE_EVT_MISSING_GOGGLES,
    PPE_EVT_MISSING_BOTH,
    PPE_EVT_ERROR,
} ppe_detection_event_type_t;

typedef struct {
    ppe_detection_event_type_t type;
    bool helmet_present;
    bool goggles_present;
    float helmet_score;
    float goggles_score;
    char message[96];
} ppe_detection_event_t;

typedef void (*ppe_detection_event_cb_t)(const ppe_detection_event_t *event, void *user_ctx);

esp_err_t ppe_detection_start(ppe_detection_event_cb_t cb, void *user_ctx,
                              lv_obj_t *preview_canvas);
void ppe_detection_cancel(void);
bool ppe_detection_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // PPE_DETECTION_SERVICE_H

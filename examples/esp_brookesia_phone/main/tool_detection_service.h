#ifndef TOOL_DETECTION_SERVICE_H
#define TOOL_DETECTION_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_DETECT_MAX_DETECTIONS 32
#define TOOL_DETECT_CLASS_COUNT   8
#define TOOL_DETECT_SLOT_COUNT    9
#define TOOL_DETECT_SNAPSHOT_PATH_MAX 128

typedef enum {
    TOOL_DET_EVT_STARTED = 0,
    TOOL_DET_EVT_FRAME_READY,
    TOOL_DET_EVT_DONE,
    TOOL_DET_EVT_ERROR,
} tool_detection_event_type_t;

typedef struct {
    int category;
    float score;
    int box[4];
} tool_detection_item_t;

typedef enum {
    TOOL_SLOT_DET_PRESENT = 0,
    TOOL_SLOT_DET_MISMATCH,
    TOOL_SLOT_DET_MISSING,
} tool_slot_detection_status_t;

typedef struct {
    uint8_t physical_slot_no;
    uint8_t cabinet_slot_no;
    int expected_class_id;
    int detected_class_id;
    float confidence;
    float iou;
    tool_slot_detection_status_t status;
} tool_slot_detection_result_t;

typedef struct {
    tool_detection_event_type_t type;
    int detection_count;
    tool_detection_item_t detections[TOOL_DETECT_MAX_DETECTIONS];
    int class_counts[TOOL_DETECT_CLASS_COUNT];
    uint32_t frame_width;
    uint32_t frame_height;
    uint8_t slot_count;
    bool quality_ok;
    tool_slot_detection_result_t slots[TOOL_DETECT_SLOT_COUNT];
    char snapshot_local_path[TOOL_DETECT_SNAPSHOT_PATH_MAX];
    char captured_at[32];
    int64_t captured_monotonic_us;
    char message[96];
} tool_detection_event_t;

typedef void (*tool_detection_event_cb_t)(const tool_detection_event_t *event, void *user_ctx);

esp_err_t tool_detection_start(tool_detection_event_cb_t cb, void *user_ctx);
esp_err_t tool_detection_start_with_snapshot(tool_detection_event_cb_t cb,
                                             void *user_ctx,
                                             const char *snapshot_path);
void tool_detection_cancel(void);
bool tool_detection_is_active(void);

esp_err_t tool_detection_run_sync(tool_detection_event_t *result);
esp_err_t tool_detection_run_sync_with_snapshot(const char *snapshot_path,
                                                tool_detection_event_t *result);

#ifdef __cplusplus
}
#endif

#endif // TOOL_DETECTION_SERVICE_H

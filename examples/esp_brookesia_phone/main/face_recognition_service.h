#ifndef FACE_RECOGNITION_SERVICE_H
#define FACE_RECOGNITION_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_REC_NAME_MAX_LEN      64
#define FACE_REC_EMPLOYEE_ID_MAX_LEN 25
#define FACE_REC_MESSAGE_MAX_LEN   96

typedef enum {
    FACE_REC_EVT_PROGRESS = 0,
    FACE_REC_EVT_ENROLL_PROGRESS,
    FACE_REC_EVT_ENROLL_DONE,
    FACE_REC_EVT_AUTH_MATCH,
    FACE_REC_EVT_NO_FACE,
    FACE_REC_EVT_MULTI_FACE,
    FACE_REC_EVT_NO_USER,
    FACE_REC_EVT_ERROR,
} face_recognition_event_type_t;

typedef struct {
    face_recognition_event_type_t type;
    char employee_id[FACE_REC_EMPLOYEE_ID_MAX_LEN];
    char name[FACE_REC_NAME_MAX_LEN];
    char message[FACE_REC_MESSAGE_MAX_LEN];
    uint16_t feature_id;
    uint8_t sample_count;
    float similarity;
} face_recognition_event_t;

typedef void (*face_recognition_event_cb_t)(const face_recognition_event_t *event, void *user_ctx);

esp_err_t face_recognition_service_init(void);
esp_err_t face_recognition_start_enroll(const char *employee_id, const char *name,
                                        face_recognition_event_cb_t cb, void *user_ctx);
esp_err_t face_recognition_start_auth(face_recognition_event_cb_t cb, void *user_ctx);
esp_err_t face_recognition_delete_features(const uint16_t *feature_ids, uint8_t feature_count);
esp_err_t face_recognition_delete_user_data(const char *employee_id,
                                            const char *name,
                                            const uint16_t *feature_ids,
                                            uint8_t feature_count);
void face_recognition_cancel(void);
bool face_recognition_is_active(void);
esp_err_t face_recognition_release_idle_resources(void);

#ifdef __cplusplus
}
#endif

#endif // FACE_RECOGNITION_SERVICE_H

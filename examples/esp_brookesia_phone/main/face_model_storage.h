#ifndef FACE_MODEL_STORAGE_H
#define FACE_MODEL_STORAGE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t face_model_storage_init(void);
bool face_model_storage_is_ready(void);
const char *face_model_storage_model_dir(void);
const char *face_model_storage_db_path(void);
const char *face_model_storage_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // FACE_MODEL_STORAGE_H

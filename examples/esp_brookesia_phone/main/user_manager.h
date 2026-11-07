/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USER_MANAGER_MAX_USERS          20
#define USER_EMPLOYEE_ID_MAX_LEN        25
#define USER_NAME_MAX_LEN               64
#define USER_CARD_HEX_MAX_LEN           27
#define USER_FACE_FEATURE_MAX_COUNT     3
#define USER_FACE_PHOTO_PATH_MAX_LEN    96
#define USER_DISPLAY_NAME_MAX_LEN       (USER_EMPLOYEE_ID_MAX_LEN + USER_NAME_MAX_LEN + 4)

typedef struct {
    char employee_id[USER_EMPLOYEE_ID_MAX_LEN];
    char name[USER_NAME_MAX_LEN];
    char nfc_card_hex[USER_CARD_HEX_MAX_LEN];
    uint16_t face_feature_ids[USER_FACE_FEATURE_MAX_COUNT];
    uint8_t face_feature_count;
    char face_photo_path[USER_FACE_PHOTO_PATH_MAX_LEN];
} user_record_t;

esp_err_t user_manager_init(void);
esp_err_t user_manager_upsert_identity(const char *employee_id, const char *name);
esp_err_t user_manager_set_face(const char *employee_id, const uint16_t *feature_ids,
                                uint8_t feature_count, const char *photo_path);
esp_err_t user_manager_set_nfc(const char *employee_id, const char *card_hex);
esp_err_t user_manager_list(user_record_t *records, size_t max_records, size_t *out_count);
esp_err_t user_manager_find_by_employee_id(const char *employee_id, user_record_t *record);
esp_err_t user_manager_find_by_face_feature(uint16_t feature_id, user_record_t *record);
esp_err_t user_manager_find_by_nfc_card(const char *card_hex, user_record_t *record);
esp_err_t user_manager_delete_user(const char *employee_id);
void user_manager_format_display_name(const user_record_t *record, char *buf, size_t buf_size);
void user_manager_format_identity(const char *employee_id, const char *name, char *buf, size_t buf_size);
bool user_manager_is_valid_employee_id(const char *employee_id);

#ifdef __cplusplus
}
#endif

#endif // USER_MANAGER_H

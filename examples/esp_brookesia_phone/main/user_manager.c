/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "user_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "face_recognition_service.h"
#include "nfc_manager.h"
#include "nvs.h"

static const char *TAG = "user_manager";

#define USER_NVS_NAMESPACE          "users"
#define USER_NVS_COUNT_KEY          "count"
#define USER_NVS_MIGRATED_KEY       "migrated"
#define USER_LEGACY_FACE_NAMESPACE  "face_users"
#define USER_LEGACY_NFC_NAMESPACE   "nfc_users"
#define USER_LEGACY_FACE_SCAN_MAX   200

static void build_key(char *key, size_t key_size, const char *prefix, uint8_t index)
{
    snprintf(key, key_size, "%s_%u", prefix, index);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t len = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (len < dst_size - 1 && src[len] != '\0') {
        len++;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static esp_err_t get_count(nvs_handle_t handle, uint8_t *count)
{
    esp_err_t ret = nvs_get_u8(handle, USER_NVS_COUNT_KEY, count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0;
        return ESP_OK;
    }
    if (ret == ESP_OK && *count > USER_MANAGER_MAX_USERS) {
        *count = USER_MANAGER_MAX_USERS;
    }
    return ret;
}

static esp_err_t get_str_or_empty(nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t required = value_size;
    esp_err_t ret = nvs_get_str(handle, key, value, &required);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        if (value != NULL && value_size > 0) {
            value[0] = '\0';
        }
        return ESP_OK;
    }
    return ret;
}

static esp_err_t load_slot(nvs_handle_t handle, uint8_t index, user_record_t *record)
{
    char key[16];
    uint8_t face_count = 0;
    size_t blob_size = sizeof(record->face_feature_ids);

    memset(record, 0, sizeof(*record));

    build_key(key, sizeof(key), "emp", index);
    esp_err_t ret = get_str_or_empty(handle, key, record->employee_id, sizeof(record->employee_id));
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "name", index);
    ret = get_str_or_empty(handle, key, record->name, sizeof(record->name));
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "card", index);
    ret = get_str_or_empty(handle, key, record->nfc_card_hex, sizeof(record->nfc_card_hex));
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "photo", index);
    ret = get_str_or_empty(handle, key, record->face_photo_path, sizeof(record->face_photo_path));
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "fcnt", index);
    ret = nvs_get_u8(handle, key, &face_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        face_count = 0;
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "faces", index);
    ret = nvs_get_blob(handle, key, record->face_feature_ids, &blob_size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(record->face_feature_ids, 0, sizeof(record->face_feature_ids));
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    record->face_feature_count = face_count;
    if (record->face_feature_count > USER_FACE_FEATURE_MAX_COUNT) {
        record->face_feature_count = USER_FACE_FEATURE_MAX_COUNT;
    }
    return ESP_OK;
}

static esp_err_t save_slot(nvs_handle_t handle, uint8_t index, const user_record_t *record)
{
    char key[16];
    uint8_t face_count = record->face_feature_count;

    if (face_count > USER_FACE_FEATURE_MAX_COUNT) {
        face_count = USER_FACE_FEATURE_MAX_COUNT;
    }

    build_key(key, sizeof(key), "emp", index);
    esp_err_t ret = nvs_set_str(handle, key, record->employee_id);
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "name", index);
    ret = nvs_set_str(handle, key, record->name);
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "card", index);
    ret = nvs_set_str(handle, key, record->nfc_card_hex);
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "photo", index);
    ret = nvs_set_str(handle, key, record->face_photo_path);
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "fcnt", index);
    ret = nvs_set_u8(handle, key, face_count);
    if (ret != ESP_OK) {
        return ret;
    }

    build_key(key, sizeof(key), "faces", index);
    return nvs_set_blob(handle, key, record->face_feature_ids, sizeof(record->face_feature_ids));
}

static esp_err_t erase_slot(nvs_handle_t handle, uint8_t index)
{
    static const char *prefixes[] = {"emp", "name", "card", "photo", "fcnt", "faces"};
    char key[16];

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        build_key(key, sizeof(key), prefixes[i], index);
        esp_err_t ret = nvs_erase_key(handle, key);
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t find_slot_by_employee(nvs_handle_t handle, const char *employee_id,
                                       uint8_t *slot, user_record_t *record)
{
    uint8_t count = 0;
    esp_err_t ret = get_count(handle, &count);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        user_record_t current;
        ret = load_slot(handle, i, &current);
        if (ret != ESP_OK) {
            return ret;
        }
        if (strcmp(current.employee_id, employee_id) == 0) {
            if (slot != NULL) {
                *slot = i;
            }
            if (record != NULL) {
                *record = current;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t find_or_create_legacy_user(nvs_handle_t handle, const char *name, user_record_t *record)
{
    uint8_t count = 0;
    esp_err_t ret = get_count(handle, &count);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        user_record_t current;
        ret = load_slot(handle, i, &current);
        if (ret != ESP_OK) {
            return ret;
        }
        if (strcmp(current.name, name) == 0) {
            *record = current;
            return ESP_OK;
        }
    }

    if (count >= USER_MANAGER_MAX_USERS) {
        return ESP_ERR_NO_MEM;
    }

    memset(record, 0, sizeof(*record));
    snprintf(record->employee_id, sizeof(record->employee_id), "LEGACY%03u", count + 1);
    copy_text(record->name, sizeof(record->name), name);
    ret = save_slot(handle, count, record);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, USER_NVS_COUNT_KEY, count + 1);
    }
    return ret;
}

static esp_err_t migrate_legacy_nfc(nvs_handle_t handle)
{
    nvs_handle_t legacy;
    esp_err_t ret = nvs_open(USER_LEGACY_NFC_NAMESPACE, NVS_READONLY, &legacy);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = nvs_get_u8(legacy, "nfc_count", &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(legacy);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        nvs_close(legacy);
        return ret;
    }
    if (count > USER_MANAGER_MAX_USERS) {
        count = USER_MANAGER_MAX_USERS;
    }

    for (uint8_t i = 0; i < count; i++) {
        char card_key[16];
        char name_key[16];
        char card[USER_CARD_HEX_MAX_LEN] = {};
        char name[USER_NAME_MAX_LEN] = {};
        size_t card_size = sizeof(card);
        size_t name_size = sizeof(name);

        snprintf(card_key, sizeof(card_key), "nfc_card_%u", i);
        snprintf(name_key, sizeof(name_key), "nfc_name_%u", i);
        if (nvs_get_str(legacy, card_key, card, &card_size) != ESP_OK ||
            nvs_get_str(legacy, name_key, name, &name_size) != ESP_OK ||
            card[0] == '\0' || name[0] == '\0') {
            continue;
        }

        user_record_t record;
        ret = find_or_create_legacy_user(handle, name, &record);
        if (ret != ESP_OK) {
            break;
        }
        copy_text(record.nfc_card_hex, sizeof(record.nfc_card_hex), card);

        uint8_t slot = 0;
        ret = find_slot_by_employee(handle, record.employee_id, &slot, NULL);
        if (ret != ESP_OK) {
            break;
        }
        ret = save_slot(handle, slot, &record);
        if (ret != ESP_OK) {
            break;
        }
    }

    nvs_close(legacy);
    return ret;
}

static esp_err_t migrate_legacy_faces(nvs_handle_t handle)
{
    nvs_handle_t legacy;
    esp_err_t ret = nvs_open(USER_LEGACY_FACE_NAMESPACE, NVS_READONLY, &legacy);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint16_t feature_id = 1; feature_id <= USER_LEGACY_FACE_SCAN_MAX; feature_id++) {
        char key[16];
        char name[USER_NAME_MAX_LEN] = {};
        size_t name_size = sizeof(name);

        snprintf(key, sizeof(key), "face_user_%u", feature_id);
        ret = nvs_get_str(legacy, key, name, &name_size);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (ret != ESP_OK) {
            break;
        }
        if (name[0] == '\0') {
            continue;
        }

        user_record_t record;
        ret = find_or_create_legacy_user(handle, name, &record);
        if (ret != ESP_OK) {
            break;
        }
        if (record.face_feature_count < USER_FACE_FEATURE_MAX_COUNT) {
            record.face_feature_ids[record.face_feature_count++] = feature_id;
        }

        uint8_t slot = 0;
        ret = find_slot_by_employee(handle, record.employee_id, &slot, NULL);
        if (ret != ESP_OK) {
            break;
        }
        ret = save_slot(handle, slot, &record);
        if (ret != ESP_OK) {
            break;
        }
    }

    nvs_close(legacy);
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

bool user_manager_is_valid_employee_id(const char *employee_id)
{
    if (employee_id == NULL || employee_id[0] == '\0') {
        return false;
    }

    for (const char *p = employee_id; *p != '\0'; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

void user_manager_format_identity(const char *employee_id, const char *name, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    snprintf(buf, buf_size, "%s  %s",
             employee_id != NULL && employee_id[0] != '\0' ? employee_id : "--",
             name != NULL && name[0] != '\0' ? name : "--");
}

void user_manager_format_display_name(const user_record_t *record, char *buf, size_t buf_size)
{
    if (record == NULL) {
        user_manager_format_identity(NULL, NULL, buf, buf_size);
        return;
    }
    user_manager_format_identity(record->employee_id, record->name, buf, buf_size);
}

esp_err_t user_manager_init(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t migrated = 0;
    ret = nvs_get_u8(handle, USER_NVS_MIGRATED_KEY, &migrated);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        migrated = 0;
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    if (!migrated) {
        ret = migrate_legacy_nfc(handle);
        if (ret == ESP_OK) {
            ret = migrate_legacy_faces(handle);
        }
        if (ret == ESP_OK) {
            ret = nvs_set_u8(handle, USER_NVS_MIGRATED_KEY, 1);
        }
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        ESP_LOGI(TAG, "Legacy user migration %s", ret == ESP_OK ? "done" : esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_upsert_identity(const char *employee_id, const char *name)
{
    if (!user_manager_is_valid_employee_id(employee_id) || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t slot = 0;
    user_record_t record;
    ret = find_slot_by_employee(handle, employee_id, &slot, &record);
    if (ret == ESP_ERR_NOT_FOUND) {
        uint8_t count = 0;
        ret = get_count(handle, &count);
        if (ret == ESP_OK) {
            if (count >= USER_MANAGER_MAX_USERS) {
                ret = ESP_ERR_NO_MEM;
            } else {
                memset(&record, 0, sizeof(record));
                copy_text(record.employee_id, sizeof(record.employee_id), employee_id);
                copy_text(record.name, sizeof(record.name), name);
                ret = save_slot(handle, count, &record);
                if (ret == ESP_OK) {
                    ret = nvs_set_u8(handle, USER_NVS_COUNT_KEY, count + 1);
                }
            }
        }
    } else if (ret == ESP_OK) {
        copy_text(record.name, sizeof(record.name), name);
        ret = save_slot(handle, slot, &record);
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_set_face(const char *employee_id, const uint16_t *feature_ids,
                                uint8_t feature_count, const char *photo_path)
{
    if (!user_manager_is_valid_employee_id(employee_id) || feature_ids == NULL ||
        feature_count == 0 || feature_count > USER_FACE_FEATURE_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = get_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    uint8_t target_slot = 0;
    user_record_t target;
    ret = find_slot_by_employee(handle, employee_id, &target_slot, &target);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    uint16_t old_feature_ids[USER_FACE_FEATURE_MAX_COUNT] = {};
    uint8_t old_feature_count = target.face_feature_count;
    memcpy(old_feature_ids, target.face_feature_ids, sizeof(old_feature_ids));

    for (uint8_t i = 0; i < count; i++) {
        user_record_t current;
        ret = load_slot(handle, i, &current);
        if (ret != ESP_OK) {
            break;
        }
        if (strcmp(current.employee_id, employee_id) == 0) {
            continue;
        }

        bool changed = false;
        for (uint8_t j = 0; j < current.face_feature_count;) {
            bool duplicate = false;
            for (uint8_t k = 0; k < feature_count; k++) {
                if (current.face_feature_ids[j] == feature_ids[k]) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                j++;
                continue;
            }

            for (uint8_t m = j + 1; m < current.face_feature_count; m++) {
                current.face_feature_ids[m - 1] = current.face_feature_ids[m];
            }
            current.face_feature_ids[--current.face_feature_count] = 0;
            changed = true;
        }
        if (changed) {
            ret = save_slot(handle, i, &current);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    if (ret == ESP_OK) {
        memset(target.face_feature_ids, 0, sizeof(target.face_feature_ids));
        memcpy(target.face_feature_ids, feature_ids, feature_count * sizeof(uint16_t));
        target.face_feature_count = feature_count;
        copy_text(target.face_photo_path, sizeof(target.face_photo_path), photo_path);
        ret = save_slot(handle, target_slot, &target);
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK && old_feature_count > 0) {
        uint16_t stale_ids[USER_FACE_FEATURE_MAX_COUNT] = {};
        uint8_t stale_count = 0;
        for (uint8_t i = 0; i < old_feature_count; i++) {
            bool still_used = false;
            for (uint8_t j = 0; j < feature_count; j++) {
                if (old_feature_ids[i] == feature_ids[j]) {
                    still_used = true;
                    break;
                }
            }
            if (!still_used) {
                stale_ids[stale_count++] = old_feature_ids[i];
            }
        }
        if (stale_count > 0) {
            esp_err_t del_ret = face_recognition_delete_features(stale_ids, stale_count);
            if (del_ret != ESP_OK) {
                ESP_LOGW(TAG, "Delete stale face features failed: %s", esp_err_to_name(del_ret));
            }
        }
    }

    return ret;
}

esp_err_t user_manager_set_nfc(const char *employee_id, const char *card_hex)
{
    if (!user_manager_is_valid_employee_id(employee_id) || card_hex == NULL || card_hex[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized_card[NFC_CARD_HEX_LEN + 1];
    esp_err_t ret = nfc_manager_normalize_card_hex(card_hex, normalized_card,
                                                   sizeof(normalized_card), NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = get_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    uint8_t target_slot = 0;
    user_record_t target;
    ret = find_slot_by_employee(handle, employee_id, &target_slot, &target);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        user_record_t current;
        ret = load_slot(handle, i, &current);
        if (ret != ESP_OK) {
            break;
        }
        char current_card[NFC_CARD_HEX_LEN + 1];
        bool same_card = nfc_manager_normalize_card_hex(current.nfc_card_hex, current_card,
                                                        sizeof(current_card), NULL) == ESP_OK &&
                         strcmp(current_card, normalized_card) == 0;
        if (strcmp(current.employee_id, employee_id) != 0 && same_card) {
            current.nfc_card_hex[0] = '\0';
            ret = save_slot(handle, i, &current);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    if (ret == ESP_OK) {
        copy_text(target.nfc_card_hex, sizeof(target.nfc_card_hex), normalized_card);
        ret = save_slot(handle, target_slot, &target);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_list(user_record_t *records, size_t max_records, size_t *out_count)
{
    if (records == NULL && max_records > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_count != NULL) {
        *out_count = 0;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = get_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    size_t copied = count < max_records ? count : max_records;
    for (size_t i = 0; i < copied; i++) {
        ret = load_slot(handle, (uint8_t)i, &records[i]);
        if (ret != ESP_OK) {
            break;
        }
    }

    if (ret == ESP_OK && out_count != NULL) {
        *out_count = count;
    }
    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_find_by_employee_id(const char *employee_id, user_record_t *record)
{
    if (!user_manager_is_valid_employee_id(employee_id) || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }
    ret = find_slot_by_employee(handle, employee_id, NULL, record);
    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_find_by_face_feature(uint16_t feature_id, user_record_t *record)
{
    if (feature_id == 0 || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }

    uint8_t count = 0;
    ret = get_count(handle, &count);
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_FOUND;
        for (uint8_t i = 0; i < count; i++) {
            user_record_t current;
            esp_err_t load_ret = load_slot(handle, i, &current);
            if (load_ret != ESP_OK) {
                ret = load_ret;
                break;
            }
            for (uint8_t j = 0; j < current.face_feature_count; j++) {
                if (current.face_feature_ids[j] == feature_id) {
                    *record = current;
                    ret = ESP_OK;
                    break;
                }
            }
            if (ret == ESP_OK) {
                break;
            }
        }
    }

    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_find_by_nfc_card(const char *card_hex, user_record_t *record)
{
    if (card_hex == NULL || card_hex[0] == '\0' || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized_card[NFC_CARD_HEX_LEN + 1];
    esp_err_t ret = nfc_manager_normalize_card_hex(card_hex, normalized_card,
                                                   sizeof(normalized_card), NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(USER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }

    uint8_t count = 0;
    ret = get_count(handle, &count);
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_FOUND;
        for (uint8_t i = 0; i < count; i++) {
            user_record_t current;
            esp_err_t load_ret = load_slot(handle, i, &current);
            if (load_ret != ESP_OK) {
                ret = load_ret;
                break;
            }
            char current_card[NFC_CARD_HEX_LEN + 1];
            bool same_card = nfc_manager_normalize_card_hex(current.nfc_card_hex, current_card,
                                                            sizeof(current_card), NULL) == ESP_OK &&
                             strcmp(current_card, normalized_card) == 0;
            if (same_card) {
                *record = current;
                ret = ESP_OK;
                break;
            }
        }
    }

    nvs_close(handle);
    return ret;
}

esp_err_t user_manager_delete_user(const char *employee_id)
{
    if (!user_manager_is_valid_employee_id(employee_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    uint8_t slot = 0;
    user_record_t record;

    ret = get_count(handle, &count);
    if (ret == ESP_OK) {
        ret = find_slot_by_employee(handle, employee_id, &slot, &record);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = face_recognition_delete_user_data(record.employee_id,
                                            record.name,
                                            record.face_feature_ids,
                                            record.face_feature_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Delete face features failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (record.nfc_card_hex[0] != '\0') {
        ret = nfc_manager_delete_user_cards(employee_id);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Delete NFC card index failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    if (record.face_photo_path[0] != '\0') {
        unlink(record.face_photo_path);
    }

    ret = nvs_open(USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = get_count(handle, &count);
    if (ret == ESP_OK) {
        ret = find_slot_by_employee(handle, employee_id, &slot, NULL);
    }
    if (ret == ESP_OK) {
        for (uint8_t i = slot + 1; i < count; i++) {
            user_record_t next;
            ret = load_slot(handle, i, &next);
            if (ret != ESP_OK) {
                break;
            }
            ret = save_slot(handle, i - 1, &next);
            if (ret != ESP_OK) {
                break;
            }
        }
    }
    if (ret == ESP_OK && count > 0) {
        ret = erase_slot(handle, count - 1);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, USER_NVS_COUNT_KEY, count > 0 ? count - 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "User deleted: %s", employee_id);
    return ret;
}

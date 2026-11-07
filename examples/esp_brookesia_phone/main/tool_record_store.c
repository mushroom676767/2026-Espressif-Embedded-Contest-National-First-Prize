#include "tool_record_store.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#define TOOL_RECORD_STORE_PATH "/spiffs/tool_records.bin"
#define TOOL_RECORD_STORE_TMP_PATH "/spiffs/tool_records_v2.tmp"
#define TOOL_RECORD_STORE_BACKUP_PATH "/spiffs/tool_records_v1.bak"
#define TOOL_STATE_STORE_PATH "/spiffs/tool_states.bin"
#define TOOL_RECORD_STORE_V1_MAGIC 0x54435244u
#define TOOL_RECORD_STORE_MAGIC 0x54435245u
#define TOOL_STATE_STORE_MAGIC 0x54435354u

typedef struct {
    uint32_t magic;
    uint32_t next_record_id;
    uint32_t count;
    uint32_t write_index;
} record_store_header_t;

typedef struct {
    uint32_t record_id;
    char event_id[TOOL_RECORD_EVENT_ID_LEN];
    char device_code[TOOL_RECORD_DEVICE_CODE_LEN];
    char operator_name[TOOL_RECORD_OPERATOR_LEN];
    char result_type[TOOL_RECORD_RESULT_LEN];
    char opened_at[TOOL_RECORD_TIME_LEN];
    char closed_at[TOOL_RECORD_TIME_LEN];
    tool_record_sync_t sync_status;
    char snapshot_local_path[TOOL_RECORD_PATH_LEN];
    char inspection_image_url[TOOL_RECORD_URL_LEN];
    char state_source[16];
    uint8_t borrowed_count;
    uint8_t returned_count;
    uint8_t anomaly_count;
} tool_record_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    tool_slot_runtime_t states[TOOL_CABINET_SLOT_COUNT];
} state_store_file_t;

_Static_assert(TOOL_RECORD_STORE_MAX_RECORDS == 100, "TOOL_RECORD_STORE_MAX_RECORDS 100");

static const char *TAG = "tool_record_store";
static record_store_header_t s_header;
static bool s_ready;
static bool s_failed_v1_migration;

static long record_offset(uint32_t index)
{
    return (long)sizeof(record_store_header_t) + (long)index * (long)sizeof(tool_record_t);
}

static esp_err_t write_empty_store(void)
{
    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open %s for reset failed: errno=%d", TOOL_RECORD_STORE_PATH, errno);
        return ESP_FAIL;
    }

    record_store_header_t header = {
        .magic = TOOL_RECORD_STORE_MAGIC,
        .next_record_id = 1,
        .count = 0,
        .write_index = 0,
    };
    tool_record_t empty = {0};

    bool ok = fwrite(&header, sizeof(header), 1, fp) == 1;
    for (uint32_t i = 0; ok && i < TOOL_RECORD_STORE_MAX_RECORDS; i++) {
        ok = fwrite(&empty, sizeof(empty), 1, fp) == 1;
    }

    fclose(fp);
    if (!ok) {
        ESP_LOGE(TAG, "Initialize record store file failed");
        return ESP_FAIL;
    }

    s_header = header;
    return ESP_OK;
}

static esp_err_t migrate_v1_store(const record_store_header_t *old_header)
{
    if (old_header == NULL || old_header->count > TOOL_RECORD_STORE_MAX_RECORDS ||
        old_header->write_index >= TOOL_RECORD_STORE_MAX_RECORDS ||
        old_header->next_record_id == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *old_fp = fopen(TOOL_RECORD_STORE_PATH, "rb");
    FILE *new_fp = fopen(TOOL_RECORD_STORE_TMP_PATH, "wb");
    if (old_fp == NULL || new_fp == NULL) {
        if (old_fp != NULL) fclose(old_fp);
        if (new_fp != NULL) fclose(new_fp);
        remove(TOOL_RECORD_STORE_TMP_PATH);
        return ESP_FAIL;
    }

    record_store_header_t new_header = {
        .magic = TOOL_RECORD_STORE_MAGIC,
        .next_record_id = old_header->next_record_id,
        .count = old_header->count,
        .write_index = old_header->count % TOOL_RECORD_STORE_MAX_RECORDS,
    };
    tool_record_t empty = {0};
    bool ok = fwrite(&new_header, sizeof(new_header), 1, new_fp) == 1;
    for (uint32_t i = 0; ok && i < TOOL_RECORD_STORE_MAX_RECORDS; ++i) {
        ok = fwrite(&empty, sizeof(empty), 1, new_fp) == 1;
    }

    uint32_t oldest = (old_header->write_index + TOOL_RECORD_STORE_MAX_RECORDS -
                       old_header->count) % TOOL_RECORD_STORE_MAX_RECORDS;
    for (uint32_t i = 0; ok && i < old_header->count; ++i) {
        uint32_t old_index = (oldest + i) % TOOL_RECORD_STORE_MAX_RECORDS;
        tool_record_v1_t old_record = {0};
        long old_offset = (long)sizeof(record_store_header_t) +
                          (long)old_index * (long)sizeof(tool_record_v1_t);
        if (fseek(old_fp, old_offset, SEEK_SET) != 0 ||
            fread(&old_record, sizeof(old_record), 1, old_fp) != 1) {
            ok = false;
            break;
        }
        tool_record_t migrated = {0};
        migrated.record_id = old_record.record_id;
        snprintf(migrated.event_id, sizeof(migrated.event_id), "%s", old_record.event_id);
        snprintf(migrated.operation_id, sizeof(migrated.operation_id), "%s", old_record.event_id);
        snprintf(migrated.device_code, sizeof(migrated.device_code), "%s", old_record.device_code);
        snprintf(migrated.operator_name, sizeof(migrated.operator_name), "%s", old_record.operator_name);
        snprintf(migrated.result_type, sizeof(migrated.result_type), "%s", old_record.result_type);
        snprintf(migrated.opened_at, sizeof(migrated.opened_at), "%s", old_record.opened_at);
        snprintf(migrated.closed_at, sizeof(migrated.closed_at), "%s", old_record.closed_at);
        migrated.sync_status = old_record.sync_status;
        snprintf(migrated.snapshot_local_path, sizeof(migrated.snapshot_local_path), "%s",
                 old_record.snapshot_local_path);
        snprintf(migrated.inspection_image_url, sizeof(migrated.inspection_image_url), "%s",
                 old_record.inspection_image_url);
        snprintf(migrated.state_source, sizeof(migrated.state_source), "%s", old_record.state_source);
        migrated.borrowed_count = old_record.borrowed_count;
        migrated.returned_count = old_record.returned_count;
        migrated.anomaly_count = old_record.anomaly_count;
        migrated.vision_complete = false;
        migrated.door_state = TOOL_RECORD_DOOR_NORMAL;
        long new_offset = (long)sizeof(record_store_header_t) +
                          (long)i * (long)sizeof(tool_record_t);
        if (fseek(new_fp, new_offset, SEEK_SET) != 0 ||
            fwrite(&migrated, sizeof(migrated), 1, new_fp) != 1) {
            ok = false;
        }
    }
    fclose(old_fp);
    fclose(new_fp);
    if (!ok) {
        remove(TOOL_RECORD_STORE_TMP_PATH);
        return ESP_FAIL;
    }

    remove(TOOL_RECORD_STORE_BACKUP_PATH);
    if (rename(TOOL_RECORD_STORE_PATH, TOOL_RECORD_STORE_BACKUP_PATH) != 0) {
        remove(TOOL_RECORD_STORE_TMP_PATH);
        return ESP_FAIL;
    }
    if (rename(TOOL_RECORD_STORE_TMP_PATH, TOOL_RECORD_STORE_PATH) != 0) {
        (void)rename(TOOL_RECORD_STORE_BACKUP_PATH, TOOL_RECORD_STORE_PATH);
        return ESP_FAIL;
    }
    remove(TOOL_RECORD_STORE_BACKUP_PATH);
    s_header = new_header;
    ESP_LOGI(TAG, "Migrated %u tool records from v1 to v2", (unsigned int)new_header.count);
    return ESP_OK;
}

static esp_err_t load_header(void)
{
    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    bool ok = fread(&s_header, sizeof(s_header), 1, fp) == 1;
    fclose(fp);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_header.magic == TOOL_RECORD_STORE_V1_MAGIC) {
        esp_err_t migration_ret = migrate_v1_store(&s_header);
        s_failed_v1_migration = migration_ret != ESP_OK;
        return migration_ret;
    }
    if (s_header.magic != TOOL_RECORD_STORE_MAGIC ||
        s_header.next_record_id == 0 ||
        s_header.count > TOOL_RECORD_STORE_MAX_RECORDS ||
        s_header.write_index >= TOOL_RECORD_STORE_MAX_RECORDS) {
        ESP_LOGW(TAG, "Record store header invalid, resetting file");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t save_header(void)
{
    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb+");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open %s for header update failed: errno=%d", TOOL_RECORD_STORE_PATH, errno);
        return ESP_FAIL;
    }

    bool ok = fwrite(&s_header, sizeof(s_header), 1, fp) == 1;
    fclose(fp);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t ensure_ready(void)
{
    return s_ready ? ESP_OK : tool_record_store_init();
}

static void copy_optional_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL) {
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

esp_err_t tool_record_store_init(void)
{
    s_failed_v1_migration = false;
    esp_err_t ret = load_header();
    if (ret != ESP_OK && !s_failed_v1_migration) {
        ret = write_empty_store();
    }

    s_ready = (ret == ESP_OK);
    if (s_ready) {
        ESP_LOGI(TAG, "Record store ready: count=%" PRIu32 " next=%" PRIu32,
                 s_header.count, s_header.next_record_id);
    }
    return ret;
}

esp_err_t tool_record_store_append(const tool_record_t *record, uint32_t *record_id_out)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb+");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open %s for append failed: errno=%d", TOOL_RECORD_STORE_PATH, errno);
        return ESP_FAIL;
    }

    tool_record_t writable = *record;
    writable.record_id = s_header.next_record_id++;
    if (s_header.next_record_id == 0) {
        s_header.next_record_id = 1;
    }

    if (fseek(fp, record_offset(s_header.write_index), SEEK_SET) != 0 ||
        fwrite(&writable, sizeof(writable), 1, fp) != 1) {
        fclose(fp);
        ESP_LOGE(TAG, "Write record failed: errno=%d", errno);
        return ESP_FAIL;
    }
    fclose(fp);

    if (record_id_out != NULL) {
        *record_id_out = writable.record_id;
    }

    s_header.write_index = (s_header.write_index + 1) % TOOL_RECORD_STORE_MAX_RECORDS;
    if (s_header.count < TOOL_RECORD_STORE_MAX_RECORDS) {
        s_header.count++;
    }

    ret = save_header();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Record appended: id=%" PRIu32, writable.record_id);
    }
    return ret;
}

esp_err_t tool_record_store_update_sync(uint32_t record_id,
                                        tool_record_sync_t sync,
                                        const char *inspection_image_url)
{
    if (record_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb+");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < TOOL_RECORD_STORE_MAX_RECORDS; i++) {
        tool_record_t record;
        if (fseek(fp, record_offset(i), SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, fp) != 1) {
            fclose(fp);
            return ESP_FAIL;
        }

        if (record.record_id != record_id) {
            continue;
        }

        record.sync_status = sync;
        if (inspection_image_url != NULL && inspection_image_url[0] != '\0') {
            copy_optional_string(record.inspection_image_url, sizeof(record.inspection_image_url), inspection_image_url);
        }

        if (fseek(fp, record_offset(i), SEEK_SET) != 0 ||
            fwrite(&record, sizeof(record), 1, fp) != 1) {
            fclose(fp);
            return ESP_FAIL;
        }

        fclose(fp);
        return ESP_OK;
    }

    fclose(fp);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t tool_record_store_update_door(uint32_t record_id,
                                        tool_record_door_state_t door_state,
                                        const char *alert_event_id,
                                        const char *closed_at)
{
    if (record_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_ready();
    if (ret != ESP_OK) {
        return ret;
    }
    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb+");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    for (uint32_t i = 0; i < TOOL_RECORD_STORE_MAX_RECORDS; ++i) {
        tool_record_t record = {0};
        if (fseek(fp, record_offset(i), SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, fp) != 1) {
            fclose(fp);
            return ESP_FAIL;
        }
        if (record.record_id != record_id) {
            continue;
        }
        record.door_state = door_state;
        if (alert_event_id != NULL) {
            snprintf(record.alert_event_id, sizeof(record.alert_event_id), "%s", alert_event_id);
        }
        if (closed_at != NULL) {
            snprintf(record.closed_at, sizeof(record.closed_at), "%s", closed_at);
        }
        if (fseek(fp, record_offset(i), SEEK_SET) != 0 ||
            fwrite(&record, sizeof(record), 1, fp) != 1) {
            fclose(fp);
            return ESP_FAIL;
        }
        fclose(fp);
        return ESP_OK;
    }
    fclose(fp);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t tool_record_store_list_latest(tool_record_t *records,
                                        size_t max_count,
                                        size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }
    if ((records == NULL && max_count > 0) || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *fp = fopen(TOOL_RECORD_STORE_PATH, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    uint32_t available = s_header.count;
    size_t limit = max_count < available ? max_count : available;
    for (size_t i = 0; i < limit; i++) {
        uint32_t index = (s_header.write_index + TOOL_RECORD_STORE_MAX_RECORDS - 1 - i) %
                         TOOL_RECORD_STORE_MAX_RECORDS;
        if (fseek(fp, record_offset(index), SEEK_SET) != 0 ||
            fread(&records[i], sizeof(records[i]), 1, fp) != 1) {
            fclose(fp);
            return ESP_FAIL;
        }
    }

    fclose(fp);
    *out_count = limit;
    return ESP_OK;
}

esp_err_t tool_record_store_clear_all(void)
{
    esp_err_t ret = write_empty_store();
    if (ret == ESP_OK) {
        s_ready = true;
    }
    if (remove(TOOL_STATE_STORE_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "Remove state store failed: errno=%d", errno);
        if (ret == ESP_OK) {
            ret = ESP_FAIL;
        }
    }
    (void)remove(TOOL_RECORD_STORE_TMP_PATH);
    (void)remove(TOOL_RECORD_STORE_BACKUP_PATH);
    return ret;
}

esp_err_t tool_record_store_save_states(const tool_slot_runtime_t *states, size_t count)
{
    if (states == NULL || count < TOOL_CABINET_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    state_store_file_t file = {
        .magic = TOOL_STATE_STORE_MAGIC,
        .count = TOOL_CABINET_SLOT_COUNT,
    };
    memcpy(file.states, states, sizeof(file.states));

    FILE *fp = fopen(TOOL_STATE_STORE_PATH, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Open %s for state save failed: errno=%d", TOOL_STATE_STORE_PATH, errno);
        return ESP_FAIL;
    }

    bool ok = fwrite(&file, sizeof(file), 1, fp) == 1;
    fclose(fp);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t tool_record_store_load_states(tool_slot_runtime_t *states,
                                        size_t max_count,
                                        size_t *out_count)
{
    if ((states == NULL && max_count > 0) || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (max_count < TOOL_CABINET_SLOT_COUNT) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(TOOL_STATE_STORE_PATH, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    state_store_file_t file;
    bool ok = fread(&file, sizeof(file), 1, fp) == 1;
    fclose(fp);
    if (!ok || file.magic != TOOL_STATE_STORE_MAGIC || file.count != TOOL_CABINET_SLOT_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(states, file.states, sizeof(file.states));
    *out_count = TOOL_CABINET_SLOT_COUNT;
    return ESP_OK;
}

const char *tool_record_sync_label(tool_record_sync_t sync)
{
    switch (sync) {
        case TOOL_RECORD_SYNC_PENDING:
            return "待同步";
        case TOOL_RECORD_SYNC_SYNCED:
            return "已同步";
        case TOOL_RECORD_SYNC_FAILED:
        default:
            return "失败";
    }
}

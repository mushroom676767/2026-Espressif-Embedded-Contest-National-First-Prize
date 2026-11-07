#include "cloud_uploader.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "wifi_manager.h"

#define CLOUD_UPLOADER_EVENTS_PATH "/api/device/events"
#define CLOUD_UPLOADER_SNAPSHOT_PATH "/api/device/snapshot"
#define CLOUD_UPLOADER_UPLOAD_IMAGE_PATH "/api/device/upload-image"
#define CLOUD_UPLOADER_DEFAULT_INSPECTION_PATH "/api/inspection/upload-and-analyze"

_Static_assert(CLOUD_UPLOADER_EVENT_TYPE_LEN >= sizeof("door_unconfirmed"),
               "event_type buffer must include the longest value and NUL terminator");
#define CLOUD_UPLOADER_HTTP_TIMEOUT_MS 60000
#define CLOUD_UPLOADER_RESPONSE_MAX 1024
#define CLOUD_UPLOADER_FILE_CHUNK 1024

static const char *TAG = "cloud_uploader";
static bool s_initialized;

static esp_err_t build_url(const char *path, char *out, size_t out_size)
{
    const char *base = CONFIG_CLOUD_UPLOADER_URL;
    if (base == NULL || base[0] == '\0' || path == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(base);
    bool base_has_slash = base_len > 0 && base[base_len - 1] == '/';
    bool path_has_slash = path[0] == '/';
    int written = snprintf(out, out_size, "%s%s%s",
                           base,
                           (base_has_slash || path_has_slash) ? "" : "/",
                           (base_has_slash && path_has_slash) ? path + 1 : path);
    return (written > 0 && (size_t)written < out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void add_device_key_header(esp_http_client_handle_t client)
{
    if (CONFIG_CLOUD_UPLOADER_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "X-Device-Key", CONFIG_CLOUD_UPLOADER_API_KEY);
    }
}

static const char *device_key_state(void)
{
    return CONFIG_CLOUD_UPLOADER_API_KEY[0] != '\0' ? "set" : "empty";
}

static bool should_url_encode_byte(unsigned char ch)
{
    return !(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~');
}

static esp_err_t url_encode(const char *src, char *out, size_t out_size)
{
    if (src == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        if (should_url_encode_byte(*p)) {
            if (pos + 3 >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[pos++] = '%';
            out[pos++] = hex[*p >> 4];
            out[pos++] = hex[*p & 0x0F];
        } else {
            if (pos + 1 >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[pos++] = (char)*p;
        }
    }
    out[pos] = '\0';
    return ESP_OK;
}

static int http_read_response(esp_http_client_handle_t client, char *response, size_t response_size)
{
    if (response == NULL || response_size == 0) {
        return 0;
    }

    size_t used = 0;
    while (used + 1 < response_size) {
        int read_len = esp_http_client_read(client, response + used, (int)(response_size - used - 1));
        if (read_len <= 0) {
            break;
        }
        used += (size_t)read_len;
    }
    response[used] = '\0';
    return esp_http_client_get_status_code(client);
}

static esp_err_t http_post_json(const char *path, const char *json_body)
{
    if (!cloud_uploader_is_ready()) {
        ESP_LOGW(TAG, "POST JSON skipped: uploader not ready path=%s initialized=%d url_set=%d wifi=%d",
                 path != NULL ? path : "(null)",
                 s_initialized,
                 CONFIG_CLOUD_UPLOADER_URL[0] != '\0',
                 wifi_manager_is_connected());
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    esp_err_t ret = build_url(path, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_UPLOADER_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    add_device_key_header(client);

    int body_len = (int)strlen(json_body);
    ESP_LOGI(TAG, "POST JSON start: url=%s bytes=%d device_key=%s", url, body_len, device_key_state());
    ret = esp_http_client_open(client, body_len);
    if (ret == ESP_OK) {
        int written = esp_http_client_write(client, json_body, body_len);
        if (written != body_len) {
            ret = ESP_FAIL;
        }
    }

    int status = 0;
    if (ret == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        char response[CLOUD_UPLOADER_RESPONSE_MAX];
        status = http_read_response(client, response, sizeof(response));
        ESP_LOGI(TAG, "POST JSON response: path=%s http=%d body=%s",
                 path, status, response[0] != '\0' ? response : "(empty)");
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        return ret;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST %s failed: http=%d", path, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void add_change_array(cJSON *root, const char *name, const cloud_tool_change_t *changes, int count)
{
    cJSON *arr = cJSON_AddArrayToObject(root, name);
    if (arr == NULL || changes == NULL || count <= 0) {
        return;
    }

    int limit = count < CLOUD_UPLOADER_MAX_CHANGES ? count : CLOUD_UPLOADER_MAX_CHANGES;
    for (int i = 0; i < limit; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "slot_no", changes[i].slot_no);
        cJSON_AddStringToObject(item, "tool_code", changes[i].tool_code);
        cJSON_AddStringToObject(item, "tool_name", changes[i].tool_name);
        cJSON_AddStringToObject(item, "tool_class", changes[i].tool_class);
        cJSON_AddNumberToObject(item, "quantity", changes[i].quantity > 0 ? changes[i].quantity : 1);
        cJSON_AddItemToArray(arr, item);
    }
}

static void add_optional_string(cJSON *root, const char *name, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        cJSON_AddStringToObject(root, name, value);
    }
}

static esp_err_t post_json_object(const char *path, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = http_post_json(path, body);
    cJSON_free(body);
    return ret;
}

esp_err_t cloud_uploader_init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "Cloud uploader configured: url=%s device=%s inspection=%s",
             CONFIG_CLOUD_UPLOADER_URL,
             CONFIG_CLOUD_UPLOADER_DEVICE_CODE,
             CONFIG_CLOUD_UPLOADER_INSPECTION_UPLOAD_PATH);
    return ESP_OK;
}

bool cloud_uploader_is_ready(void)
{
    return s_initialized &&
           CONFIG_CLOUD_UPLOADER_URL[0] != '\0' &&
           wifi_manager_is_connected();
}

esp_err_t cloud_uploader_upload_inspection_image(const cloud_inspection_image_t *image,
                                                 char *image_url_out,
                                                 size_t image_url_out_size)
{
    if (image == NULL || image->local_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (image_url_out != NULL && image_url_out_size > 0) {
        image_url_out[0] = '\0';
    }
    if (!cloud_uploader_is_ready()) {
        ESP_LOGW(TAG, "Inspection upload skipped: uploader not ready path=%s initialized=%d url_set=%d wifi=%d",
                 image->local_path,
                 s_initialized,
                 CONFIG_CLOUD_UPLOADER_URL[0] != '\0',
                 wifi_manager_is_connected());
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(image->local_path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "Open inspection image failed: %s errno=%d", image->local_path, errno);
        return ESP_FAIL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long file_size = ftell(fp);
    if (file_size <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Inspection upload prepare: local_path=%s size=%ld tool_code=%s key=%s",
             image->local_path,
             file_size,
             image->tool_code[0] ? image->tool_code : "CABINET",
             device_key_state());

    char tool_code[96];
    char tool_name[160];
    char tool_class[96];
    esp_err_t ret = ESP_OK;
    ret = url_encode(image->tool_code[0] ? image->tool_code : "CABINET", tool_code, sizeof(tool_code));
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }
    ret = url_encode(image->tool_name[0] ? image->tool_name : "cabinet_snapshot", tool_name, sizeof(tool_name));
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }
    ret = url_encode(image->tool_class[0] ? image->tool_class : "cabinet_snapshot", tool_class, sizeof(tool_class));
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }

    const char *path = CONFIG_CLOUD_UPLOADER_INSPECTION_UPLOAD_PATH[0] != '\0' ?
                       CONFIG_CLOUD_UPLOADER_INSPECTION_UPLOAD_PATH :
                       CLOUD_UPLOADER_DEFAULT_INSPECTION_PATH;
    char path_with_query[320];
    int query_len = snprintf(path_with_query, sizeof(path_with_query),
                             "%s?tool_code=%s&tool_name=%s&tool_class=%s",
                             path, tool_code, tool_name, tool_class);
    if (query_len <= 0 || (size_t)query_len >= sizeof(path_with_query)) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    char url[448];
    ret = build_url(path_with_query, url, sizeof(url));
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }
    const char *boundary = "----esp32p4toolcabinet";
    char part_header[256];
    int header_len = snprintf(part_header, sizeof(part_header),
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"file\"; filename=\"cabinet.jpg\"\r\n"
                              "Content-Type: image/jpeg\r\n\r\n",
                              boundary);
    char part_tail[64];
    int tail_len = snprintf(part_tail, sizeof(part_tail), "\r\n--%s--\r\n", boundary);
    if (header_len <= 0 || tail_len <= 0 ||
        (size_t)header_len >= sizeof(part_header) ||
        (size_t)tail_len >= sizeof(part_tail)) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    int content_length = header_len + (int)file_size + tail_len;
    ESP_LOGI(TAG, "Inspection upload start: url=%s content_length=%d", url, content_length);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_UPLOADER_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    add_device_key_header(client);

    ret = esp_http_client_open(client, content_length);
    if (ret == ESP_OK && esp_http_client_write(client, part_header, header_len) != header_len) {
        ret = ESP_FAIL;
    }

    char chunk[CLOUD_UPLOADER_FILE_CHUNK];
    while (ret == ESP_OK) {
        size_t read_len = fread(chunk, 1, sizeof(chunk), fp);
        if (read_len > 0) {
            if (esp_http_client_write(client, chunk, (int)read_len) != (int)read_len) {
                ret = ESP_FAIL;
                break;
            }
        }
        if (read_len < sizeof(chunk)) {
            if (ferror(fp)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }

    if (ret == ESP_OK && esp_http_client_write(client, part_tail, tail_len) != tail_len) {
        ret = ESP_FAIL;
    }

    fclose(fp);

    int status = 0;
    char response[CLOUD_UPLOADER_RESPONSE_MAX];
    response[0] = '\0';
    if (ret == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = http_read_response(client, response, sizeof(response));
        ESP_LOGI(TAG, "Inspection upload response: http=%d body=%s",
                 status, response[0] != '\0' ? response : "(empty)");
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        return ret;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Inspection image upload failed: http=%d", status);
        return ESP_FAIL;
    }

    if (image_url_out != NULL && image_url_out_size > 0 && response[0] != '\0') {
        cJSON *root = cJSON_Parse(response);
        if (root != NULL) {
            cJSON *image_url = cJSON_GetObjectItemCaseSensitive(root, "image_url");
            if (cJSON_IsString(image_url) && image_url->valuestring != NULL) {
                snprintf(image_url_out, image_url_out_size, "%s", image_url->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    return ESP_OK;
}

esp_err_t cloud_uploader_upload_image_file(const char *local_path,
                                           char *image_url_out,
                                           size_t image_url_out_size)
{
    if (local_path == NULL || local_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (image_url_out != NULL && image_url_out_size > 0) {
        image_url_out[0] = '\0';
    }
    if (!cloud_uploader_is_ready()) {
        ESP_LOGW(TAG, "Generic image upload skipped: uploader not ready path=%s initialized=%d url_set=%d wifi=%d",
                 local_path,
                 s_initialized,
                 CONFIG_CLOUD_UPLOADER_URL[0] != '\0',
                 wifi_manager_is_connected());
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(local_path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "Open generic image failed: %s errno=%d", local_path, errno);
        return ESP_FAIL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long file_size = ftell(fp);
    if (file_size <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    char url[192];
    esp_err_t ret = build_url(CLOUD_UPLOADER_UPLOAD_IMAGE_PATH, url, sizeof(url));
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }

    const char *boundary = "----esp32p4uploadimage";
    char part_header[256];
    int header_len = snprintf(part_header, sizeof(part_header),
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"file\"; filename=\"usb_preview.jpg\"\r\n"
                              "Content-Type: image/jpeg\r\n\r\n",
                              boundary);
    char part_tail[64];
    int tail_len = snprintf(part_tail, sizeof(part_tail), "\r\n--%s--\r\n", boundary);
    if (header_len <= 0 || tail_len <= 0 ||
        (size_t)header_len >= sizeof(part_header) ||
        (size_t)tail_len >= sizeof(part_tail)) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    int content_length = header_len + (int)file_size + tail_len;
    ESP_LOGI(TAG, "Generic image upload start: url=%s content_length=%d key=%s",
             url, content_length, device_key_state());

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_UPLOADER_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    add_device_key_header(client);

    ret = esp_http_client_open(client, content_length);
    if (ret == ESP_OK && esp_http_client_write(client, part_header, header_len) != header_len) {
        ret = ESP_FAIL;
    }

    char chunk[CLOUD_UPLOADER_FILE_CHUNK];
    while (ret == ESP_OK) {
        size_t read_len = fread(chunk, 1, sizeof(chunk), fp);
        if (read_len > 0) {
            if (esp_http_client_write(client, chunk, (int)read_len) != (int)read_len) {
                ret = ESP_FAIL;
                break;
            }
        }
        if (read_len < sizeof(chunk)) {
            if (ferror(fp)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }

    if (ret == ESP_OK && esp_http_client_write(client, part_tail, tail_len) != tail_len) {
        ret = ESP_FAIL;
    }

    fclose(fp);

    int status = 0;
    char response[CLOUD_UPLOADER_RESPONSE_MAX];
    response[0] = '\0';
    if (ret == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = http_read_response(client, response, sizeof(response));
        ESP_LOGI(TAG, "Generic image upload response: http=%d body=%s",
                 status, response[0] != '\0' ? response : "(empty)");
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        return ret;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Generic image upload failed: http=%d", status);
        return ESP_FAIL;
    }

    if (image_url_out != NULL && image_url_out_size > 0 && response[0] != '\0') {
        cJSON *root = cJSON_Parse(response);
        if (root != NULL) {
            cJSON *url_item = NULL;
            cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(data)) {
                url_item = cJSON_GetObjectItemCaseSensitive(data, "image_url");
            }
            if (!cJSON_IsString(url_item)) {
                url_item = cJSON_GetObjectItemCaseSensitive(root, "image_url");
            }
            if (!cJSON_IsString(url_item)) {
                url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
            }
            if (cJSON_IsString(url_item) && url_item->valuestring != NULL) {
                snprintf(image_url_out, image_url_out_size, "%s", url_item->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    return ESP_OK;
}

esp_err_t cloud_uploader_post_event(const cloud_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "event_id", event->event_id);
    cJSON_AddStringToObject(root, "device_code", event->device_code);
    cJSON *operator_info = cJSON_AddObjectToObject(root, "operator");
    if (operator_info != NULL) {
        add_optional_string(operator_info, "user_code", event->operator_info.user_code);
        add_optional_string(operator_info, "name", event->operator_info.name);
        add_optional_string(operator_info, "auth_type", event->operator_info.auth_type);
    }
    cJSON_AddStringToObject(root, "event_type", event->event_type[0] ? event->event_type : "operation");
    add_optional_string(root, "operation_id", event->operation_id);
    add_optional_string(root, "related_event_id", event->related_event_id);
    add_optional_string(root, "occurred_at", event->occurred_at);
    cJSON_AddBoolToObject(root, "door_confirmed", event->door_confirmed);
    cJSON_AddNumberToObject(root, "reminder_count", event->reminder_count);
    cJSON_AddStringToObject(root, "result_type", event->result_type);
    cJSON_AddStringToObject(root, "opened_at", event->opened_at);
    add_optional_string(root, "closed_at", event->closed_at);
    add_change_array(root, "borrowed", event->borrowed, event->borrowed_count);
    add_change_array(root, "returned", event->returned, event->returned_count);
    add_change_array(root, "anomalies", event->anomalies, event->anomaly_count);

    cJSON *raw = cJSON_AddObjectToObject(root, "raw");
    if (raw != NULL) {
        cJSON_AddNumberToObject(raw, "local_record_id", event->local_record_id);
        add_optional_string(raw, "state_source", event->state_source);
        add_optional_string(raw, "inspection_image_url", event->inspection_image_url);
        add_optional_string(raw, "snapshot_local_path", event->snapshot_local_path);
        add_optional_string(raw, "captured_at", event->captured_at);
    }

    esp_err_t ret = post_json_object(CLOUD_UPLOADER_EVENTS_PATH, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t cloud_uploader_post_snapshot(const cloud_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "device_code", snapshot->device_code);
    add_optional_string(root, "timestamp", snapshot->timestamp);
    add_optional_string(root, "firmware_version", snapshot->firmware_version);
    cJSON_AddNumberToObject(root, "total", snapshot->total);
    cJSON_AddNumberToObject(root, "available", snapshot->available);
    add_optional_string(root, "snapshot_id", snapshot->snapshot_id);
    add_optional_string(root, "captured_at", snapshot->captured_at);
    add_optional_string(root, "state_source", snapshot->state_source);
    cJSON_AddNumberToObject(root, "screwdriver_present", snapshot->screwdriver_present);
    cJSON_AddBoolToObject(root, "vision_complete", snapshot->vision_complete);

    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    if (slots != NULL) {
        int limit = snapshot->slot_count < CLOUD_UPLOADER_MAX_SLOTS ?
                    snapshot->slot_count : CLOUD_UPLOADER_MAX_SLOTS;
        for (int i = 0; i < limit; i++) {
            cJSON *slot = cJSON_CreateObject();
            if (slot == NULL) {
                continue;
            }
            cJSON_AddNumberToObject(slot, "slot_no", snapshot->slots[i].slot_no);
            cJSON_AddStringToObject(slot, "tool_code", snapshot->slots[i].tool_code);
            cJSON_AddStringToObject(slot, "tool_name", snapshot->slots[i].tool_name);
            cJSON_AddStringToObject(slot, "tool_class", snapshot->slots[i].tool_class);
            cJSON_AddStringToObject(slot, "status", snapshot->slots[i].status);
            add_optional_string(slot, "image_url", snapshot->slots[i].image_url);
            cJSON_AddItemToArray(slots, slot);
        }
    }

    esp_err_t ret = post_json_object(CLOUD_UPLOADER_SNAPSHOT_PATH, root);
    cJSON_Delete(root);
    return ret;
}

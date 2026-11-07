/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone WiFi Manager - implementation
 */

#include "wifi_manager.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi_manager";

#define WIFI_BIT_STA_CONNECTED     BIT(0)
#define WIFI_BIT_GOT_IP            BIT(1)
#define WIFI_BIT_CONNECT_FAILED    BIT(2)
#define WIFI_BIT_STA_DISCONNECTED  BIT(3)
#define WIFI_BIT_STA_STARTED       BIT(4)

#define WIFI_START_TIMEOUT_MS      2000
#define WIFI_DISCONNECT_TIMEOUT_MS 1000
#define WIFI_CONNECT_TIMEOUT_MS    10000
#define WIFI_SCAN_TASK_STACK       6144
#define WIFI_CONNECT_TASK_STACK    6144
#define WIFI_AUTO_CONNECT_TASK_STACK 8192
#define WIFI_TASK_PRIORITY         5
#define WIFI_SAVED_MAX             5

#define NVS_NAMESPACE              "storage"
#define NVS_KEY_LEGACY_SSID        "wifi_ssid"
#define NVS_KEY_LEGACY_PASS        "wifi_pass"
#define NVS_KEY_COUNT              "wifi_count"
#define NVS_KEY_SSID_PREFIX        "wifi_ssid_"
#define NVS_KEY_PASS_PREFIX        "wifi_pass_"

static EventGroupHandle_t s_wifi_event_group;
static TaskHandle_t s_scan_task_handle = NULL;
static TaskHandle_t s_connect_task_handle = NULL;
static TaskHandle_t s_auto_connect_task_handle = NULL;

static bool s_initialized = false;
static bool s_connected = false;
static bool s_sta_associated = false;
static bool s_connecting = false;
static bool s_scanning = false;
static bool s_scan_cancelled = false;
static bool s_user_disconnect = false;
static bool s_expected_disconnect = false;
static bool s_auto_connect_cancelled = false;

static wifi_state_t s_state = WIFI_STATE_IDLE;
static char s_current_ssid[33] = {0};
static char s_current_pass[65] = {0};
static char s_pending_ssid[33] = {0};
static char s_pending_pass[65] = {0};
static wifi_signal_level_t s_signal_level = WIFI_SIGNAL_NONE;
static wifi_evt_cb_t s_evt_cb = NULL;
static uint8_t s_last_disconnect_reason = 0;

static wifi_ap_info_t s_ap_list[WIFI_SCAN_LIST_SIZE];
static uint16_t s_ap_count = 0;

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_saved_credential_t;

static void clear_pending_credentials(void)
{
    s_pending_ssid[0] = '\0';
    s_pending_pass[0] = '\0';
}

static bool wait_for_sta_started(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & WIFI_BIT_STA_STARTED) {
        return true;
    }

    bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_BIT_STA_STARTED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_START_TIMEOUT_MS)
    );
    if (bits & WIFI_BIT_STA_STARTED) {
        return true;
    }

    esp_err_t start_err = esp_wifi_start();
    if (start_err == ESP_ERR_WIFI_STATE) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_STARTED);
        return true;
    }
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "Start WiFi STA failed: %s", esp_err_to_name(start_err));
        return false;
    }

    bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_BIT_STA_STARTED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_START_TIMEOUT_MS)
    );

    if (!(bits & WIFI_BIT_STA_STARTED)) {
        ESP_LOGW(TAG, "Timed out waiting for WiFi STA start");
        return false;
    }

    return true;
}

static void wait_for_disconnect_event(void)
{
    xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_BIT_STA_DISCONNECTED,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT_MS)
    );
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    size_t len = 0;
    size_t max_len = dst_size - 1;
    while (len < max_len && src[len] != '\0') {
        len++;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_wifi_field(uint8_t *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    memset(dst, 0, dst_size);
    if (src == NULL) {
        return;
    }

    size_t len = strlen(src);
    if (len > dst_size) {
        len = dst_size;
    }
    memcpy(dst, src, len);
}

static void notify_event(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info)
{
    if (s_evt_cb != NULL) {
        s_evt_cb(evt, ap_info);
    }
}

static bool auth_has_password(wifi_auth_mode_t authmode)
{
    return authmode != WIFI_AUTH_OPEN && authmode != WIFI_AUTH_OWE;
}

static const char *disconnect_reason_to_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4WAY_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:
            return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:
            return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:
            return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "INCOMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "AUTHMODE_THRESHOLD";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "RSSI_THRESHOLD";
        default:
            return "UNKNOWN";
    }
}

static wifi_signal_level_t rssi_to_level(int8_t rssi)
{
    if (rssi > -60) {
        return WIFI_SIGNAL_GOOD;
    }
    if (rssi > -80) {
        return WIFI_SIGNAL_MODERATE;
    }
    if (rssi > -100) {
        return WIFI_SIGNAL_WEAK;
    }
    return WIFI_SIGNAL_NONE;
}

static void fill_ap_info(wifi_ap_info_t *dst, const wifi_ap_record_t *src)
{
    memset(dst, 0, sizeof(*dst));
    copy_string(dst->ssid, sizeof(dst->ssid), (const char *)src->ssid);
    dst->rssi = src->rssi;
    dst->authmode = src->authmode;
    dst->has_password = auth_has_password(src->authmode);
    dst->signal = rssi_to_level(src->rssi);
}

static void make_nvs_indexed_key(char *dst, size_t dst_size, const char *prefix, uint8_t index)
{
    snprintf(dst, dst_size, "%s%u", prefix, (unsigned int)index);
}

static bool nvs_read_string(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    size_t len = out_size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    return true;
}

static int saved_credentials_find(const wifi_saved_credential_t *list, uint8_t count, const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static uint8_t saved_credentials_push_front(wifi_saved_credential_t *list, uint8_t count,
                                            const char *ssid, const char *pass)
{
    wifi_saved_credential_t updated[WIFI_SAVED_MAX];
    uint8_t updated_count = 0;
    memset(updated, 0, sizeof(updated));

    copy_string(updated[updated_count].ssid, sizeof(updated[updated_count].ssid), ssid);
    copy_string(updated[updated_count].pass, sizeof(updated[updated_count].pass), pass);
    updated_count++;

    for (uint8_t i = 0; i < count && updated_count < WIFI_SAVED_MAX; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            continue;
        }
        updated[updated_count++] = list[i];
    }

    memset(list, 0, sizeof(wifi_saved_credential_t) * WIFI_SAVED_MAX);
    memcpy(list, updated, sizeof(wifi_saved_credential_t) * updated_count);
    return updated_count;
}

static esp_err_t nvs_store_saved_credentials(nvs_handle_t h,
                                             const wifi_saved_credential_t *list,
                                             uint8_t count)
{
    esp_err_t err = nvs_set_u8(h, NVS_KEY_COUNT, count);
    char key[16];

    for (uint8_t i = 0; err == ESP_OK && i < WIFI_SAVED_MAX; i++) {
        make_nvs_indexed_key(key, sizeof(key), NVS_KEY_SSID_PREFIX, i);
        if (i < count) {
            err = nvs_set_str(h, key, list[i].ssid);
        } else {
            esp_err_t erase_err = nvs_erase_key(h, key);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
        }

        make_nvs_indexed_key(key, sizeof(key), NVS_KEY_PASS_PREFIX, i);
        if (err == ESP_OK && i < count) {
            err = nvs_set_str(h, key, list[i].pass);
        } else if (err == ESP_OK) {
            esp_err_t erase_err = nvs_erase_key(h, key);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    return err;
}

static esp_err_t nvs_load_saved_credentials(wifi_saved_credential_t *list, uint8_t *out_count)
{
    if (list == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(list, 0, sizeof(wifi_saved_credential_t) * WIFI_SAVED_MAX);
    *out_count = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t stored_count = 0;
    if (nvs_get_u8(h, NVS_KEY_COUNT, &stored_count) != ESP_OK) {
        stored_count = 0;
    }
    if (stored_count > WIFI_SAVED_MAX) {
        stored_count = WIFI_SAVED_MAX;
    }

    char key[16];
    for (uint8_t i = 0; i < stored_count; i++) {
        wifi_saved_credential_t item = {0};

        make_nvs_indexed_key(key, sizeof(key), NVS_KEY_SSID_PREFIX, i);
        bool has_ssid = nvs_read_string(h, key, item.ssid, sizeof(item.ssid));

        make_nvs_indexed_key(key, sizeof(key), NVS_KEY_PASS_PREFIX, i);
        bool has_pass = nvs_read_string(h, key, item.pass, sizeof(item.pass));

        if (has_ssid && has_pass && saved_credentials_find(list, *out_count, item.ssid) < 0) {
            list[*out_count] = item;
            (*out_count)++;
        }
    }

    bool needs_commit = false;
    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    bool has_legacy_ssid = nvs_read_string(h, NVS_KEY_LEGACY_SSID, legacy_ssid, sizeof(legacy_ssid));
    bool has_legacy_pass = nvs_read_string(h, NVS_KEY_LEGACY_PASS, legacy_pass, sizeof(legacy_pass));

    if (has_legacy_ssid && has_legacy_pass) {
        if (saved_credentials_find(list, *out_count, legacy_ssid) < 0) {
            *out_count = saved_credentials_push_front(list, *out_count, legacy_ssid, legacy_pass);
            needs_commit = true;
            ESP_LOGI(TAG, "Migrated legacy WiFi credential: %s", legacy_ssid);
        }
    } else if (has_legacy_ssid) {
        ESP_LOGW(TAG, "Legacy WiFi is open or missing password, skip auto-connect: %s", legacy_ssid);
    }

    if (needs_commit) {
        err = nvs_store_saved_credentials(h, list, *out_count);
    }

    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Load saved WiFi credentials failed: %s", esp_err_to_name(err));
        return err;
    }

    if (*out_count > 0) {
        ESP_LOGI(TAG, "Loaded %u saved WiFi credential(s)", (unsigned int)*out_count);
    }

    return ESP_OK;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass == NULL || pass[0] == '\0') {
        ESP_LOGI(TAG, "Skip saving open WiFi credentials: %s", ssid);
        return ESP_ERR_INVALID_ARG;
    }

    wifi_saved_credential_t list[WIFI_SAVED_MAX];
    uint8_t count = 0;
    esp_err_t err = nvs_load_saved_credentials(list, &count);
    if (err != ESP_OK) {
        count = 0;
        memset(list, 0, sizeof(list));
    }

    count = saved_credentials_push_front(list, count, ssid, pass);

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_store_saved_credentials(h, list, count);
        nvs_close(h);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Save WiFi credentials failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved WiFi credential to NVS: %s (total=%u)",
                 ssid, (unsigned int)count);
    }

    return err;
}

static bool nvs_load_credentials(void)
{
    wifi_saved_credential_t list[WIFI_SAVED_MAX];
    uint8_t count = 0;

    if (nvs_load_saved_credentials(list, &count) != ESP_OK || count == 0) {
        s_current_ssid[0] = '\0';
        s_current_pass[0] = '\0';
        return false;
    }

    copy_string(s_current_ssid, sizeof(s_current_ssid), list[0].ssid);
    copy_string(s_current_pass, sizeof(s_current_pass), list[0].pass);
    return true;
}

static void set_state_after_scan(void)
{
    if (s_connecting) {
        s_state = WIFI_STATE_CONNECTING;
    } else if (s_connected) {
        s_state = WIFI_STATE_CONNECTED;
    } else {
        s_state = WIFI_STATE_IDLE;
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    notify_event(WIFI_EVT_SCAN_STARTED, NULL);

    uint16_t record_count = WIFI_SCAN_LIST_SIZE;
    uint16_t ap_total = 0;
    wifi_ap_record_t records[WIFI_SCAN_LIST_SIZE];
    memset(records, 0, sizeof(records));

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_num(&ap_total);
    }
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&record_count, records);
    }

    s_ap_count = 0;

    if (err == ESP_OK) {
        for (uint16_t i = 0; i < record_count && i < WIFI_SCAN_LIST_SIZE; i++) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }

            fill_ap_info(&s_ap_list[s_ap_count], &records[i]);

            if (s_connected && strcmp(s_ap_list[s_ap_count].ssid, s_current_ssid) == 0) {
                s_signal_level = s_ap_list[s_ap_count].signal;
            }

            s_ap_count++;
        }

        ESP_LOGI(TAG, "WiFi scan done: total=%u cached=%u", ap_total, s_ap_count);
    } else if (!s_scan_cancelled) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    }

    s_scanning = false;
    set_state_after_scan();

    s_scan_task_handle = NULL;

    if (err == ESP_OK) {
        notify_event(WIFI_EVT_SCAN_DONE, NULL);
    } else if (!s_scan_cancelled) {
        notify_event(WIFI_EVT_SCAN_FAILED, NULL);
    }

    vTaskDelete(NULL);
}

static bool connect_pending_blocking(bool notify_failure)
{
    if (s_pending_ssid[0] == '\0') {
        s_connecting = false;
        s_state = WIFI_STATE_DISCONNECTED;
        return false;
    }
    if (!wait_for_sta_started()) {
        s_connecting = false;
        s_state = WIFI_STATE_DISCONNECTED;
        clear_pending_credentials();
        if (notify_failure) {
            notify_event(WIFI_EVT_CONNECT_FAILED, NULL);
        }
        return false;
    }

    s_user_disconnect = false;
    s_connecting = true;
    s_state = WIFI_STATE_CONNECTING;
    notify_event(WIFI_EVT_CONNECTING, NULL);

    EventBits_t current_bits = xEventGroupGetBits(s_wifi_event_group);
    bool need_disconnect = s_connected || s_sta_associated ||
                           ((current_bits & WIFI_BIT_STA_CONNECTED) != 0);

    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_BIT_GOT_IP | WIFI_BIT_CONNECT_FAILED | WIFI_BIT_STA_DISCONNECTED);

    if (need_disconnect) {
        s_expected_disconnect = true;
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err == ESP_OK) {
            wait_for_disconnect_event();
        } else if (disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Disconnect before connect failed: %s", esp_err_to_name(disconnect_err));
        }
        s_expected_disconnect = false;
    }

    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_BIT_STA_CONNECTED | WIFI_BIT_GOT_IP | WIFI_BIT_CONNECT_FAILED |
                         WIFI_BIT_STA_DISCONNECTED);

    wifi_config_t cfg = {0};
    copy_wifi_field(cfg.sta.ssid, sizeof(cfg.sta.ssid), s_pending_ssid);
    copy_wifi_field(cfg.sta.password, sizeof(cfg.sta.password), s_pending_pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connecting to WiFi: %s", s_pending_ssid);
        err = esp_wifi_connect();
    }

    EventBits_t bits = 0;
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_BIT_GOT_IP | WIFI_BIT_CONNECT_FAILED,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
        );
    }

    if (err != ESP_OK || !(bits & WIFI_BIT_GOT_IP)) {
        ESP_LOGW(TAG, "Connect failed before IP: %s",
                 err == ESP_OK ? "timeout or disconnected" : esp_err_to_name(err));
        s_user_disconnect = true;
        s_connected = false;
        s_connecting = false;
        s_signal_level = WIFI_SIGNAL_NONE;
        s_state = WIFI_STATE_DISCONNECTED;
        clear_pending_credentials();
        s_expected_disconnect = true;
        esp_wifi_disconnect();
        wait_for_disconnect_event();
        s_expected_disconnect = false;
        if (notify_failure) {
            notify_event(WIFI_EVT_CONNECT_FAILED, NULL);
        }
        return false;
    }

    return true;
}

static void connect_task(void *arg)
{
    (void)arg;

    connect_pending_blocking(true);

    s_connect_task_handle = NULL;
    vTaskDelete(NULL);
}

static uint8_t build_auto_connect_order(const wifi_saved_credential_t *saved, uint8_t saved_count,
                                        wifi_saved_credential_t *ordered)
{
    bool seen[WIFI_SAVED_MAX] = {0};
    bool used[WIFI_SAVED_MAX] = {0};
    int8_t rssi[WIFI_SAVED_MAX];
    uint8_t ordered_count = 0;

    for (uint8_t i = 0; i < WIFI_SAVED_MAX; i++) {
        rssi[i] = -127;
    }

    uint16_t record_count = WIFI_SCAN_LIST_SIZE;
    wifi_ap_record_t records[WIFI_SCAN_LIST_SIZE];
    memset(records, 0, sizeof(records));

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&record_count, records);
    }

    if (err == ESP_OK) {
        for (uint16_t i = 0; i < record_count; i++) {
            char ssid[33] = {0};
            copy_string(ssid, sizeof(ssid), (const char *)records[i].ssid);
            int index = saved_credentials_find(saved, saved_count, ssid);
            if (index >= 0 && (!seen[index] || records[i].rssi > rssi[index])) {
                seen[index] = true;
                rssi[index] = records[i].rssi;
            }
        }
        ESP_LOGI(TAG, "Auto-connect scan done: %u AP record(s)", (unsigned)record_count);
    } else {
        ESP_LOGW(TAG, "Auto-connect scan failed, use saved order: %s", esp_err_to_name(err));
    }

    while (ordered_count < saved_count) {
        int best_index = -1;
        for (uint8_t i = 0; i < saved_count; i++) {
            if (seen[i] && !used[i] && (best_index < 0 || rssi[i] > rssi[best_index])) {
                best_index = i;
            }
        }
        if (best_index < 0) {
            break;
        }

        ordered[ordered_count++] = saved[best_index];
        used[best_index] = true;
    }

    for (uint8_t i = 0; i < saved_count && ordered_count < saved_count; i++) {
        if (!used[i]) {
            ordered[ordered_count++] = saved[i];
        }
    }

    return ordered_count;
}

static void auto_connect_task(void *arg)
{
    (void)arg;

    wifi_saved_credential_t saved[WIFI_SAVED_MAX];
    wifi_saved_credential_t ordered[WIFI_SAVED_MAX];
    uint8_t saved_count = 0;
    bool connected = false;

    memset(saved, 0, sizeof(saved));
    memset(ordered, 0, sizeof(ordered));

    if (nvs_load_saved_credentials(saved, &saved_count) != ESP_OK || saved_count == 0) {
        ESP_LOGW(TAG, "No saved encrypted WiFi credentials for auto-connect");
        goto done;
    }
    if (!wait_for_sta_started()) {
        ESP_LOGW(TAG, "Skip auto-connect: WiFi STA is not ready");
        goto done;
    }

    uint8_t ordered_count = build_auto_connect_order(saved, saved_count, ordered);
    for (uint8_t i = 0; i < ordered_count && !s_auto_connect_cancelled; i++) {
        ESP_LOGI(TAG, "Auto-connect WiFi %u/%u: %s",
                 (unsigned)(i + 1), (unsigned)ordered_count, ordered[i].ssid);
        copy_string(s_pending_ssid, sizeof(s_pending_ssid), ordered[i].ssid);
        copy_string(s_pending_pass, sizeof(s_pending_pass), ordered[i].pass);
        s_last_disconnect_reason = 0;

        if (connect_pending_blocking(false)) {
            connected = true;
            break;
        }

        s_user_disconnect = false;
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (!connected && !s_auto_connect_cancelled) {
        ESP_LOGW(TAG, "Auto-connect failed for all saved WiFi credentials");
        s_connecting = false;
        s_connected = false;
        s_signal_level = WIFI_SIGNAL_NONE;
        s_state = WIFI_STATE_DISCONNECTED;
        clear_pending_credentials();
        notify_event(WIFI_EVT_CONNECT_FAILED, NULL);
    }

done:
    if (!connected && !s_connected) {
        s_connecting = false;
        s_state = WIFI_STATE_DISCONNECTED;
    }
    s_auto_connect_task_handle = NULL;
    s_auto_connect_cancelled = false;
    vTaskDelete(NULL);
}

static bool start_auto_connect_saved(void)
{
    if (s_connect_task_handle != NULL || s_auto_connect_task_handle != NULL || s_connecting) {
        ESP_LOGW(TAG, "WiFi connection already in progress");
        return false;
    }

    wifi_saved_credential_t saved[WIFI_SAVED_MAX];
    uint8_t saved_count = 0;
    if (nvs_load_saved_credentials(saved, &saved_count) != ESP_OK || saved_count == 0) {
        return false;
    }

    copy_string(s_current_ssid, sizeof(s_current_ssid), saved[0].ssid);
    copy_string(s_current_pass, sizeof(s_current_pass), saved[0].pass);

    s_auto_connect_cancelled = false;
    s_state = WIFI_STATE_CONNECTING;
    BaseType_t ret = xTaskCreate(auto_connect_task, "wifi_auto_connect",
                                 WIFI_AUTO_CONNECT_TASK_STACK, NULL,
                                 WIFI_TASK_PRIORITY, &s_auto_connect_task_handle);
    if (ret != pdPASS) {
        s_auto_connect_task_handle = NULL;
        s_state = s_connected ? WIFI_STATE_CONNECTED : WIFI_STATE_DISCONNECTED;
        ESP_LOGE(TAG, "Failed to create WiFi auto-connect task");
        return false;
    }

    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_STARTED);
                break;
            case WIFI_EVENT_STA_STOP:
                ESP_LOGI(TAG, "WiFi STA stopped");
                s_connected = false;
                s_sta_associated = false;
                s_signal_level = WIFI_SIGNAL_NONE;
                s_state = WIFI_STATE_DISCONNECTED;
                xEventGroupClearBits(s_wifi_event_group,
                                     WIFI_BIT_STA_STARTED | WIFI_BIT_STA_CONNECTED | WIFI_BIT_GOT_IP);
                xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_DISCONNECTED);
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *)event_data;
                char connected_ssid[33] = {0};
                if (ev != NULL) {
                    size_t ssid_len = ev->ssid_len;
                    if (ssid_len > sizeof(connected_ssid) - 1) {
                        ssid_len = sizeof(connected_ssid) - 1;
                    }
                    memcpy(connected_ssid, ev->ssid, ssid_len);
                    connected_ssid[ssid_len] = '\0';
                }
                ESP_LOGI(TAG, "Associated with AP: %s", connected_ssid[0] != '\0' ? connected_ssid : "?");

                s_user_disconnect = false;
                s_sta_associated = true;
                s_state = WIFI_STATE_CONNECTING;
                xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_CONNECTED);

                if (s_pending_ssid[0] != '\0') {
                    copy_string(s_current_ssid, sizeof(s_current_ssid), s_pending_ssid);
                    copy_string(s_current_pass, sizeof(s_current_pass), s_pending_pass);
                } else if (connected_ssid[0] != '\0') {
                    copy_string(s_current_ssid, sizeof(s_current_ssid), connected_ssid);
                }
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
                bool expected = s_expected_disconnect;
                bool was_online = s_connected;
                bool was_connecting = s_connecting;
                uint8_t reason = ev != NULL ? ev->reason : 0;
                s_expected_disconnect = false;

                ESP_LOGI(TAG, "Disconnected from AP, reason=%u (%s)",
                         reason, disconnect_reason_to_name(reason));

                s_connected = false;
                s_sta_associated = false;
                s_signal_level = WIFI_SIGNAL_NONE;
                xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_STA_CONNECTED | WIFI_BIT_GOT_IP);
                xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_DISCONNECTED);

                if (was_online) {
                    notify_event(WIFI_EVT_DISCONNECTED, NULL);
                }

                if (was_connecting) {
                    if (!expected) {
                        s_last_disconnect_reason = reason;
                        xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_CONNECT_FAILED);
                    }
                    break;
                }
                if (expected) {
                    s_state = WIFI_STATE_DISCONNECTED;
                    break;
                }

                s_state = WIFI_STATE_DISCONNECTED;
                s_last_disconnect_reason = reason;
                if (!was_online) {
                    notify_event(WIFI_EVT_DISCONNECTED, NULL);
                }

                if (!s_user_disconnect && s_current_pass[0] != '\0') {
                    ESP_LOGI(TAG, "Trying saved WiFi auto-connect after disconnect");
                    start_auto_connect_saved();
                } else if (!s_user_disconnect && s_current_ssid[0] != '\0') {
                    ESP_LOGI(TAG, "Skip auto-reconnect for open WiFi: %s", s_current_ssid);
                }
                break;
            }
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGD(TAG, "WIFI_EVENT_SCAN_DONE");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));

        s_connected = true;
        s_connecting = false;
        s_user_disconnect = false;
        s_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_GOT_IP);

        wifi_ap_info_t info = {0};
        wifi_ap_record_t ap_record = {0};
        if (esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
            fill_ap_info(&info, &ap_record);
            s_signal_level = info.signal;
        }

        if (info.ssid[0] != '\0') {
            copy_string(s_current_ssid, sizeof(s_current_ssid), info.ssid);
        }

        if (info.ssid[0] == '\0') {
            copy_string(info.ssid, sizeof(info.ssid), s_current_ssid);
            info.signal = s_signal_level;
        }

        if (s_pending_ssid[0] != '\0') {
            copy_string(s_current_ssid, sizeof(s_current_ssid), s_pending_ssid);
            copy_string(s_current_pass, sizeof(s_current_pass), s_pending_pass);
            if (s_pending_pass[0] != '\0') {
                nvs_save_credentials(s_pending_ssid, s_pending_pass);
            } else {
                ESP_LOGI(TAG, "Connected to open WiFi, do not save for auto-connect: %s", s_pending_ssid);
            }
            clear_pending_credentials();
        }

        notify_event(WIFI_EVT_CONNECTED, &info);
        wifi_manager_start_sntp();
    }
}

void wifi_manager_init(void)
{
    if (s_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi manager...");

    s_wifi_event_group = xEventGroupCreate();
    assert(s_wifi_event_group != NULL);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif != NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    nvs_load_credentials();

    s_initialized = true;
    s_state = WIFI_STATE_IDLE;

    ESP_LOGI(TAG, "WiFi manager initialized. Stored SSID: %s",
             s_current_ssid[0] != '\0' ? s_current_ssid : "(none)");
    notify_event(WIFI_EVT_INIT_DONE, NULL);
}

void wifi_manager_set_event_callback(wifi_evt_cb_t cb)
{
    s_evt_cb = cb;
}

void wifi_manager_start_scan(void)
{
    if (!s_initialized || s_scan_task_handle != NULL || s_connecting ||
        s_auto_connect_task_handle != NULL) {
        return;
    }

    s_scan_cancelled = false;
    s_scanning = true;
    s_state = WIFI_STATE_SCANNING;
    s_ap_count = 0;

    BaseType_t ret = xTaskCreate(scan_task, "wifi_scan", WIFI_SCAN_TASK_STACK,
                                 NULL, WIFI_TASK_PRIORITY, &s_scan_task_handle);
    if (ret != pdPASS) {
        s_scan_task_handle = NULL;
        s_scanning = false;
        set_state_after_scan();
        ESP_LOGE(TAG, "Failed to create WiFi scan task");
        notify_event(WIFI_EVT_SCAN_FAILED, NULL);
    }
}

void wifi_manager_stop_scan(void)
{
    if (!s_scanning) {
        return;
    }

    s_scan_cancelled = true;
    s_scanning = false;
    esp_wifi_scan_stop();
    set_state_after_scan();
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

bool wifi_manager_is_scanning(void)
{
    return s_scanning;
}

uint16_t wifi_manager_get_ap_count(void)
{
    return s_ap_count;
}

bool wifi_manager_get_ap_info(uint16_t index, wifi_ap_info_t *out_info)
{
    if (out_info == NULL || index >= s_ap_count) {
        return false;
    }

    *out_info = s_ap_list[index];
    return true;
}

static bool start_connect(const char *ssid, const char *password)
{
    if (s_connect_task_handle != NULL || s_auto_connect_task_handle != NULL) {
        ESP_LOGW(TAG, "WiFi connection already in progress");
        return false;
    }

    copy_string(s_pending_ssid, sizeof(s_pending_ssid), ssid);
    s_last_disconnect_reason = 0;
    copy_string(s_pending_pass, sizeof(s_pending_pass), password);

    s_user_disconnect = false;
    s_connecting = true;
    s_state = WIFI_STATE_CONNECTING;

    BaseType_t ret = xTaskCreate(connect_task, "wifi_connect", WIFI_CONNECT_TASK_STACK,
                                 NULL, WIFI_TASK_PRIORITY, &s_connect_task_handle);
    if (ret != pdPASS) {
        s_connect_task_handle = NULL;
        s_connecting = false;
        s_state = WIFI_STATE_DISCONNECTED;
        clear_pending_credentials();
        ESP_LOGE(TAG, "Failed to create WiFi connect task");
        return false;
    }

    return true;
}

void wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_initialized || ssid == NULL || ssid[0] == '\0') {
        return;
    }
    if (password == NULL) {
        ESP_LOGW(TAG, "Refuse WiFi connect with NULL password");
        return;
    }

    start_connect(ssid, password);
}

bool wifi_manager_connect_saved(void)
{
    if (!s_initialized) {
        return false;
    }

    return start_auto_connect_saved();
}

void wifi_manager_disconnect(void)
{
    bool was_connected = s_connected;

    s_user_disconnect = true;
    s_auto_connect_cancelled = true;
    s_expected_disconnect = true;
    s_connecting = false;
    s_connected = false;
    s_sta_associated = false;
    s_signal_level = WIFI_SIGNAL_NONE;
    s_state = WIFI_STATE_DISCONNECTED;
    clear_pending_credentials();
    xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_STA_CONNECTED | WIFI_BIT_GOT_IP);
    xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_CONNECT_FAILED);
    xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_STA_DISCONNECTED);
    esp_wifi_disconnect();
    if (was_connected) {
        notify_event(WIFI_EVT_DISCONNECTED, NULL);
    }
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

wifi_signal_level_t wifi_manager_get_signal_level(void)
{
    if (s_connected) {
        wifi_ap_record_t ap_record = {0};
        if (esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
            s_signal_level = rssi_to_level(ap_record.rssi);
        }
    }

    return s_signal_level;
}

const char *wifi_manager_get_current_ssid(void)
{
    return s_current_ssid[0] != '\0' ? s_current_ssid : NULL;
}

uint8_t wifi_manager_get_last_disconnect_reason(void)
{
    return s_last_disconnect_reason;
}

const char *wifi_manager_get_last_disconnect_reason_name(void)
{
    return disconnect_reason_to_name(s_last_disconnect_reason);
}

void wifi_manager_register_ui(void)
{
}

bool wifi_manager_load_from_nvs(void)
{
    return nvs_load_credentials();
}

void wifi_manager_save_to_nvs(const char *ssid, const char *password)
{
    nvs_save_credentials(ssid, password);
}

void wifi_manager_start_sntp(void)
{
    static bool sntp_done = false;
    if (sntp_done) {
        return;
    }
    sntp_done = true;

    ESP_LOGI(TAG, "Starting SNTP...");
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

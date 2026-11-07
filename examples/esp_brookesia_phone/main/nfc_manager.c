/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nfc_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pn532_uart.h"

static const char *TAG = "nfc_manager";

#define NFC_UART_NUM              UART_NUM_1
#define NFC_UART_RX_GPIO          GPIO_NUM_4
#define NFC_UART_TX_GPIO          GPIO_NUM_5
#define NFC_TASK_STACK_SIZE       (1024 * 6)
#define NFC_TASK_PRIORITY         4
#define NFC_TASK_CORE             0
#define NFC_POLL_TIMEOUT_MS       1000
#define NFC_IDLE_DELAY_MS         50
#define NFC_EVENT_DEBOUNCE_MS     1500
#define NFC_STACK_LOG_INTERVAL_MS 5000
#define NFC_CONFIRM_REQUIRED_COUNT 1
#define NFC_CONFIRM_WINDOW_MS      1200

#define NFC_NVS_NAMESPACE         "nfc_users"
#define NFC_NVS_COUNT_KEY         "nfc_count"
#define NFC_MAX_CARDS             20

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    bool listening;
    bool parser_reset_requested;
    bool event_busy;
    TickType_t last_event_tick;
    char last_card_hex[NFC_CARD_HEX_LEN + 1];
    char candidate_card_hex[NFC_CARD_HEX_LEN + 1];
    uint8_t candidate_count;
    TickType_t candidate_tick;
    nfc_event_cb_t cb;
    void *user_ctx;
} nfc_state_t;

static nfc_state_t s_nfc;

static void nfc_task(void *arg);

static void lock_state(void)
{
    if (s_nfc.lock != NULL) {
        xSemaphoreTake(s_nfc.lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_nfc.lock != NULL) {
        xSemaphoreGive(s_nfc.lock);
    }
}

static void build_key(char *key, size_t key_size, const char *prefix, uint8_t index)
{
    snprintf(key, key_size, "%s_%u", prefix, index);
}

static void raw_to_hex(const uint8_t *raw, size_t raw_len, char *hex)
{
    static const char digits[] = "0123456789ABCDEF";

    for (size_t i = 0; i < raw_len; i++) {
        hex[i * 2] = digits[(raw[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = digits[raw[i] & 0x0F];
    }
    hex[raw_len * 2] = '\0';
}

static void reset_event_gate_locked(void)
{
    s_nfc.event_busy = false;
    s_nfc.last_event_tick = 0;
    s_nfc.last_card_hex[0] = '\0';
}

static void reset_candidate_locked(void)
{
    s_nfc.candidate_card_hex[0] = '\0';
    s_nfc.candidate_count = 0;
    s_nfc.candidate_tick = 0;
}

static void reset_candidate_state(void)
{
    lock_state();
    reset_candidate_locked();
    unlock_state();
}

static void reset_parser_state_locked(void)
{
    reset_event_gate_locked();
    reset_candidate_locked();
}

esp_err_t nfc_manager_normalize_card_hex(const char *src, char *dst, size_t dst_size,
                                         size_t *uid_len_out)
{
    if (src == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t src_len = strlen(src);
    // PN532 ISO14443A UIDs have one of the three standard lengths below.
    if ((src_len != 8 && src_len != 14 && src_len != 20) || dst_size <= src_len) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < src_len; i++) {
        if (!isxdigit((unsigned char)src[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[src_len] = '\0';
    if (uid_len_out != NULL) {
        *uid_len_out = src_len / 2;
    }
    return ESP_OK;
}

static esp_err_t get_card_count(nvs_handle_t handle, uint8_t *count)
{
    esp_err_t ret = nvs_get_u8(handle, NFC_NVS_COUNT_KEY, count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0;
        return ESP_OK;
    }

    if (ret == ESP_OK && *count > NFC_MAX_CARDS) {
        *count = NFC_MAX_CARDS;
    }
    return ret;
}

static esp_err_t get_str_key(nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t required = value_size;
    return nvs_get_str(handle, key, value, &required);
}

static esp_err_t set_card_slot_full(nvs_handle_t handle, uint8_t index, const char *card_hex,
                                    const char *employee_id, const char *name)
{
    char card_key[16];
    char emp_key[16];
    char name_key[16];
    build_key(card_key, sizeof(card_key), "nfc_card", index);
    build_key(emp_key, sizeof(emp_key), "nfc_emp", index);
    build_key(name_key, sizeof(name_key), "nfc_name", index);

    esp_err_t ret = nvs_set_str(handle, card_key, card_hex);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(handle, emp_key, employee_id != NULL ? employee_id : "");
    if (ret != ESP_OK) {
        return ret;
    }
    return nvs_set_str(handle, name_key, name);
}

static esp_err_t copy_card_slot(nvs_handle_t handle, uint8_t dst_index, uint8_t src_index)
{
    char src_card_key[16];
    char src_emp_key[16];
    char src_name_key[16];
    char card[NFC_CARD_HEX_LEN + 1];
    char employee_id[25] = {};
    char name[NFC_USER_NAME_MAX_LEN];

    build_key(src_card_key, sizeof(src_card_key), "nfc_card", src_index);
    build_key(src_emp_key, sizeof(src_emp_key), "nfc_emp", src_index);
    build_key(src_name_key, sizeof(src_name_key), "nfc_name", src_index);

    esp_err_t ret = get_str_key(handle, src_card_key, card, sizeof(card));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = get_str_key(handle, src_emp_key, employee_id, sizeof(employee_id));
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        employee_id[0] = '\0';
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = get_str_key(handle, src_name_key, name, sizeof(name));
    if (ret != ESP_OK) {
        return ret;
    }

    return set_card_slot_full(handle, dst_index, card, employee_id, name);
}

static esp_err_t erase_card_slot(nvs_handle_t handle, uint8_t index)
{
    static const char *prefixes[] = {"nfc_card", "nfc_emp", "nfc_name"};
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

static void log_stack_high_watermark(void)
{
    static TickType_t s_last_log_tick = 0;
    TickType_t now = xTaskGetTickCount();

    if (s_last_log_tick == 0 ||
        now - s_last_log_tick >= pdMS_TO_TICKS(NFC_STACK_LOG_INTERVAL_MS)) {
        s_last_log_tick = now;
        ESP_LOGI(TAG, "NFC task stack high watermark: %u words",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static bool is_listening(void)
{
    bool listening = false;

    lock_state();
    listening = s_nfc.listening;
    unlock_state();

    return listening;
}

static bool is_event_busy(void)
{
    bool busy = false;
    lock_state();
    busy = s_nfc.event_busy;
    unlock_state();
    return busy;
}

static bool take_parser_reset_request(void)
{
    bool requested = false;

    lock_state();
    requested = s_nfc.parser_reset_requested;
    s_nfc.parser_reset_requested = false;
    unlock_state();

    return requested;
}

static void reset_uart_input_queue(void)
{
    pn532_uart_flush_input();
}

static bool confirm_card_candidate(const char *card_hex)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t count = 0;
    bool confirmed = false;

    lock_state();
    bool same_candidate = s_nfc.candidate_card_hex[0] != '\0' &&
                          strcmp(s_nfc.candidate_card_hex, card_hex) == 0 &&
                          now - s_nfc.candidate_tick <= pdMS_TO_TICKS(NFC_CONFIRM_WINDOW_MS);
    if (!same_candidate) {
        snprintf(s_nfc.candidate_card_hex, sizeof(s_nfc.candidate_card_hex), "%s", card_hex);
        s_nfc.candidate_count = 1;
    } else if (s_nfc.candidate_count < NFC_CONFIRM_REQUIRED_COUNT) {
        s_nfc.candidate_count++;
    }
    s_nfc.candidate_tick = now;
    count = s_nfc.candidate_count;
    confirmed = count >= NFC_CONFIRM_REQUIRED_COUNT;
    if (confirmed) {
        reset_candidate_locked();
    }
    unlock_state();

    if (!confirmed) {
        ESP_LOGI(TAG, "NFC candidate %s count %u/%u",
                 card_hex, count, NFC_CONFIRM_REQUIRED_COUNT);
    }
    return confirmed;
}

static void emit_event(const nfc_event_t *event)
{
    nfc_event_cb_t cb = NULL;
    void *user_ctx = NULL;
    TickType_t now = xTaskGetTickCount();
    bool drop_busy = false;
    bool drop_debounce = false;
    bool drop_no_listener = false;

    lock_state();
    if (s_nfc.cb == NULL) {
        drop_no_listener = true;
    } else if (s_nfc.event_busy) {
        drop_busy = true;
    } else if (s_nfc.last_card_hex[0] != '\0' &&
               strcmp(s_nfc.last_card_hex, event->card_hex) == 0 &&
               now - s_nfc.last_event_tick < pdMS_TO_TICKS(NFC_EVENT_DEBOUNCE_MS)) {
        drop_debounce = true;
    } else {
        s_nfc.event_busy = true;
        s_nfc.last_event_tick = now;
        snprintf(s_nfc.last_card_hex, sizeof(s_nfc.last_card_hex), "%s", event->card_hex);
        cb = s_nfc.cb;
        user_ctx = s_nfc.user_ctx;
    }
    unlock_state();

    if (drop_no_listener) {
        ESP_LOGD(TAG, "Drop NFC card without listener: %s", event->card_hex);
        return;
    }
    if (drop_busy) {
        ESP_LOGW(TAG, "Drop NFC card while previous event is busy: %s", event->card_hex);
        return;
    }
    if (drop_debounce) {
        ESP_LOGI(TAG, "Drop NFC card debounce: %s", event->card_hex);
        return;
    }

    if (cb != NULL) {
        ESP_LOGI(TAG, "NFC card event queued: %s", event->card_hex);
        cb(event, user_ctx);
    }
}

static void handle_card_uid(const uint8_t *uid, size_t uid_len)
{
    if (uid == NULL || uid_len == 0 || uid_len > NFC_CARD_RAW_LEN) {
        return;
    }

    nfc_event_t event = {};
    memcpy(event.raw, uid, uid_len);
    event.raw_len = (uint8_t)uid_len;
    raw_to_hex(event.raw, uid_len, event.card_hex);
    ESP_LOGI(TAG, "PN532 card UID: %s (%u bytes)", event.card_hex,
             (unsigned)uid_len);
    if (confirm_card_candidate(event.card_hex)) {
        ESP_LOGI(TAG, "NFC card confirmed: %s", event.card_hex);
        emit_event(&event);
    }
    log_stack_high_watermark();
}

static void nfc_task(void *arg)
{
    (void)arg;

    while (true) {
        if (take_parser_reset_request()) {
            reset_uart_input_queue();
            reset_candidate_state();
        }

        if (!is_listening()) {
            vTaskDelay(pdMS_TO_TICKS(NFC_IDLE_DELAY_MS));
            continue;
        }
        if (is_event_busy()) {
            // A UID has been saved into an event and is being handled by the
            // login/binding flow. Resume polling only after it is acknowledged.
            vTaskDelay(pdMS_TO_TICKS(NFC_IDLE_DELAY_MS));
            continue;
        }

        uint8_t uid[PN532_UID_MAX_LEN];
        size_t uid_len = 0;
        esp_err_t ret = pn532_uart_read_passive_target(uid, sizeof(uid), &uid_len,
                                                       NFC_POLL_TIMEOUT_MS);
        if (ret == ESP_OK) {
            handle_card_uid(uid, uid_len);
        } else if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "PN532 card poll failed: %s", esp_err_to_name(ret));
            reset_candidate_state();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        log_stack_high_watermark();
    }
}

esp_err_t nfc_manager_init(void)
{
    if (s_nfc.initialized) {
        return ESP_OK;
    }

    if (s_nfc.lock == NULL) {
        s_nfc.lock = xSemaphoreCreateMutex();
        if (s_nfc.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = pn532_uart_init(NFC_UART_NUM, NFC_UART_TX_GPIO, NFC_UART_RX_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(nfc_task, "nfc_uart",
                                                  NFC_TASK_STACK_SIZE, NULL,
                                                  NFC_TASK_PRIORITY, &s_nfc.task,
                                                  NFC_TASK_CORE);
    if (task_ret != pdPASS) {
        pn532_uart_deinit();
        s_nfc.task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_nfc.initialized = true;
    ESP_LOGI(TAG, "PN532 NFC ready: UART%d RX=%d TX=%d",
             NFC_UART_NUM, NFC_UART_RX_GPIO, NFC_UART_TX_GPIO);
    return ESP_OK;
}

bool nfc_manager_is_ready(void)
{
    bool ready = false;
    lock_state();
    ready = s_nfc.initialized && s_nfc.task != NULL;
    unlock_state();
    return ready;
}

void nfc_manager_flush_input(void)
{
    if (!s_nfc.initialized) {
        return;
    }

    lock_state();
    reset_parser_state_locked();
    s_nfc.parser_reset_requested = true;
    unlock_state();

    reset_uart_input_queue();
    ESP_LOGI(TAG, "PN532 input flush requested");
}

void nfc_manager_ack_event(void)
{
    if (!s_nfc.initialized) {
        return;
    }

    lock_state();
    bool was_busy = s_nfc.event_busy;
    s_nfc.event_busy = false;
    unlock_state();

    ESP_LOGI(TAG, "NFC card event ack%s", was_busy ? "" : " (not busy)");
}

void nfc_manager_set_event_callback(nfc_event_cb_t cb, void *user_ctx)
{
    if (s_nfc.lock == NULL) {
        return;
    }

    bool enable_listening = cb != NULL;
    if (s_nfc.initialized && enable_listening) {
        reset_uart_input_queue();
    }

    lock_state();
    s_nfc.cb = cb;
    s_nfc.user_ctx = user_ctx;
    s_nfc.listening = enable_listening;
    s_nfc.parser_reset_requested = true;
    s_nfc.event_busy = false;
    reset_parser_state_locked();
    unlock_state();

    if (s_nfc.initialized && !enable_listening) {
        reset_uart_input_queue();
    }

    ESP_LOGI(TAG, "PN532 polling %s", enable_listening ? "enabled" : "disabled");
}

static esp_err_t save_card_binding_internal(const char *card_hex, const char *employee_id, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized_card[NFC_CARD_HEX_LEN + 1];
    esp_err_t ret = nfc_manager_normalize_card_hex(card_hex, normalized_card,
                                                   sizeof(normalized_card), NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(NFC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = get_card_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        char key[16];
        char saved_card[NFC_CARD_HEX_LEN + 1];
        build_key(key, sizeof(key), "nfc_card", i);
        if (get_str_key(handle, key, saved_card, sizeof(saved_card)) == ESP_OK &&
            strcmp(saved_card, normalized_card) == 0) {
            ret = set_card_slot_full(handle, i, normalized_card, employee_id, name);
            if (ret == ESP_OK) {
                ret = nvs_commit(handle);
            }
            nvs_close(handle);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "NFC card bound: %s -> %s", normalized_card, name);
            }
            return ret;
        }
    }

    if (count < NFC_MAX_CARDS) {
        ret = set_card_slot_full(handle, count, normalized_card, employee_id, name);
        if (ret == ESP_OK) {
            ret = nvs_set_u8(handle, NFC_NVS_COUNT_KEY, count + 1);
        }
    } else {
        for (uint8_t i = 1; i < NFC_MAX_CARDS && ret == ESP_OK; i++) {
            ret = copy_card_slot(handle, i - 1, i);
        }
        if (ret == ESP_OK) {
            ret = set_card_slot_full(handle, NFC_MAX_CARDS - 1, normalized_card, employee_id, name);
        }
        if (ret == ESP_OK) {
            ret = nvs_set_u8(handle, NFC_NVS_COUNT_KEY, NFC_MAX_CARDS);
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NFC card bound: %s -> %s", normalized_card, name);
    }
    return ret;
}

esp_err_t nfc_manager_save_card_user(const char *card_hex, const char *name)
{
    return save_card_binding_internal(card_hex, "", name);
}

esp_err_t nfc_manager_save_card_binding(const char *card_hex, const char *employee_id, const char *name)
{
    if (employee_id == NULL || employee_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return save_card_binding_internal(card_hex, employee_id, name);
}

esp_err_t nfc_manager_find_user(const char *card_hex, char *name, size_t name_size)
{
    if (name == NULL || name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    name[0] = '\0';

    char normalized_card[NFC_CARD_HEX_LEN + 1];
    esp_err_t ret = nfc_manager_normalize_card_hex(card_hex, normalized_card,
                                                   sizeof(normalized_card), NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "NFC card auth: %s", normalized_card);

    nvs_handle_t handle;
    ret = nvs_open(NFC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND;
        }
        return ret;
    }

    uint8_t count = 0;
    ret = get_card_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        char card_key[16];
        char name_key[16];
        char saved_card[NFC_CARD_HEX_LEN + 1];
        build_key(card_key, sizeof(card_key), "nfc_card", i);
        if (get_str_key(handle, card_key, saved_card, sizeof(saved_card)) != ESP_OK ||
            strcmp(saved_card, normalized_card) != 0) {
            continue;
        }

        build_key(name_key, sizeof(name_key), "nfc_name", i);
        ret = get_str_key(handle, name_key, name, name_size);
        nvs_close(handle);
        return ret;
    }

    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nfc_manager_delete_user_cards(const char *employee_id)
{
    if (employee_id == NULL || employee_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NFC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t count = 0;
    ret = get_card_count(handle, &count);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    bool removed = false;
    for (uint8_t i = 0; i < count;) {
        char emp_key[16];
        char saved_emp[25] = {};
        build_key(emp_key, sizeof(emp_key), "nfc_emp", i);
        ret = get_str_key(handle, emp_key, saved_emp, sizeof(saved_emp));
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
        if (ret != ESP_OK) {
            break;
        }
        if (strcmp(saved_emp, employee_id) != 0) {
            i++;
            continue;
        }

        for (uint8_t j = i + 1; j < count && ret == ESP_OK; j++) {
            ret = copy_card_slot(handle, j - 1, j);
        }
        if (ret == ESP_OK) {
            ret = erase_card_slot(handle, count - 1);
        }
        if (ret == ESP_OK) {
            count--;
            removed = true;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, NFC_NVS_COUNT_KEY, count);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK && !removed) {
        return ESP_ERR_NOT_FOUND;
    }
    return ret;
}

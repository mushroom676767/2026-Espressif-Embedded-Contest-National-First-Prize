#include "pn532_uart.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pn532_uart";

#define PN532_UART_BAUD_RATE       115200
#define PN532_UART_RX_BUF_SIZE     512
#define PN532_FRAME_MAX_DATA_LEN   64
#define PN532_ACK_TIMEOUT_MS       150
#define PN532_PN532_TO_HOST        0xD5
#define PN532_CMD_SAM_CONFIG       0x14
#define PN532_CMD_IN_LIST_TARGET   0x4A

static const uint8_t s_activation_frame[] = {
    0x55, 0x55,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD4, 0x14, 0x01, 0x17, 0x00,
};

static const uint8_t s_poll_frame[] = {
    0x00, 0x00, 0xFF, 0x04, 0xFC, 0xD4, 0x4A, 0x01, 0x00, 0xE1, 0x00,
};

typedef struct {
    uart_port_t uart_num;
    bool initialized;
} pn532_uart_state_t;

static pn532_uart_state_t s_pn532 = {
    .uart_num = UART_NUM_MAX,
};

static int uart_read_until(uint8_t *buf, size_t len, int64_t deadline_us)
{
    size_t offset = 0;
    while (offset < len) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return (int)offset;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }
        int read_len = uart_read_bytes(s_pn532.uart_num, buf + offset,
                                       len - offset, wait_ticks);
        if (read_len > 0) {
            offset += (size_t)read_len;
        }
    }
    return (int)offset;
}

static esp_err_t find_frame_preamble(int64_t deadline_us)
{
    static const uint8_t preamble[] = {0x00, 0x00, 0xFF};
    size_t matched = 0;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t byte = 0;
        if (uart_read_until(&byte, 1, deadline_us) != 1) {
            return ESP_ERR_TIMEOUT;
        }
        if (byte == preamble[matched]) {
            matched++;
            if (matched == sizeof(preamble)) {
                return ESP_OK;
            }
        } else {
            matched = byte == 0x00 ? 1 : 0;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_ack(uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t ret = find_frame_preamble(deadline_us);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t tail[3];
    if (uart_read_until(tail, sizeof(tail), deadline_us) != sizeof(tail)) {
        return ESP_ERR_TIMEOUT;
    }
    static const uint8_t expected[] = {0x00, 0xFF, 0x00};
    if (memcmp(tail, expected, sizeof(expected)) != 0) {
        ESP_LOGE(TAG, "Invalid PN532 ACK: %02X %02X %02X", tail[0], tail[1], tail[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_response(uint8_t expected_command, uint8_t *payload,
                               size_t payload_capacity, size_t *payload_len,
                               uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t ret = find_frame_preamble(deadline_us);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t length_fields[2];
    if (uart_read_until(length_fields, sizeof(length_fields), deadline_us) != sizeof(length_fields)) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t data_len = length_fields[0];
    if (data_len == 0xFF || (uint8_t)(data_len + length_fields[1]) != 0 ||
        data_len < 2 || data_len > PN532_FRAME_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Invalid PN532 response length: LEN=%u LCS=%02X",
                 data_len, length_fields[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t frame[PN532_FRAME_MAX_DATA_LEN + 2];
    size_t tail_len = (size_t)data_len + 2; // DATA + DCS + postamble
    if (uart_read_until(frame, tail_len, deadline_us) != (int)tail_len) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < data_len; i++) {
        sum = (uint8_t)(sum + frame[i]);
    }
    if ((uint8_t)(sum + frame[data_len]) != 0 || frame[data_len + 1] != 0x00) {
        ESP_LOGE(TAG, "PN532 response checksum failed");
        return ESP_ERR_INVALID_CRC;
    }
    if (frame[0] != PN532_PN532_TO_HOST || frame[1] != (uint8_t)(expected_command + 1)) {
        ESP_LOGE(TAG, "Unexpected PN532 response: TFI=%02X CMD=%02X",
                 frame[0], frame[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t response_payload_len = data_len - 2;
    if (response_payload_len > payload_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (response_payload_len > 0 && payload != NULL) {
        memcpy(payload, frame + 2, response_payload_len);
    }
    if (payload_len != NULL) {
        *payload_len = response_payload_len;
    }
    return ESP_OK;
}

static esp_err_t send_raw_frame(const uint8_t *frame, size_t frame_len,
                                uint8_t command, uint8_t *response,
                                size_t response_capacity, size_t *response_len,
                                uint32_t response_timeout_ms)
{
    if (!s_pn532.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame == NULL || frame_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_flush_input(s_pn532.uart_num);
    int written = uart_write_bytes(s_pn532.uart_num, frame, frame_len);
    if (written != (int)frame_len) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(s_pn532.uart_num, pdMS_TO_TICKS(100)),
                        TAG, "PN532 UART transmit timeout");
    esp_err_t ack_ret = read_ack(PN532_ACK_TIMEOUT_MS);
    if (ack_ret != ESP_OK) {
        return ack_ret;
    }
    return read_response(command, response, response_capacity, response_len,
                         response_timeout_ms);
}

esp_err_t pn532_uart_init(uart_port_t uart_num, gpio_num_t tx_gpio, gpio_num_t rx_gpio)
{
    if (s_pn532.initialized) {
        return ESP_OK;
    }

    const uart_config_t config = {
        .baud_rate = PN532_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &config), TAG, "Configure PN532 UART failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_num, tx_gpio, rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "Configure PN532 UART pins failed");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(rx_gpio, GPIO_PULLUP_ONLY),
                        TAG, "Configure PN532 RX pull-up failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, PN532_UART_RX_BUF_SIZE, 0,
                                            0, NULL, 0),
                        TAG, "Install PN532 UART failed");

    s_pn532.uart_num = uart_num;
    s_pn532.initialized = true;

    uint8_t response[8];
    size_t response_len = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t ret = send_raw_frame(s_activation_frame, sizeof(s_activation_frame),
                                   PN532_CMD_SAM_CONFIG, response, sizeof(response),
                                   &response_len, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 activation failed: %s", esp_err_to_name(ret));
        pn532_uart_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "PN532 activated on UART%d TX=%d RX=%d",
             uart_num, tx_gpio, rx_gpio);
    return ESP_OK;
}

void pn532_uart_deinit(void)
{
    if (!s_pn532.initialized) {
        return;
    }
    uart_driver_delete(s_pn532.uart_num);
    s_pn532.initialized = false;
    s_pn532.uart_num = UART_NUM_MAX;
}

void pn532_uart_flush_input(void)
{
    if (s_pn532.initialized) {
        uart_flush_input(s_pn532.uart_num);
    }
}

esp_err_t pn532_uart_read_passive_target(uint8_t *uid, size_t uid_capacity,
                                         size_t *uid_len, uint32_t timeout_ms)
{
    if (uid == NULL || uid_len == NULL || uid_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *uid_len = 0;

    uint8_t response[PN532_FRAME_MAX_DATA_LEN];
    size_t response_len = 0;
    esp_err_t ret = send_raw_frame(s_poll_frame, sizeof(s_poll_frame),
                                   PN532_CMD_IN_LIST_TARGET, response,
                                   sizeof(response), &response_len, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (response_len < 1 || response[0] == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (response_len < 6) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t detected_uid_len = response[5];
    if ((detected_uid_len != 4 && detected_uid_len != 7 && detected_uid_len != 10) ||
        detected_uid_len > uid_capacity || 6 + detected_uid_len > response_len) {
        ESP_LOGE(TAG, "Invalid PN532 UID length: %u", (unsigned)detected_uid_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(uid, response + 6, detected_uid_len);
    *uid_len = detected_uid_len;
    return ESP_OK;
}

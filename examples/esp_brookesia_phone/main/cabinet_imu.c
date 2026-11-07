#include "cabinet_imu.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CABINET_IMU_UART UART_NUM_2
#define CABINET_IMU_TX_GPIO GPIO_NUM_21
#define CABINET_IMU_RX_GPIO GPIO_NUM_22
#define CABINET_IMU_BAUD_RATE 115200
#define CABINET_IMU_RX_BUFFER_SIZE 512
#define CABINET_IMU_TASK_STACK_SIZE 4096
#define CABINET_IMU_TASK_PRIORITY 3
#define CABINET_IMU_QUERY_INTERVAL_MS 200
#define CABINET_IMU_READ_SLICE_MS 20
#define CABINET_IMU_RESPONSE_TIMEOUT_MS 500
#define CABINET_IMU_PREPARE_RETRY_MS 500

#define CABINET_IMU_FRAME_START 0x49
#define CABINET_IMU_FRAME_ADDRESS 0x00
#define CABINET_IMU_HOST_ADDRESS 0xFF
#define CABINET_IMU_FRAME_DATA_LENGTH 0x0D
#define CABINET_IMU_FRAME_END 0x4D
#define CABINET_IMU_FUNCTION_WAKE 0x03
#define CABINET_IMU_FUNCTION_IMU 0x11
#define CABINET_IMU_FUNCTION_GET_STATE 0x10
#define CABINET_IMU_FUNCTION_SET_PARAMS 0x12
#define CABINET_IMU_SUBSCRIPTION_EULER 0x0040
#define CABINET_IMU_FRAME_SIZE (3U + CABINET_IMU_FRAME_DATA_LENGTH + 2U)
#define CABINET_IMU_STREAM_CAPACITY 96
#define CABINET_IMU_PREAMBLE_SIZE 50U
#define CABINET_IMU_PREAMBLE_FF0_INDEX 47U
#define CABINET_IMU_PREAMBLE_FF1_INDEX 49U
#define CABINET_IMU_MAX_COMMAND_BODY_SIZE 33U
#define CABINET_IMU_MAX_PROTOCOL_FRAME_SIZE (3U + CABINET_IMU_MAX_COMMAND_BODY_SIZE + 2U)
#define CABINET_IMU_MAX_COMMAND_SIZE (CABINET_IMU_PREAMBLE_SIZE + CABINET_IMU_MAX_PROTOCOL_FRAME_SIZE)
#define CABINET_IMU_STATE_BODY_LENGTH 33U
#define CABINET_IMU_SET_PARAMS_BODY_LENGTH 11U
#define CABINET_IMU_STATE_SENSOR_FLAGS_INDEX 4U
#define CABINET_IMU_STATE_SUBSCRIPTION_INDEX 9U
#define CABINET_IMU_STATE_SENSOR_ENABLED_MASK (1U << 3)
#define CABINET_IMU_SET_PARAMS_SENSOR_CONFIG_MASK 0x07U

#define CABINET_IMU_ANGLE_SCALE_DEG (180.0f / 32768.0f)
#define CABINET_IMU_OPEN_DELTA_DEG 45.0f
#define CABINET_IMU_STABLE_RANGE_DEG 2.0f
#define CABINET_IMU_STABLE_TIME_US 1500000LL
#define CABINET_IMU_LOG_INTERVAL_US 1000000LL
#define CABINET_IMU_SAMPLE_MAX_AGE_US 1000000LL

static const char *TAG = "cabinet_imu";

/* The protocol body is wrapped with the documented 50-byte UART wake preamble. */
static const uint8_t s_query_body[] = {CABINET_IMU_FUNCTION_IMU};

typedef struct {
    uint32_t sensor_timestamp_ms;
    float angle_x_deg;
    float angle_y_deg;
    float angle_z_deg;
} imu_sample_t;

typedef struct {
    uint8_t data[CABINET_IMU_STREAM_CAPACITY];
    size_t length;
} imu_stream_parser_t;

typedef struct {
    bool active;
    int64_t started_at_us;
    float minimum;
    float maximum;
    float sum;
    uint32_t count;
} stability_window_t;

typedef struct {
    bool sample_valid;
    bool detection_active;
    bool raw_angle_valid;
    float last_raw_x_deg;
    float unwrapped_x_deg;
    float baseline_deg;
    float delta_deg;
    cabinet_imu_door_state_t state;
    stability_window_t stable;
} door_tracker_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    uint32_t detection_generation;
    uint32_t sample_sequence;
    int64_t received_at_us;
    int64_t last_log_at_us;
    imu_sample_t sample;
    imu_stream_parser_t parser;
    door_tracker_t tracker;
} cabinet_imu_context_t;

static cabinet_imu_context_t s_imu;

typedef void (*sample_sink_t)(const imu_sample_t *sample, void *ctx);

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint8_t frame_checksum(const uint8_t *frame)
{
    uint32_t sum = 0;
    size_t data_length = frame[2];
    for (size_t i = 1; i < 3U + data_length; ++i) {
        sum += frame[i];
    }
    return (uint8_t)sum;
}

static bool protocol_frame_valid(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < 5U || frame[0] != CABINET_IMU_FRAME_START) {
        return false;
    }
    size_t expected_length = 3U + frame[2] + 2U;
    return length == expected_length &&
           frame[expected_length - 1U] == CABINET_IMU_FRAME_END &&
           frame[expected_length - 2U] == frame_checksum(frame);
}

static size_t build_host_command(const uint8_t *body, size_t body_length,
                                 uint8_t *command, size_t command_capacity)
{
    size_t frame_length = 3U + body_length + 2U;
    size_t command_length = CABINET_IMU_PREAMBLE_SIZE + frame_length;
    if (body == NULL || body_length == 0 || body_length > CABINET_IMU_MAX_COMMAND_BODY_SIZE ||
        command == NULL || command_capacity < command_length) {
        return 0;
    }

    memset(command, 0, CABINET_IMU_PREAMBLE_SIZE);
    command[CABINET_IMU_PREAMBLE_FF0_INDEX] = 0xFF;
    command[CABINET_IMU_PREAMBLE_FF1_INDEX] = 0xFF;
    uint8_t *frame = command + CABINET_IMU_PREAMBLE_SIZE;
    frame[0] = CABINET_IMU_FRAME_START;
    frame[1] = CABINET_IMU_HOST_ADDRESS;
    frame[2] = (uint8_t)body_length;
    memcpy(&frame[3], body, body_length);
    frame[3U + body_length] = frame_checksum(frame);
    frame[4U + body_length] = CABINET_IMU_FRAME_END;
    return command_length;
}

static bool parse_frame(const uint8_t *frame, size_t length, imu_sample_t *sample_out)
{
    if (frame == NULL || sample_out == NULL || length != CABINET_IMU_FRAME_SIZE ||
        frame[0] != CABINET_IMU_FRAME_START ||
        frame[1] != CABINET_IMU_FRAME_ADDRESS ||
        frame[2] != CABINET_IMU_FRAME_DATA_LENGTH ||
        !protocol_frame_valid(frame, length)) {
        return false;
    }

    const uint8_t *body = &frame[3];
    if (body[0] != CABINET_IMU_FUNCTION_IMU ||
        read_u16_le(&body[1]) != CABINET_IMU_SUBSCRIPTION_EULER) {
        return false;
    }

    sample_out->sensor_timestamp_ms = read_u32_le(&body[3]);
    sample_out->angle_x_deg = (float)(int16_t)read_u16_le(&body[7]) * CABINET_IMU_ANGLE_SCALE_DEG;
    sample_out->angle_y_deg = (float)(int16_t)read_u16_le(&body[9]) * CABINET_IMU_ANGLE_SCALE_DEG;
    sample_out->angle_z_deg = (float)(int16_t)read_u16_le(&body[11]) * CABINET_IMU_ANGLE_SCALE_DEG;
    return true;
}

static void parser_consume(imu_stream_parser_t *parser, size_t count)
{
    if (count >= parser->length) {
        parser->length = 0;
        return;
    }
    memmove(parser->data, parser->data + count, parser->length - count);
    parser->length -= count;
}

static void parser_process(imu_stream_parser_t *parser, sample_sink_t sink, void *ctx)
{
    while (parser->length > 0) {
        size_t start = 0;
        while (start < parser->length && parser->data[start] != CABINET_IMU_FRAME_START) {
            ++start;
        }
        if (start > 0) {
            parser_consume(parser, start);
        }
        if (parser->length < 3) {
            return;
        }
        if (parser->data[1] != CABINET_IMU_FRAME_ADDRESS ||
            parser->data[2] != CABINET_IMU_FRAME_DATA_LENGTH) {
            parser_consume(parser, 1);
            continue;
        }
        if (parser->length < CABINET_IMU_FRAME_SIZE) {
            return;
        }

        imu_sample_t sample;
        if (parse_frame(parser->data, CABINET_IMU_FRAME_SIZE, &sample)) {
            if (sink != NULL) {
                sink(&sample, ctx);
            }
            parser_consume(parser, CABINET_IMU_FRAME_SIZE);
        } else {
            /* Drop only the candidate start byte so a following valid frame survives. */
            parser_consume(parser, 1);
        }
    }
}

static void parser_feed(imu_stream_parser_t *parser,
                        const uint8_t *data,
                        size_t length,
                        sample_sink_t sink,
                        void *ctx)
{
    while (length > 0) {
        if (parser->length == sizeof(parser->data)) {
            parser_consume(parser, 1);
        }
        size_t capacity = sizeof(parser->data) - parser->length;
        size_t copy_length = length < capacity ? length : capacity;
        memcpy(parser->data + parser->length, data, copy_length);
        parser->length += copy_length;
        data += copy_length;
        length -= copy_length;
        parser_process(parser, sink, ctx);
    }
}

static void stability_reset(stability_window_t *window)
{
    memset(window, 0, sizeof(*window));
}

static bool stability_feed(stability_window_t *window, float value, int64_t now_us)
{
    if (!window->active) {
        window->active = true;
        window->started_at_us = now_us;
        window->minimum = value;
        window->maximum = value;
        window->sum = value;
        window->count = 1;
        return false;
    }

    float minimum = value < window->minimum ? value : window->minimum;
    float maximum = value > window->maximum ? value : window->maximum;
    if (maximum - minimum > CABINET_IMU_STABLE_RANGE_DEG) {
        stability_reset(window);
        window->active = true;
        window->started_at_us = now_us;
        window->minimum = value;
        window->maximum = value;
        window->sum = value;
        window->count = 1;
        return false;
    }

    window->minimum = minimum;
    window->maximum = maximum;
    window->sum += value;
    window->count++;
    return now_us - window->started_at_us >= CABINET_IMU_STABLE_TIME_US;
}

static void tracker_start_open_detection(door_tracker_t *tracker)
{
    memset(tracker, 0, sizeof(*tracker));
    tracker->detection_active = true;
    tracker->state = CABINET_IMU_DOOR_BASELINING;
}

static void tracker_set_state(door_tracker_t *tracker, cabinet_imu_door_state_t state)
{
    if (tracker->state != state) {
        tracker->state = state;
        stability_reset(&tracker->stable);
    }
}

static void tracker_feed_sample(door_tracker_t *tracker, float raw_x_deg, int64_t now_us)
{
    if (!tracker->raw_angle_valid) {
        tracker->raw_angle_valid = true;
        tracker->last_raw_x_deg = raw_x_deg;
        tracker->unwrapped_x_deg = raw_x_deg;
    } else {
        float step = raw_x_deg - tracker->last_raw_x_deg;
        if (step > 180.0f) {
            step -= 360.0f;
        } else if (step < -180.0f) {
            step += 360.0f;
        }
        tracker->unwrapped_x_deg += step;
        tracker->last_raw_x_deg = raw_x_deg;
    }
    tracker->sample_valid = true;

    if (!tracker->detection_active) {
        return;
    }

    switch (tracker->state) {
        case CABINET_IMU_DOOR_BASELINING:
            /* The first legal sample is the closed reference for this operation. */
            tracker->baseline_deg = tracker->unwrapped_x_deg;
            tracker->delta_deg = 0.0f;
            tracker_set_state(tracker, CABINET_IMU_DOOR_OPENING);
            break;

        case CABINET_IMU_DOOR_OPENING:
            tracker->delta_deg = tracker->unwrapped_x_deg - tracker->baseline_deg;
            if (tracker->delta_deg >= CABINET_IMU_OPEN_DELTA_DEG) {
                if (stability_feed(&tracker->stable, tracker->unwrapped_x_deg, now_us)) {
                    tracker_set_state(tracker, CABINET_IMU_DOOR_FULLY_OPEN);
                }
            } else {
                stability_reset(&tracker->stable);
            }
            break;

        case CABINET_IMU_DOOR_FULLY_OPEN:
            tracker->delta_deg = tracker->unwrapped_x_deg - tracker->baseline_deg;
            break;

        case CABINET_IMU_DOOR_UNAVAILABLE:
        default:
            break;
    }
}

const char *cabinet_imu_state_name(cabinet_imu_door_state_t state)
{
    switch (state) {
        case CABINET_IMU_DOOR_BASELINING:
            return "baselining";
        case CABINET_IMU_DOOR_OPENING:
            return "opening";
        case CABINET_IMU_DOOR_FULLY_OPEN:
            return "fully_open";
        case CABINET_IMU_DOOR_UNAVAILABLE:
        default:
            return "unavailable";
    }
}

static void runtime_sample_sink(const imu_sample_t *sample, void *ctx)
{
    cabinet_imu_context_t *imu = (cabinet_imu_context_t *)ctx;
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(imu->lock, portMAX_DELAY);
    cabinet_imu_door_state_t previous_state = imu->tracker.state;
    imu->sample = *sample;
    imu->received_at_us = now_us;
    imu->sample_sequence++;
    tracker_feed_sample(&imu->tracker, sample->angle_x_deg, now_us);
    cabinet_imu_door_state_t current_state = imu->tracker.state;
    float unwrapped = imu->tracker.unwrapped_x_deg;
    float baseline = imu->tracker.baseline_deg;
    float delta = imu->tracker.delta_deg;
    bool log_sample = now_us - imu->last_log_at_us >= CABINET_IMU_LOG_INTERVAL_US;
    if (log_sample) {
        imu->last_log_at_us = now_us;
    }
    xSemaphoreGive(imu->lock);

    if (previous_state != current_state) {
        ESP_LOGI(TAG, "Door state: %s -> %s x=%.3f baseline=%.3f delta=%.3f",
                 cabinet_imu_state_name(previous_state), cabinet_imu_state_name(current_state),
                 unwrapped, baseline, delta);
    } else if (log_sample) {
        ESP_LOGI(TAG, "IMU t=%" PRIu32 " x=%.3f y=%.3f z=%.3f unwrapped=%.3f delta=%.3f state=%s",
                 sample->sensor_timestamp_ms, sample->angle_x_deg, sample->angle_y_deg,
                 sample->angle_z_deg, unwrapped, delta, cabinet_imu_state_name(current_state));
    }
}

typedef struct {
    int count;
    imu_sample_t last;
} self_test_sink_t;

static void self_test_sink(const imu_sample_t *sample, void *ctx)
{
    self_test_sink_t *sink = (self_test_sink_t *)ctx;
    sink->last = *sample;
    sink->count++;
}

static bool nearly_equal(float lhs, float rhs, float tolerance)
{
    return fabsf(lhs - rhs) <= tolerance;
}

static bool run_protocol_self_test(void)
{
    static const uint8_t example[] = {
        0x49, 0x00, 0x0D, 0x11, 0x40, 0x00, 0xBA, 0x76, 0x2B,
        0x00, 0xF6, 0xFF, 0x22, 0xFF, 0xA2, 0xAE, 0x1F, 0x4D,
    };
    static const uint8_t noise[] = {0x00, 0x49, 0x7E, 0x01, 0xAA};
    uint8_t bad_frame[sizeof(example)];
    memcpy(bad_frame, example, sizeof(bad_frame));
    bad_frame[sizeof(bad_frame) - 2] ^= 0x01;

    uint8_t query_command[CABINET_IMU_MAX_COMMAND_SIZE];
    size_t query_length = build_host_command(s_query_body, sizeof(s_query_body),
                                             query_command, sizeof(query_command));
    static const uint8_t expected_query_tail[] = {
        0x49, 0xFF, 0x01, 0x11, 0x11, 0x4D,
    };
    static const uint8_t wake_body[] = {CABINET_IMU_FUNCTION_WAKE};
    uint8_t wake_command[CABINET_IMU_MAX_COMMAND_SIZE];
    size_t wake_length = build_host_command(wake_body, sizeof(wake_body),
                                            wake_command, sizeof(wake_command));
    static const uint8_t expected_wake_tail[] = {
        0x49, 0xFF, 0x01, 0x03, 0x03, 0x4D,
    };
    bool command_layout_valid = query_length == 56U && wake_length == 56U &&
                                query_command[CABINET_IMU_PREAMBLE_FF0_INDEX] == 0xFF &&
                                query_command[CABINET_IMU_PREAMBLE_FF1_INDEX] == 0xFF &&
                                memcmp(query_command + CABINET_IMU_PREAMBLE_SIZE,
                                       expected_query_tail, sizeof(expected_query_tail)) == 0 &&
                                memcmp(wake_command + CABINET_IMU_PREAMBLE_SIZE,
                                       expected_wake_tail, sizeof(expected_wake_tail)) == 0;
    for (size_t i = 0; command_layout_valid && i < CABINET_IMU_PREAMBLE_SIZE; ++i) {
        uint8_t expected = (i == CABINET_IMU_PREAMBLE_FF0_INDEX ||
                            i == CABINET_IMU_PREAMBLE_FF1_INDEX) ? 0xFF : 0x00;
        command_layout_valid = query_command[i] == expected && wake_command[i] == expected;
    }

    imu_stream_parser_t parser = {0};
    self_test_sink_t sink = {0};
    parser_feed(&parser, noise, sizeof(noise), self_test_sink, &sink);
    parser_feed(&parser, example, 5, self_test_sink, &sink);
    parser_feed(&parser, example + 5, sizeof(example) - 5, self_test_sink, &sink);
    parser_feed(&parser, bad_frame, sizeof(bad_frame), self_test_sink, &sink);
    uint8_t joined[sizeof(example) * 2];
    memcpy(joined, example, sizeof(example));
    memcpy(joined + sizeof(example), example, sizeof(example));
    parser_feed(&parser, joined, sizeof(joined), self_test_sink, &sink);

    return command_layout_valid && sink.count == 3 &&
           sink.last.sensor_timestamp_ms == 2848442U &&
           nearly_equal(sink.last.angle_x_deg, -0.0549316f, 0.0001f) &&
           nearly_equal(sink.last.angle_y_deg, -1.2194824f, 0.0001f) &&
           nearly_equal(sink.last.angle_z_deg, -114.4226074f, 0.0001f);
}

static bool run_tracker_self_test(void)
{
    door_tracker_t tracker = {0};
    tracker_start_open_detection(&tracker);
    tracker_feed_sample(&tracker, 0.0f, 0);
    if (tracker.state != CABINET_IMU_DOOR_OPENING ||
        !nearly_equal(tracker.baseline_deg, 0.0f, 0.001f)) {
        return false;
    }

    for (int i = 1; i <= 9; ++i) {
        tracker_feed_sample(&tracker, 44.0f, (int64_t)i * 200000);
    }
    if (tracker.state != CABINET_IMU_DOOR_OPENING) {
        return false;
    }
    /* A large transient must reset the stability window. */
    tracker_feed_sample(&tracker, 48.0f, 2000000);
    tracker_feed_sample(&tracker, 45.5f, 2200000);
    for (int i = 1; i <= 8; ++i) {
        tracker_feed_sample(&tracker, 45.5f + ((i & 1) ? 0.2f : 0.0f),
                            2200000 + (int64_t)i * 200000);
    }
    return tracker.state == CABINET_IMU_DOOR_FULLY_OPEN;
}

static bool detection_session_active(cabinet_imu_context_t *imu, uint32_t generation)
{
    xSemaphoreTake(imu->lock, portMAX_DELAY);
    bool active = imu->tracker.detection_active &&
                  imu->detection_generation == generation;
    xSemaphoreGive(imu->lock);
    return active;
}

static esp_err_t write_host_command(const uint8_t *body, size_t body_length)
{
    uint8_t command[CABINET_IMU_MAX_COMMAND_SIZE];
    size_t command_length = build_host_command(body, body_length, command, sizeof(command));
    if (command_length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(CABINET_IMU_UART, command, command_length);
    if (written != (int)command_length) {
        ESP_LOGW(TAG, "IMU command 0x%02X write incomplete: %d/%u",
                 body[0], written, (unsigned int)command_length);
        return ESP_FAIL;
    }
    return uart_wait_tx_done(CABINET_IMU_UART, pdMS_TO_TICKS(50));
}

static bool take_expected_response(imu_stream_parser_t *parser, uint8_t expected_function,
                                   uint8_t *response, size_t response_capacity,
                                   size_t *response_length)
{
    while (parser->length > 0) {
        size_t start = 0;
        while (start < parser->length && parser->data[start] != CABINET_IMU_FRAME_START) {
            ++start;
        }
        if (start > 0) {
            parser_consume(parser, start);
        }
        if (parser->length < 3U) {
            return false;
        }

        size_t frame_length = 3U + parser->data[2] + 2U;
        if (frame_length > sizeof(parser->data)) {
            parser_consume(parser, 1);
            continue;
        }
        if (parser->length < frame_length) {
            return false;
        }

        bool expected = protocol_frame_valid(parser->data, frame_length) &&
                        parser->data[1] == CABINET_IMU_FRAME_ADDRESS &&
                        parser->data[2] > 0 &&
                        parser->data[3] == expected_function;
        if (expected) {
            if (response == NULL || response_length == NULL || response_capacity < frame_length) {
                return false;
            }
            memcpy(response, parser->data, frame_length);
            *response_length = frame_length;
            parser_consume(parser, frame_length);
            return true;
        }

        /* Consume a complete unrelated/invalid frame and keep looking for the reply. */
        parser_consume(parser, protocol_frame_valid(parser->data, frame_length) ? frame_length : 1U);
    }
    return false;
}

static esp_err_t command_exchange(cabinet_imu_context_t *imu, uint32_t generation,
                                  const uint8_t *body, size_t body_length,
                                  uint8_t expected_function,
                                  uint8_t *response, size_t response_capacity,
                                  size_t *response_length)
{
    ESP_RETURN_ON_ERROR(write_host_command(body, body_length), TAG,
                        "Write IMU command 0x%02X failed", body[0]);

    imu_stream_parser_t response_parser = {0};
    uint8_t rx_buffer[64];
    int64_t deadline_us = esp_timer_get_time() +
                          (int64_t)CABINET_IMU_RESPONSE_TIMEOUT_MS * 1000LL;
    while (detection_session_active(imu, generation) && esp_timer_get_time() < deadline_us) {
        int received = uart_read_bytes(CABINET_IMU_UART, rx_buffer, sizeof(rx_buffer),
                                       pdMS_TO_TICKS(CABINET_IMU_READ_SLICE_MS));
        if (received <= 0) {
            continue;
        }

        size_t offset = 0;
        while (offset < (size_t)received) {
            if (response_parser.length == sizeof(response_parser.data)) {
                parser_consume(&response_parser, 1);
            }
            size_t capacity = sizeof(response_parser.data) - response_parser.length;
            size_t copy_length = (size_t)received - offset;
            if (copy_length > capacity) {
                copy_length = capacity;
            }
            memcpy(response_parser.data + response_parser.length, rx_buffer + offset, copy_length);
            response_parser.length += copy_length;
            offset += copy_length;
            if (take_expected_response(&response_parser, expected_function,
                                       response, response_capacity, response_length)) {
                return ESP_OK;
            }
        }
    }

    if (!detection_session_active(imu, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "IMU command 0x%02X response timeout", body[0]);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_sensor_state(cabinet_imu_context_t *imu, uint32_t generation,
                                   uint8_t *response, size_t response_capacity,
                                   size_t *response_length)
{
    static const uint8_t body[] = {CABINET_IMU_FUNCTION_GET_STATE};
    esp_err_t ret = command_exchange(imu, generation, body, sizeof(body),
                                     CABINET_IMU_FUNCTION_GET_STATE,
                                     response, response_capacity, response_length);
    if (ret != ESP_OK) {
        return ret;
    }
    if (*response_length != 3U + CABINET_IMU_STATE_BODY_LENGTH + 2U ||
        response[2] != CABINET_IMU_STATE_BODY_LENGTH) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t prepare_sensor_for_euler(cabinet_imu_context_t *imu, uint32_t generation)
{
    uint8_t response[CABINET_IMU_MAX_PROTOCOL_FRAME_SIZE];
    size_t response_length = 0;
    static const uint8_t wake_body[] = {CABINET_IMU_FUNCTION_WAKE};

    ESP_RETURN_ON_ERROR(command_exchange(imu, generation, wake_body, sizeof(wake_body),
                                         CABINET_IMU_FUNCTION_WAKE,
                                         response, sizeof(response), &response_length),
                        TAG, "Wake IMU failed");
    if (response_length != 6U || response[2] != 1U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(read_sensor_state(imu, generation, response, sizeof(response),
                                          &response_length),
                        TAG, "Read IMU state failed");
    uint8_t *state_body = &response[3];
    uint16_t subscription = read_u16_le(&state_body[CABINET_IMU_STATE_SUBSCRIPTION_INDEX]);
    bool sensor_enabled =
        (state_body[CABINET_IMU_STATE_SENSOR_FLAGS_INDEX] & CABINET_IMU_STATE_SENSOR_ENABLED_MASK) != 0;
    ESP_LOGI(TAG, "IMU wake acknowledged: enabled=%d subscription=0x%04X",
             sensor_enabled, subscription);
    if (!sensor_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (subscription == CABINET_IMU_SUBSCRIPTION_EULER) {
        return ESP_OK;
    }

    uint8_t set_body[CABINET_IMU_SET_PARAMS_BODY_LENGTH] = {0};
    set_body[0] = CABINET_IMU_FUNCTION_SET_PARAMS;
    memcpy(&set_body[1], &state_body[1], CABINET_IMU_SET_PARAMS_BODY_LENGTH - 1U);
    /* Status bits 3/4 are read-only state; only bits 0-2 belong in command 0x12. */
    set_body[CABINET_IMU_STATE_SENSOR_FLAGS_INDEX] &= CABINET_IMU_SET_PARAMS_SENSOR_CONFIG_MASK;
    set_body[CABINET_IMU_STATE_SUBSCRIPTION_INDEX] =
        (uint8_t)(CABINET_IMU_SUBSCRIPTION_EULER & 0xFFU);
    set_body[CABINET_IMU_STATE_SUBSCRIPTION_INDEX + 1U] =
        (uint8_t)(CABINET_IMU_SUBSCRIPTION_EULER >> 8);
    ESP_RETURN_ON_ERROR(command_exchange(imu, generation, set_body, sizeof(set_body),
                                         CABINET_IMU_FUNCTION_SET_PARAMS,
                                         response, sizeof(response), &response_length),
                        TAG, "Set Euler subscription failed");
    if (response_length != 6U || response[2] != 1U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(read_sensor_state(imu, generation, response, sizeof(response),
                                          &response_length),
                        TAG, "Verify IMU subscription failed");
    state_body = &response[3];
    subscription = read_u16_le(&state_body[CABINET_IMU_STATE_SUBSCRIPTION_INDEX]);
    if (subscription != CABINET_IMU_SUBSCRIPTION_EULER) {
        ESP_LOGE(TAG, "Euler subscription verification failed: 0x%04X", subscription);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Euler subscription configured: 0x%04X", subscription);
    return ESP_OK;
}

static void cabinet_imu_task(void *arg)
{
    cabinet_imu_context_t *imu = (cabinet_imu_context_t *)arg;
    uint8_t rx_buffer[128];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(imu->lock, portMAX_DELAY);
        bool active = imu->tracker.detection_active;
        uint32_t generation = imu->detection_generation;
        xSemaphoreGive(imu->lock);
        if (!active) {
            continue;
        }

        (void)uart_flush_input(CABINET_IMU_UART);
        ESP_LOGI(TAG, "IMU preparation started");
        esp_err_t prepare_ret = ESP_FAIL;
        while (detection_session_active(imu, generation)) {
            prepare_ret = prepare_sensor_for_euler(imu, generation);
            if (prepare_ret == ESP_OK) {
                break;
            }
            if (prepare_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "IMU preparation failed (%s); retrying",
                         esp_err_to_name(prepare_ret));
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CABINET_IMU_PREPARE_RETRY_MS));
            (void)uart_flush_input(CABINET_IMU_UART);
        }
        if (!detection_session_active(imu, generation)) {
            ESP_LOGI(TAG, "IMU query loop stopped during preparation");
            continue;
        }

        memset(&imu->parser, 0, sizeof(imu->parser));
        (void)uart_flush_input(CABINET_IMU_UART);
        ESP_LOGI(TAG, "IMU query loop activated at %u Hz",
                 (unsigned int)(1000U / CABINET_IMU_QUERY_INTERVAL_MS));

        while (detection_session_active(imu, generation)) {
            TickType_t cycle_started = xTaskGetTickCount();
            esp_err_t write_ret = write_host_command(s_query_body, sizeof(s_query_body));
            if (write_ret != ESP_OK) {
                ESP_LOGW(TAG, "IMU query write failed: %s", esp_err_to_name(write_ret));
            }

            TickType_t deadline = cycle_started + pdMS_TO_TICKS(CABINET_IMU_QUERY_INTERVAL_MS);
            while (detection_session_active(imu, generation) &&
                   (int32_t)(deadline - xTaskGetTickCount()) > 0) {
                int received = uart_read_bytes(CABINET_IMU_UART, rx_buffer, sizeof(rx_buffer),
                                               pdMS_TO_TICKS(CABINET_IMU_READ_SLICE_MS));
                if (received > 0) {
                    parser_feed(&imu->parser, rx_buffer, (size_t)received,
                                runtime_sample_sink, imu);
                }
            }
            vTaskDelayUntil(&cycle_started, pdMS_TO_TICKS(CABINET_IMU_QUERY_INTERVAL_MS));
        }
        ESP_LOGI(TAG, "IMU query loop stopped");
    }
}

esp_err_t cabinet_imu_init(void)
{
    if (s_imu.initialized) {
        return ESP_OK;
    }
    if (!run_protocol_self_test() || !run_tracker_self_test()) {
        ESP_LOGE(TAG, "IMU protocol/state self-test failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "IMU protocol/state self-test passed");

    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.lock = xSemaphoreCreateMutex();
    if (s_imu.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_imu.tracker.state = CABINET_IMU_DOOR_UNAVAILABLE;

    const uart_config_t config = {
        .baud_rate = CABINET_IMU_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(CABINET_IMU_UART, &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(CABINET_IMU_UART, CABINET_IMU_TX_GPIO, CABINET_IMU_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (ret == ESP_OK) {
        ret = uart_driver_install(CABINET_IMU_UART, CABINET_IMU_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    }
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_imu.lock);
        memset(&s_imu, 0, sizeof(s_imu));
        ESP_LOGE(TAG, "Initialize UART2 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_imu.initialized = true;
    BaseType_t task_ret = xTaskCreate(cabinet_imu_task, "cabinet_imu",
                                      CABINET_IMU_TASK_STACK_SIZE, &s_imu,
                                      CABINET_IMU_TASK_PRIORITY, &s_imu.task);
    if (task_ret != pdPASS) {
        uart_driver_delete(CABINET_IMU_UART);
        vSemaphoreDelete(s_imu.lock);
        memset(&s_imu, 0, sizeof(s_imu));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Cabinet IMU ready and idle: UART2 TX=%d RX=%d baud=%d query=%ums",
             CABINET_IMU_TX_GPIO, CABINET_IMU_RX_GPIO, CABINET_IMU_BAUD_RATE,
             CABINET_IMU_QUERY_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t cabinet_imu_start_open_detection(void)
{
    if (!s_imu.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_imu.lock, portMAX_DELAY);
    if (s_imu.tracker.detection_active) {
        xSemaphoreGive(s_imu.lock);
        return ESP_ERR_INVALID_STATE;
    }
    tracker_start_open_detection(&s_imu.tracker);
    s_imu.received_at_us = 0;
    s_imu.last_log_at_us = 0;
    s_imu.detection_generation++;
    xSemaphoreGive(s_imu.lock);
    xTaskNotifyGive(s_imu.task);
    ESP_LOGI(TAG, "Open detection started; first legal X angle will be the baseline");
    return ESP_OK;
}

void cabinet_imu_stop_open_detection(void)
{
    if (!s_imu.initialized) {
        return;
    }
    xSemaphoreTake(s_imu.lock, portMAX_DELAY);
    s_imu.tracker.detection_active = false;
    s_imu.tracker.state = CABINET_IMU_DOOR_UNAVAILABLE;
    s_imu.tracker.sample_valid = false;
    stability_reset(&s_imu.tracker.stable);
    s_imu.detection_generation++;
    xSemaphoreGive(s_imu.lock);
    xTaskNotifyGive(s_imu.task);
    ESP_LOGI(TAG, "Open detection stopped");
}

esp_err_t cabinet_imu_get_status(cabinet_imu_status_t *status_out)
{
    if (status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status_out, 0, sizeof(*status_out));
    if (!s_imu.initialized) {
        status_out->door_state = CABINET_IMU_DOOR_UNAVAILABLE;
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_imu.lock, portMAX_DELAY);
    bool sample_fresh = s_imu.tracker.sample_valid &&
                        s_imu.received_at_us > 0 &&
                        now_us >= s_imu.received_at_us &&
                        now_us - s_imu.received_at_us <= CABINET_IMU_SAMPLE_MAX_AGE_US;
    status_out->initialized = s_imu.initialized;
    status_out->sample_valid = sample_fresh;
    status_out->detection_active = s_imu.tracker.detection_active;
    status_out->door_state = sample_fresh ? s_imu.tracker.state : CABINET_IMU_DOOR_UNAVAILABLE;
    status_out->sample_sequence = s_imu.sample_sequence;
    status_out->sensor_timestamp_ms = s_imu.sample.sensor_timestamp_ms;
    status_out->received_at_us = s_imu.received_at_us;
    status_out->angle_x_deg = s_imu.sample.angle_x_deg;
    status_out->angle_y_deg = s_imu.sample.angle_y_deg;
    status_out->angle_z_deg = s_imu.sample.angle_z_deg;
    status_out->angle_x_unwrapped_deg = s_imu.tracker.unwrapped_x_deg;
    status_out->closed_baseline_deg = s_imu.tracker.baseline_deg;
    status_out->angle_x_delta_deg = s_imu.tracker.delta_deg;
    xSemaphoreGive(s_imu.lock);
    return ESP_OK;
}

#include "cabinet_door_signal.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cabinet_door_signal";

// GPIO48 is exposed on J1 and is unused by the enabled BSP peripherals.
// GPIO20 is reserved exclusively for USB-OTG D+.
#define CABINET_DOOR_OUTPUT GPIO_NUM_48
#define CABINET_DOOR_GPIO_MASK (1ULL << CABINET_DOOR_OUTPUT)
#define CABINET_DOOR_UNLOCK_PULSE_US (5LL * 1000LL * 1000LL)
#define CABINET_DOOR_CLOSE_PULSE_US (2LL * 1000LL * 1000LL)

typedef enum {
    CABINET_DOOR_PULSE_NONE = 0,
    CABINET_DOOR_PULSE_UNLOCK,
    CABINET_DOOR_PULSE_CLOSE,
} cabinet_door_pulse_t;

static bool s_initialized;
static bool s_pulse_active;
static cabinet_door_pulse_t s_active_pulse;
static int64_t s_pulse_deadline_us;
static esp_timer_handle_t s_unlock_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t set_output_level(uint32_t level)
{
    esp_err_t ret = gpio_set_level(CABINET_DOOR_OUTPUT, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set GPIO48=%u failed: %s", (unsigned int)level,
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Door output: GPIO48=%u", (unsigned int)level);
    return ESP_OK;
}

static void unlock_timeout_cb(void *arg)
{
    (void)arg;
    bool should_lock = false;
    cabinet_door_pulse_t expired_pulse = CABINET_DOOR_PULSE_NONE;
    esp_err_t gpio_ret = ESP_OK;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (s_pulse_active && now_us >= s_pulse_deadline_us) {
        s_pulse_active = false;
        expired_pulse = s_active_pulse;
        s_active_pulse = CABINET_DOOR_PULSE_NONE;
        s_pulse_deadline_us = 0;
        gpio_ret = gpio_set_level(CABINET_DOOR_OUTPUT, 0);
        should_lock = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (should_lock) {
        if (gpio_ret == ESP_OK) {
            ESP_LOGI(TAG, "Door output: GPIO48=0");
            ESP_LOGI(TAG, "Door %s pulse completed",
                     expired_pulse == CABINET_DOOR_PULSE_CLOSE ? "close" : "unlock");
        } else {
            ESP_LOGE(TAG, "Set GPIO48=0 after pulse failed: %s", esp_err_to_name(gpio_ret));
        }
    }
}

static esp_err_t start_pulse(cabinet_door_pulse_t pulse, int64_t duration_us)
{
    if (!s_initialized || s_unlock_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (s_pulse_active) {
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "Door pulse already active; refusing to extend it");
        return ESP_ERR_INVALID_STATE;
    }
    s_pulse_active = true;
    s_active_pulse = pulse;
    s_pulse_deadline_us = esp_timer_get_time() + duration_us;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret = set_output_level(1);
    if (ret == ESP_OK) {
        ret = esp_timer_start_once(s_unlock_timer, duration_us);
    }
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_pulse_active = false;
        s_active_pulse = CABINET_DOOR_PULSE_NONE;
        s_pulse_deadline_us = 0;
        portEXIT_CRITICAL(&s_state_lock);
        (void)set_output_level(0);
        ESP_LOGE(TAG, "Start door pulse failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Door %s pulse started: duration=%lld ms",
             pulse == CABINET_DOOR_PULSE_CLOSE ? "close" : "unlock",
             duration_us / 1000LL);
    return ESP_OK;
}

esp_err_t cabinet_door_signal_unlock_pulse(void)
{
    return start_pulse(CABINET_DOOR_PULSE_UNLOCK, CABINET_DOOR_UNLOCK_PULSE_US);
}

esp_err_t cabinet_door_signal_close_pulse(void)
{
    return start_pulse(CABINET_DOOR_PULSE_CLOSE, CABINET_DOOR_CLOSE_PULSE_US);
}

esp_err_t cabinet_door_signal_lock(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_unlock_timer != NULL) {
        esp_err_t stop_ret = esp_timer_stop(s_unlock_timer);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Stop unlock timer failed: %s", esp_err_to_name(stop_ret));
        }
    }
    esp_err_t gpio_ret;
    portENTER_CRITICAL(&s_state_lock);
    s_pulse_active = false;
    s_active_pulse = CABINET_DOOR_PULSE_NONE;
    s_pulse_deadline_us = 0;
    gpio_ret = gpio_set_level(CABINET_DOOR_OUTPUT, 0);
    portEXIT_CRITICAL(&s_state_lock);
    if (gpio_ret != ESP_OK) {
        ESP_LOGE(TAG, "Set GPIO48=0 failed: %s", esp_err_to_name(gpio_ret));
        return gpio_ret;
    }
    ESP_LOGI(TAG, "Door output: GPIO48=0");
    return ESP_OK;
}

esp_err_t cabinet_door_signal_init(void)
{
    if (s_initialized) {
        return cabinet_door_signal_lock();
    }

    // Preload the output latch low before enabling the pad as an output.
    gpio_set_level(CABINET_DOOR_OUTPUT, 0);

    const gpio_config_t config = {
        .pin_bit_mask = CABINET_DOOR_GPIO_MASK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configure GPIO48 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = unlock_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "door_unlock",
        .skip_unhandled_events = false,
    };
    ret = esp_timer_create(&timer_args, &s_unlock_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create door unlock timer failed: %s", esp_err_to_name(ret));
        (void)set_output_level(0);
        return ret;
    }

    s_initialized = true;
    return cabinet_door_signal_lock();
}

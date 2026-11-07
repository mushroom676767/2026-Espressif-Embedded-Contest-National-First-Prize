/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_manager.h"
#include "ui_theme.h"
#include "screen_standby.h"
#include "screen_auth.h"
#include "screen_usb_preview.h"
#include "screen_operation.h"
#include "screen_result.h"
#include "screen_wifi.h"
#include "screen_register.h"
#include "screen_admin.h"
#include "screen_tool_records.h"
#include "cabinet_core.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

static const char *TAG = "ui_manager";

static ui_screen_id_t s_current_screen = SCREEN_MAX;

// ============================================================================
// Screen Initialization
// ============================================================================

void ui_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing UI manager...");

    // Lock LVGL for thread-safe access
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));

    // Create all screens (they won't be displayed until shown)
    screen_standby_create();
    screen_auth_create();
    screen_usb_preview_create();
    screen_operation_create();
    screen_result_create();
    screen_wifi_create();
    screen_register_create();
    screen_admin_create();
    screen_tool_records_create();

    // Load standby screen as default
    s_current_screen = SCREEN_STANDBY;
    lv_scr_load(screen_standby_get_screen());

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "UI manager initialized, default screen: STANDBY");
}

void ui_manager_goto_screen(ui_screen_id_t screen_id)
{
    if (screen_id >= SCREEN_MAX) {
        ESP_LOGW(TAG, "Invalid screen ID: %d", screen_id);
        return;
    }

    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));

    lv_obj_t *new_screen = NULL;
    const char *screen_name = NULL;

    switch (screen_id) {
        case SCREEN_STANDBY:
            {
                uint16_t total = 0;
                uint16_t available = 0;
                cabinet_core_get_tool_counts(&total, &available);
                screen_standby_update_count(total, available);
            }
            new_screen = screen_standby_get_screen();
            screen_name = "STANDBY";
            break;
        case SCREEN_AUTH:
            new_screen = screen_auth_get_screen();
            screen_name = "AUTH";
            break;
        case SCREEN_USB_PREVIEW:
            new_screen = screen_usb_preview_get_screen();
            screen_name = "USB_PREVIEW";
            break;
        case SCREEN_OPERATION:
            screen_operation_init(NULL, 0);
            new_screen = screen_operation_get_screen();
            screen_name = "OPERATION";
            break;
        case SCREEN_RESULT:
            new_screen = screen_result_get_screen();
            screen_name = "RESULT";
            break;
        case SCREEN_WIFI:
            new_screen = screen_wifi_get_screen();
            screen_name = "WIFI";
            break;
        case SCREEN_REGISTER:
            new_screen = screen_register_get_screen();
            screen_name = "REGISTER";
            break;
        case SCREEN_ADMIN:
            new_screen = screen_admin_get_screen();
            screen_name = "ADMIN";
            break;
        case SCREEN_TOOL_RECORDS:
            screen_tool_records_refresh();
            new_screen = screen_tool_records_get_screen();
            screen_name = "TOOL_RECORDS";
            break;
        default:
            break;
    }

    if (new_screen != NULL) {
        s_current_screen = screen_id;
        lv_scr_load(new_screen);
        ESP_LOGI(TAG, "Navigated to screen: %s", screen_name);
    }

    esp_lv_adapter_unlock();
}

ui_screen_id_t ui_manager_get_current_screen(void)
{
    return s_current_screen;
}

// ============================================================================
// Standby Screen API
// ============================================================================

void standby_update_tool_count(uint16_t total, uint16_t available)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_standby_update_count(total, available);
    esp_lv_adapter_unlock();
}

void standby_set_online_status(bool online)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_standby_set_online(online);
    esp_lv_adapter_unlock();
}

void standby_trigger_pulse(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_standby_trigger_pulse();
    esp_lv_adapter_unlock();
}

void standby_set_time_synced(bool synced)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_standby_set_time_synced(synced);
    esp_lv_adapter_unlock();
}

void standby_set_nfc_callback(void (*cb)(void))
{
    screen_standby_set_nfc_callback(cb);
}

void standby_set_face_callback(void (*cb)(void))
{
    screen_standby_set_face_callback(cb);
}

void standby_set_register_callback(void (*cb)(void))
{
    screen_standby_set_register_callback(cb);
}

void standby_set_admin_callback(void (*cb)(void))
{
    screen_standby_set_admin_callback(cb);
}

void standby_set_settings_callback(void (*cb)(void))
{
    screen_standby_set_settings_callback(cb);
}

// ============================================================================
// Register Screen API
// ============================================================================

void register_start(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_register_start();
    esp_lv_adapter_unlock();
}

void register_cancel_wait(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_register_cancel_wait();
    esp_lv_adapter_unlock();
}

// ============================================================================
// Auth Screen API
// ============================================================================

void auth_set_step_state(uint8_t step, auth_step_state_t state, const char *info)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_auth_set_step(step, state, info);
    esp_lv_adapter_unlock();
}

void auth_reset_all_steps(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_auth_reset();
    esp_lv_adapter_unlock();
}

void auth_set_operator(const char *name)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_auth_set_operator(name);
    esp_lv_adapter_unlock();
}

void auth_start_wait(auth_type_t type)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_auth_start_wait(type);
    esp_lv_adapter_unlock();
}

void auth_cancel_wait(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_auth_cancel_wait();
    esp_lv_adapter_unlock();
}

// ============================================================================
// Operation Screen API
// ============================================================================

void operation_init_grid(tool_info_t *tools, uint8_t count)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_operation_init(tools, count);
    esp_lv_adapter_unlock();
}

void operation_update_slot_status(uint8_t slot_id, tool_slot_status_t status)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_operation_update_slot(slot_id, status);
    esp_lv_adapter_unlock();
}

void operation_set_operator(const char *name)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_operation_set_operator(name);
    esp_lv_adapter_unlock();
}

// ============================================================================
// Result Screen API
// ============================================================================

void result_show(operation_result_t *result)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_result_show(result);
    esp_lv_adapter_unlock();
}

void result_clear(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));
    screen_result_clear();
    esp_lv_adapter_unlock();
}

// ============================================================================
// Screen Data Access
// ============================================================================

screen_standby_data_t *standby_get_data(void)
{
    return screen_standby_get_data();
}

screen_auth_data_t *auth_get_data(void)
{
    return screen_auth_get_data();
}

screen_operation_data_t *operation_get_data(void)
{
    return screen_operation_get_data();
}

screen_result_data_t *result_get_data(void)
{
    return screen_result_get_data();
}

screen_wifi_data_t *wifi_get_data(void)
{
    return screen_wifi_get_data();
}

screen_register_data_t *register_get_data(void)
{
    return screen_register_get_data();
}

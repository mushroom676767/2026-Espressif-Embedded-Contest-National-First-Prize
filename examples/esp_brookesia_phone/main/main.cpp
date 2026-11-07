/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FOD Smart Tool Cabinet - Main Entry Point
 *
 * Initializes:
 *  - BSP (display, touch, audio codec)
 *  - WiFi + auto-connect from NVS
 *  - FOD UI screens (standby/auth/operation/result) via ui_manager
 *
 * Screen transitions are controlled via ui_manager_goto_screen().
 * Use the DEBUG buttons at bottom of standby screen to test transitions.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_lv_adapter.h"
#include "lvgl_adapter_init.h"

#include <new>

#include "ui/ui_manager.h"
#include "ui/ui_theme.h"
#include "screen_wifi.h"
#include "screen_admin.h"
#include "wifi_manager.h"
#include "tool_record_store.h"
#include "cabinet_snapshot_camera.h"
#include "cloud_uploader.h"
#include "cabinet_core.h"
#include "cabinet_door_signal.h"
#include "cabinet_imu.h"
#include "face_recognition_service.h"
#include "face_model_storage.h"
#include "nfc_manager.h"
#include "user_manager.h"

static const char *TAG = "main";

struct WifiUiEvent {
    wifi_evt_type_t evt;
    bool has_ap_info;
    wifi_ap_info_t ap_info;
};

// ============================================================================
// Settings button handler - WiFi settings UI
// ============================================================================
static void settings_btn_callback(void)
{
    ESP_LOGI(TAG, "Settings button -> WiFi settings");
    ui_manager_goto_screen(SCREEN_WIFI);
}

// ============================================================================
// NFC / Face button handlers - navigate to auth screen
// ============================================================================
static void nfc_btn_callback(void)
{
    ESP_LOGI(TAG, "NFC button -> Auth wait screen");
    ui_manager_goto_screen(SCREEN_AUTH);
    auth_start_wait(AUTH_TYPE_NFC);
}

static void face_btn_callback(void)
{
    ESP_LOGI(TAG, "Face button -> Auth wait screen");
    ui_manager_goto_screen(SCREEN_AUTH);
    auth_start_wait(AUTH_TYPE_FACE);
}

static void register_btn_callback(void)
{
    ESP_LOGI(TAG, "Register button -> Register screen");
    ui_manager_goto_screen(SCREEN_REGISTER);
    register_start();
}

static void admin_btn_callback(void)
{
    ESP_LOGI(TAG, "Admin button -> Admin screen");
    ui_manager_goto_screen(SCREEN_ADMIN);
    screen_admin_start();
}

// ============================================================================
// WiFi event callback
// ============================================================================
static void wifi_apply_ui_event(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info)
{
    switch (evt) {
        case WIFI_EVT_CONNECTED:
            standby_set_online_status(true);
            break;
        case WIFI_EVT_DISCONNECTED:
            standby_set_online_status(false);
            break;
        case WIFI_EVT_CONNECT_FAILED:
            standby_set_online_status(false);
            break;
        case WIFI_EVT_CONNECTING:
            standby_set_online_status(false);
            break;
        default:
            break;
    }

    screen_wifi_handle_wifi_event(evt, ap_info);
}

static void wifi_ui_event_async_cb(void *user_data)
{
    WifiUiEvent *pending = static_cast<WifiUiEvent *>(user_data);
    if (pending == nullptr) {
        return;
    }

    wifi_apply_ui_event(pending->evt, pending->has_ap_info ? &pending->ap_info : nullptr);
    delete pending;
}

static void queue_wifi_ui_event(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info)
{
    WifiUiEvent *pending = new (std::nothrow) WifiUiEvent {};
    if (pending == nullptr) {
        ESP_LOGW(TAG, "No memory to queue WiFi UI event");
        return;
    }

    pending->evt = evt;
    if (ap_info != nullptr) {
        pending->has_ap_info = true;
        pending->ap_info = *ap_info;
    }

    esp_err_t lock_ret = esp_lv_adapter_lock(portMAX_DELAY);
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for WiFi UI event: %s", esp_err_to_name(lock_ret));
        delete pending;
        return;
    }

    lv_res_t async_ret = lv_async_call(wifi_ui_event_async_cb, pending);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue WiFi UI event");
        delete pending;
    }
}

static void wifi_event_cb(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info)
{
    switch (evt) {
        case WIFI_EVT_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected! Starting SNTP...");
            wifi_manager_start_sntp();
            break;
        case WIFI_EVT_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected");
            break;
        case WIFI_EVT_CONNECT_FAILED:
            ESP_LOGW(TAG, "WiFi connect failed");
            break;
        case WIFI_EVT_CONNECTING:
            ESP_LOGI(TAG, "WiFi connecting...");
            break;
        default:
            break;
    }

    queue_wifi_ui_event(evt, ap_info);
}

// ============================================================================
// UI init task (runs on core 1)
// ============================================================================
#define UI_TASK_STACK_SIZE     (1024 * 8)
#define UI_TASK_PRIORITY       (5)
#define UI_TASK_CORE           (1)
#define WIFI_AUTO_RETRY_TASK_STACK    (1024 * 4)
#define WIFI_AUTO_RETRY_TASK_PRIORITY (3)
#define WIFI_AUTO_RETRY_INTERVAL_MS   (1000)

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI task started on core %d", xPortGetCoreID());

    // Lock LVGL during UI creation
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));

    // Initialize theme + UI manager (creates all screens, loads standby)
    ui_theme_init();
    ui_manager_init();

    // Register standby screen button callbacks
    standby_set_nfc_callback(nfc_btn_callback);
    standby_set_face_callback(face_btn_callback);
    standby_set_register_callback(register_btn_callback);
    standby_set_admin_callback(admin_btn_callback);
    standby_set_settings_callback(settings_btn_callback);

    // Set initial values from persisted cabinet state
    uint16_t tool_total = 9;
    uint16_t tool_available = 9;
    cabinet_core_get_tool_counts(&tool_total, &tool_available);
    standby_update_tool_count(tool_total, tool_available);
    standby_set_online_status(wifi_manager_is_connected());

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "UI initialized, standby screen active");
    ESP_LOGI(TAG, "Use DEBUG buttons (bottom) to test screen transitions");

    // Keep task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void wifi_standby_auto_retry_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(WIFI_AUTO_RETRY_INTERVAL_MS));

    while (1) {
        wifi_state_t state = wifi_manager_get_state();
        if (ui_manager_get_current_screen() == SCREEN_STANDBY &&
            !wifi_manager_is_connected() &&
            state != WIFI_STATE_CONNECTING &&
            state != WIFI_STATE_SCANNING &&
            !wifi_manager_is_scanning()) {
            if (wifi_manager_connect_saved()) {
                ESP_LOGI(TAG, "Standby offline, retrying saved WiFi auto-connect...");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_AUTO_RETRY_INTERVAL_MS));
    }
}

static void init_sd_card_and_face_models(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s. Face models will be unavailable.", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card mount OK");

    ret = face_model_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Face model storage init failed: %s (%s)",
                 esp_err_to_name(ret), face_model_storage_last_error());
        return;
    }

    ESP_LOGI(TAG, "Face model storage ready");
}

// ============================================================================
// App main
// ============================================================================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== FOD Smart Tool Cabinet starting... ===");

    ESP_ERROR_CHECK(cabinet_door_signal_init());
    esp_err_t imu_err = cabinet_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGE(TAG, "Cabinet IMU init failed: %s; door flow will remain locked",
                 esp_err_to_name(imu_err));
    }

    // --- NVS Init ---
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // --- SPIFFS Mount ---
    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount OK");

    err = tool_record_store_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Tool record store init failed: %s", esp_err_to_name(err));
    }
    err = cabinet_snapshot_camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cabinet snapshot camera init failed: %s", esp_err_to_name(err));
    }
    err = cloud_uploader_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cloud uploader init failed: %s", esp_err_to_name(err));
    }
    err = cabinet_core_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cabinet core init failed: %s", esp_err_to_name(err));
    }

    // --- SD Card + bundled face model provisioning ---
    init_sd_card_and_face_models();
    err = face_recognition_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Face recognition service init failed: %s", esp_err_to_name(err));
    }
    err = nfc_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NFC manager init failed: %s", esp_err_to_name(err));
    }
    err = user_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "User manager init failed: %s", esp_err_to_name(err));
    }

    // --- Audio Codec Init ---
    ESP_ERROR_CHECK(bsp_extra_codec_init());

    // --- Display + Touch Init (BSP) ---
    bsp_display_cfg_t cfg = {
        .hw_cfg = {
            .hdmi_resolution = BSP_HDMI_RES_NONE,
            .dsi_bus = {
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            },
        },
    };
    lv_display_t *disp = lvgl_adapter_init(&cfg);
    assert(disp != nullptr && "Failed to init LVGL adapter");
    bsp_display_backlight_on();

    // Lock LVGL before creating UI task
    ESP_ERROR_CHECK(esp_lv_adapter_lock(portMAX_DELAY));

    TaskHandle_t ui_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_task, "ui_task", UI_TASK_STACK_SIZE, NULL,
        UI_TASK_PRIORITY, &ui_task_handle, UI_TASK_CORE);
    assert(ret == pdPASS && ui_task_handle != NULL);

    esp_lv_adapter_unlock();

    // --- Real WiFi init + auto-connect from NVS ---
    wifi_manager_init();
    wifi_manager_set_event_callback(wifi_event_cb);

    if (wifi_manager_connect_saved()) {
        ESP_LOGI(TAG, "Auto-connecting to saved WiFi list...");
    } else {
        ESP_LOGW(TAG, "No WiFi configured. Tap Set (top-right) for WiFi settings.");
    }

    TaskHandle_t wifi_retry_task_handle = NULL;
    ret = xTaskCreate(wifi_standby_auto_retry_task, "wifi_auto_retry",
                      WIFI_AUTO_RETRY_TASK_STACK, NULL,
                      WIFI_AUTO_RETRY_TASK_PRIORITY, &wifi_retry_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi standby auto-retry task");
    }

    ESP_LOGI(TAG, "=== FOD Smart Tool Cabinet ready ===");
}

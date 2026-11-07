/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone WiFi Manager for FOD Smart Tool Cabinet
 *
 * Extracted and adapted from esp_brookesia_phone AppSettings.
 * Handles WiFi init, scan, connect, and provides callbacks for UI.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_LIST_SIZE    25

/**
 * @brief WiFi signal strength level
 */
typedef enum {
    WIFI_SIGNAL_NONE = 0,
    WIFI_SIGNAL_WEAK = 1,
    WIFI_SIGNAL_MODERATE = 2,
    WIFI_SIGNAL_GOOD = 3,
} wifi_signal_level_t;

/**
 * @brief WiFi connection state
 */
typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
} wifi_state_t;

typedef enum {
    WIFI_EVT_INIT_DONE,
    WIFI_EVT_SCAN_STARTED,
    WIFI_EVT_SCAN_DONE,
    WIFI_EVT_SCAN_FAILED,
    WIFI_EVT_CONNECTING,
    WIFI_EVT_CONNECTED,
    WIFI_EVT_CONNECT_FAILED,
    WIFI_EVT_DISCONNECTED,
} wifi_evt_type_t;

/**
 * @brief AP info for display in UI
 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
    bool has_password;
    wifi_signal_level_t signal;
} wifi_ap_info_t;

/**
 * @brief Callback function type for WiFi events
 *
 * @param evt Event type
 * @param ap_info AP info, valid for WIFI_EVT_CONNECTED only
 */
typedef void (*wifi_evt_cb_t)(wifi_evt_type_t evt, const wifi_ap_info_t *ap_info);

/**
 * @brief Initialize WiFi (call once at startup)
 *
 * Initializes ESP-NETIF, WiFi driver, registers event handlers.
 * Does not connect automatically. Call wifi_manager_connect() after.
 */
void wifi_manager_init(void);

/**
 * @brief Start scanning for nearby APs
 *
 * Results are reported via WIFI_EVT_SCAN_DONE callback.
 */
void wifi_manager_start_scan(void);

/**
 * @brief Stop ongoing scan
 */
void wifi_manager_stop_scan(void);

/**
 * @brief Get current WiFi manager state
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Check whether a scan is running
 */
bool wifi_manager_is_scanning(void);

/**
 * @brief Get number of cached scan results
 */
uint16_t wifi_manager_get_ap_count(void);

/**
 * @brief Copy one cached scan result
 */
bool wifi_manager_get_ap_info(uint16_t index, wifi_ap_info_t *out_info);

/**
 * @brief Connect to a specific AP
 *
 * @param ssid SSID string
 * @param password Password string. Must not be NULL; empty string connects to an open network.
 */
void wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Start auto-connect with encrypted credentials stored in NVS
 */
bool wifi_manager_connect_saved(void);

/**
 * @brief Disconnect current connection
 */
void wifi_manager_disconnect(void);

/**
 * @brief Check if currently connected and has an IP address
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current signal strength
 */
wifi_signal_level_t wifi_manager_get_signal_level(void);

/**
 * @brief Register event callback
 *
 * Callbacks are invoked from WiFi event, scan, or connect task context.
 */
void wifi_manager_set_event_callback(wifi_evt_cb_t cb);

/**
 * @brief Get the current SSID if known
 */
const char *wifi_manager_get_current_ssid(void);

/**
 * @brief Get the latest WiFi disconnect reason code
 */
uint8_t wifi_manager_get_last_disconnect_reason(void);

/**
 * @brief Get a short name for the latest WiFi disconnect reason
 */
const char *wifi_manager_get_last_disconnect_reason_name(void);

/**
 * @brief Legacy SquareLine UI bridge hook. Kept as a no-op for compatibility.
 */
void wifi_manager_register_ui(void);

/**
 * @brief Load WiFi credentials from NVS
 *
 * Reads stored encrypted WiFi credentials from NVS namespace "storage".
 * Returns false if no credentials stored.
 */
bool wifi_manager_load_from_nvs(void);

/**
 * @brief Save WiFi credentials to NVS
 */
void wifi_manager_save_to_nvs(const char *ssid, const char *password);

/**
 * @brief Start SNTP time sync (called automatically when connected)
 */
void wifi_manager_start_sntp(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H

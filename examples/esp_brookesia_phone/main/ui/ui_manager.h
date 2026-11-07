/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct screen_standby_data_s screen_standby_data_t;
typedef struct screen_auth_data_s screen_auth_data_t;
typedef struct screen_operation_data_s screen_operation_data_t;
typedef struct screen_result_data_s screen_result_data_t;
typedef struct screen_wifi_data_s screen_wifi_data_t;
typedef struct screen_register_data_s screen_register_data_t;
typedef struct screen_admin_data_s screen_admin_data_t;

/**
 * @brief Screen IDs for the FOD system
 */
typedef enum {
    SCREEN_STANDBY = 0,
    SCREEN_AUTH,
    SCREEN_USB_PREVIEW,
    SCREEN_OPERATION,
    SCREEN_RESULT,
    SCREEN_WIFI,
    SCREEN_REGISTER,
    SCREEN_ADMIN,
    SCREEN_TOOL_RECORDS,
    SCREEN_MAX
} ui_screen_id_t;

/**
 * @brief Authentication step states
 */
typedef enum {
    AUTH_STEP_IDLE = 0,
    AUTH_STEP_WAITING,
    AUTH_STEP_PROCESSING,
    AUTH_STEP_SUCCESS,
    AUTH_STEP_FAILED
} auth_step_state_t;

/**
 * @brief Authentication type
 */
typedef enum {
    AUTH_TYPE_NONE = 0,
    AUTH_TYPE_NFC,
    AUTH_TYPE_FACE,
    AUTH_TYPE_SAFETY_CHECK,
    AUTH_TYPE_ADMIN
} auth_type_t;

/**
 * @brief Tool slot status
 */
typedef enum {
    TOOL_SLOT_AVAILABLE = 0,   // Tool present (green)
    TOOL_SLOT_BORROWED,       // Tool borrowed (red)
    TOOL_SLOT_WARNING,        // Anomaly (amber)
    TOOL_SLOT_EMPTY           // Empty slot outline
} tool_slot_status_t;

/**
 * @brief Result screen types
 */
typedef enum {
    RESULT_BORROW_OK = 0,      // Successful borrow
    RESULT_RETURN_OK,         // Successful return
    RESULT_OPERATION_OK,       // Operation completed without a single borrow/return direction
    RESULT_ANOMALY,           // Anomaly detected
    RESULT_AUTH_FAILED        // Authentication failed
} result_type_t;

#define UI_OPERATOR_NAME_MAX_LEN 96

/**
 * @brief Tool information structure
 */
typedef struct {
    uint8_t slot_id;
    char name[32];
    char id[16];
    tool_slot_status_t status;
} tool_info_t;

/**
 * @brief Operation result data
 */
typedef struct {
    char operator_name[UI_OPERATOR_NAME_MAX_LEN];
    char timestamp[32];
    tool_info_t borrowed[9];
    uint8_t borrowed_count;
    tool_info_t returned[9];
    uint8_t returned_count;
    tool_info_t anomalous[9];
    uint8_t anomaly_count;
    result_type_t result_type;
    bool cloud_pushed;
} operation_result_t;

// ============================================================================
// Screen Management API
// ============================================================================

/**
 * @brief Initialize the UI manager
 *
 * Must be called after LVGL display is initialized.
 */
void ui_manager_init(void);

/**
 * @brief Navigate to a specific screen
 *
 * @param screen_id Target screen ID
 */
void ui_manager_goto_screen(ui_screen_id_t screen_id);

/**
 * @brief Get the current screen ID
 */
ui_screen_id_t ui_manager_get_current_screen(void);

/**
 * @brief Task entry function for UI management
 *
 * @param arg Unused argument
 */
void ui_manager_task(void *arg);

// ============================================================================
// Standby Screen API
// ============================================================================

/**
 * @brief Update the tool count display on standby screen
 *
 * @param total Total tools
 * @param available Available tools
 */
void standby_update_tool_count(uint16_t total, uint16_t available);

/**
 * @brief Update the online/offline status indicator
 *
 * @param online true if Wi-Fi is connected
 */
void standby_set_online_status(bool online);

/**
 * @brief Trigger a pulse animation on the standby screen
 */
void standby_trigger_pulse(void);

/**
 * @brief Notify that NTP time has been synced
 */
void standby_set_time_synced(bool synced);

/**
 * @brief Register standby screen button callbacks
 */
void standby_set_nfc_callback(void (*cb)(void));
void standby_set_face_callback(void (*cb)(void));
void standby_set_register_callback(void (*cb)(void));
void standby_set_admin_callback(void (*cb)(void));
void standby_set_settings_callback(void (*cb)(void));

// ============================================================================
// Register Screen API
// ============================================================================

/**
 * @brief Start a new user registration flow
 */
void register_start(void);

/**
 * @brief Cancel active register wait session
 */
void register_cancel_wait(void);

// ============================================================================
// Auth Screen API
// ============================================================================

/**
 * @brief Set the state of a specific authentication step
 *
 * @param step Step index (0=NFC, 1=Face, 2=Safety)
 * @param state New state
 * @param info Additional info text (can be NULL)
 */
void auth_set_step_state(uint8_t step, auth_step_state_t state, const char *info);

/**
 * @brief Set all steps to idle
 */
void auth_reset_all_steps(void);

/**
 * @brief Set operator name when auth is complete
 *
 * @param name Operator name
 */
void auth_set_operator(const char *name);

/**
 * @brief Start a 60s authentication wait session
 *
 * @param type AUTH_TYPE_NFC or AUTH_TYPE_FACE
 */
void auth_start_wait(auth_type_t type);

/**
 * @brief Cancel the active authentication wait session
 */
void auth_cancel_wait(void);

// ============================================================================
// Operation Screen API
// ============================================================================

/**
 * @brief Initialize the tool grid with tool list
 *
 * @param tools Array of tool info
 * @param count Number of tools
 */
void operation_init_grid(tool_info_t *tools, uint8_t count);

/**
 * @brief Update the status of a single tool slot
 *
 * @param slot_id Slot ID
 * @param status New status
 */
void operation_update_slot_status(uint8_t slot_id, tool_slot_status_t status);

/**
 * @brief Update operator info on operation screen
 *
 * @param name Operator name
 */
void operation_set_operator(const char *name);

// ============================================================================
// Result Screen API
// ============================================================================

/**
 * @brief Show the result screen with operation details
 *
 * @param result Pointer to operation result data
 */
void result_show(operation_result_t *result);

/**
 * @brief Clear the result display
 */
void result_clear(void);

// ============================================================================
// Screen Data Access (for callbacks / business logic)
// ============================================================================

/**
 * @brief Get standby screen data for callbacks
 */
screen_standby_data_t *standby_get_data(void);

/**
 * @brief Get auth screen data for callbacks
 */
screen_auth_data_t *auth_get_data(void);

/**
 * @brief Get operation screen data for callbacks
 */
screen_operation_data_t *operation_get_data(void);

/**
 * @brief Get result screen data for callbacks
 */
screen_result_data_t *result_get_data(void);

/**
 * @brief Get WiFi settings screen data
 */
screen_wifi_data_t *wifi_get_data(void);

screen_register_data_t *register_get_data(void);

#ifdef __cplusplus
}
#endif

#endif // UI_MANAGER_H

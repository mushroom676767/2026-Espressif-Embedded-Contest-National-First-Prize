/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NFC_MANAGER_H
#define NFC_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NFC_CARD_RAW_LEN 13
#define NFC_CARD_HEX_LEN (NFC_CARD_RAW_LEN * 2)
#define NFC_USER_NAME_MAX_LEN 64

typedef struct {
    uint8_t raw[NFC_CARD_RAW_LEN];
    uint8_t raw_len;
    char card_hex[NFC_CARD_HEX_LEN + 1];
} nfc_event_t;

typedef void (*nfc_event_cb_t)(const nfc_event_t *event, void *user_ctx);

esp_err_t nfc_manager_init(void);
bool nfc_manager_is_ready(void);
void nfc_manager_flush_input(void);
void nfc_manager_ack_event(void);
void nfc_manager_set_event_callback(nfc_event_cb_t cb, void *user_ctx);
esp_err_t nfc_manager_normalize_card_hex(const char *src, char *dst, size_t dst_size,
                                         size_t *uid_len_out);
esp_err_t nfc_manager_save_card_user(const char *card_hex, const char *name);
esp_err_t nfc_manager_save_card_binding(const char *card_hex, const char *employee_id, const char *name);
esp_err_t nfc_manager_find_user(const char *card_hex, char *name, size_t name_size);
esp_err_t nfc_manager_delete_user_cards(const char *employee_id);

#ifdef __cplusplus
}
#endif

#endif // NFC_MANAGER_H

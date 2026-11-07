#ifndef PN532_UART_H
#define PN532_UART_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_UID_MAX_LEN 10

esp_err_t pn532_uart_init(uart_port_t uart_num, gpio_num_t tx_gpio, gpio_num_t rx_gpio);
void pn532_uart_deinit(void);
void pn532_uart_flush_input(void);
esp_err_t pn532_uart_read_passive_target(uint8_t *uid, size_t uid_capacity,
                                         size_t *uid_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // PN532_UART_H

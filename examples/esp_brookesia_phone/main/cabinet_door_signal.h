#ifndef CABINET_DOOR_SIGNAL_H
#define CABINET_DOOR_SIGNAL_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cabinet_door_signal_init(void);
esp_err_t cabinet_door_signal_unlock_pulse(void);
esp_err_t cabinet_door_signal_close_pulse(void);
esp_err_t cabinet_door_signal_lock(void);

#ifdef __cplusplus
}
#endif

#endif // CABINET_DOOR_SIGNAL_H

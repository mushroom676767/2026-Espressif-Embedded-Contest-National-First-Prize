#ifndef CABINET_IMU_H
#define CABINET_IMU_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CABINET_IMU_DOOR_UNAVAILABLE = 0,
    CABINET_IMU_DOOR_BASELINING,
    CABINET_IMU_DOOR_OPENING,
    CABINET_IMU_DOOR_FULLY_OPEN,
} cabinet_imu_door_state_t;

typedef struct {
    bool initialized;
    bool sample_valid;
    bool detection_active;
    cabinet_imu_door_state_t door_state;
    uint32_t sample_sequence;
    uint32_t sensor_timestamp_ms;
    int64_t received_at_us;
    float angle_x_deg;
    float angle_y_deg;
    float angle_z_deg;
    float angle_x_unwrapped_deg;
    float closed_baseline_deg;
    float angle_x_delta_deg;
} cabinet_imu_status_t;

esp_err_t cabinet_imu_init(void);
esp_err_t cabinet_imu_start_open_detection(void);
void cabinet_imu_stop_open_detection(void);
esp_err_t cabinet_imu_get_status(cabinet_imu_status_t *status_out);
const char *cabinet_imu_state_name(cabinet_imu_door_state_t state);

#ifdef __cplusplus
}
#endif

#endif // CABINET_IMU_H

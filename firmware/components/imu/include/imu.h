// BMI270 accelerometer (Tab5 internal I2C bus).
#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif

// Finds the BMI270 on `bus`, uploads its firmware and starts the accelerometer (+-2 g, 100 Hz).
// Returns 0 on success, negative on failure (-10 = Bosch driver files not installed).
int imu_init(i2c_master_bus_handle_t bus);

// Acceleration in milli-g (1000 = 1 g) along the sensor's own x, y, z. Returns 0 on success.
int imu_read_mg(int16_t* x, int16_t* y, int16_t* z);

#ifdef __cplusplus
}
#endif

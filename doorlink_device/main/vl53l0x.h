#pragma once

#include "driver/i2c.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the VL53L0X Time-of-Flight sensor on the specified I2C port.
 *
 * @param i2c_num I2C port number
 * @return esp_err_t ESP_OK on success, or error code on failure
 */
esp_err_t vl53l0x_init(i2c_port_t i2c_num);

/**
 * @brief Perform a single range measurement in millimeters.
 *
 * @param i2c_num I2C port number
 * @param out_mm Pointer to store measured range in millimeters
 * @return esp_err_t ESP_OK on success, or error code on failure
 */
esp_err_t vl53l0x_read_range_mm(i2c_port_t i2c_num, uint16_t *out_mm);

#ifdef __cplusplus
}
#endif

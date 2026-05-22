// Minimal VL6180X driver interface
#pragma once

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

esp_err_t vl6180x_init(i2c_port_t i2c_num);
esp_err_t vl6180x_read_range_mm(i2c_port_t i2c_num, uint16_t *out_mm);

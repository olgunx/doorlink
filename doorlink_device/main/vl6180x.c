// Minimal VL6180X driver, ported from olgunx/doorlock.
#include "vl6180x.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint8_t VL6180X_ADDR = 0x29;

static esp_err_t write_reg16(i2c_port_t i2c_num, uint16_t reg, uint8_t val)
{
    uint8_t buf[3];
    buf[0] = (reg >> 8) & 0xFF;
    buf[1] = reg & 0xFF;
    buf[2] = val;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL6180X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, sizeof(buf), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    return err;
}

static esp_err_t read_reg16(i2c_port_t i2c_num, uint16_t reg, uint8_t *val)
{
    uint8_t addr[2];
    addr[0] = (reg >> 8) & 0xFF;
    addr[1] = reg & 0xFF;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL6180X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, addr, sizeof(addr), true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL6180X_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    return err;
}

esp_err_t vl6180x_init(i2c_port_t i2c_num)
{
    uint8_t model_id = 0;
    esp_err_t err = read_reg16(i2c_num, 0x000, &model_id);
    if (err != ESP_OK) {
        return err;
    }
    (void)model_id;

    return ESP_OK;
}

esp_err_t vl6180x_read_range_mm(i2c_port_t i2c_num, uint16_t *out_mm)
{
    esp_err_t err = write_reg16(i2c_num, 0x018, 0x01);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t range = 0;
    err = read_reg16(i2c_num, 0x062, &range);
    if (err != ESP_OK) {
        return err;
    }

    *out_mm = (uint16_t)range;
    return ESP_OK;
}

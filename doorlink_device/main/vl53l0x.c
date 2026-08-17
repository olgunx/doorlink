// Complete VL53L0X Time-of-Flight sensor driver for ESP-IDF.
#include "vl53l0x.h"

#include "config.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vl53l0x";
static const uint8_t VL53L0X_ADDR = 0x29;
static uint8_t s_stop_variable = 0x3C;

static esp_err_t write_reg8(i2c_port_t i2c_num, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL53L0X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, sizeof(buf), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t read_reg8(i2c_port_t i2c_num, uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL53L0X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL53L0X_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t read_bytes(i2c_port_t i2c_num, uint8_t reg, uint8_t *buf, size_t len)
{
    if (len == 0) return ESP_OK;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL53L0X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VL53L0X_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &buf[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t read_reg16(i2c_port_t i2c_num, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2] = {0};
    esp_err_t err = read_bytes(i2c_num, reg, buf, 2);
    if (err == ESP_OK && val != NULL) {
        *val = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return err;
}

static esp_err_t init_spads(i2c_port_t i2c_num)
{
    write_reg8(i2c_num, 0x80, 0x01);
    write_reg8(i2c_num, 0xFF, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);
    write_reg8(i2c_num, 0xFF, 0x06);

    uint8_t spad_curr = 0;
    read_reg8(i2c_num, 0x83, &spad_curr);
    write_reg8(i2c_num, 0x83, spad_curr | 0x04);
    write_reg8(i2c_num, 0xFF, 0x07);
    write_reg8(i2c_num, 0x81, 0x01);

    write_reg8(i2c_num, 0x80, 0x01);
    write_reg8(i2c_num, 0x92, 0x01);
    write_reg8(i2c_num, 0x80, 0x00);
    write_reg8(i2c_num, 0xFF, 0x00);
    return ESP_OK;
}

static esp_err_t load_tuning_settings(i2c_port_t i2c_num)
{
    write_reg8(i2c_num, 0xFF, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);
    write_reg8(i2c_num, 0xFF, 0x00);
    write_reg8(i2c_num, 0x09, 0x00);
    write_reg8(i2c_num, 0x10, 0x00);
    write_reg8(i2c_num, 0x11, 0x00);
    write_reg8(i2c_num, 0x24, 0x01);
    write_reg8(i2c_num, 0x25, 0xFF);
    write_reg8(i2c_num, 0x75, 0x00);
    write_reg8(i2c_num, 0x4E, 0x2C);
    write_reg8(i2c_num, 0x48, 0x00);
    write_reg8(i2c_num, 0x30, 0x20);
    return ESP_OK;
}

static esp_err_t perform_ref_calibration(i2c_port_t i2c_num)
{
    // VHV calibration sequence
    esp_err_t err = write_reg8(i2c_num, 0x00, 0x01 | 0x40);
    if (err != ESP_OK) return err;

    uint8_t status = 0;
    int timeout = 30;
    while (timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (read_reg8(i2c_num, 0x13, &status) == ESP_OK && (status & 0x07) != 0) {
            break;
        }
        timeout--;
    }
    write_reg8(i2c_num, 0x0B, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);

    // Phase calibration sequence
    err = write_reg8(i2c_num, 0x00, 0x01 | 0x00);
    if (err != ESP_OK) return err;

    timeout = 30;
    while (timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (read_reg8(i2c_num, 0x13, &status) == ESP_OK && (status & 0x07) != 0) {
            break;
        }
        timeout--;
    }
    write_reg8(i2c_num, 0x0B, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);

    return ESP_OK;
}

esp_err_t vl53l0x_init(i2c_port_t i2c_num)
{
    uint8_t model_id = 0;
    esp_err_t err = read_reg8(i2c_num, 0xC0, &model_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Model ID over I2C: %s", esp_err_to_name(err));
        return err;
    }

    if (model_id != 0xEE) {
        ESP_LOGW(TAG, "VL53L0X Model ID mismatch (expected 0xEE, got 0x%02X)", model_id);
    }

    write_reg8(i2c_num, 0x88, 0x00);
    write_reg8(i2c_num, 0x80, 0x01);
    write_reg8(i2c_num, 0xFF, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);

    err = read_reg8(i2c_num, 0x91, &s_stop_variable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read stop variable: %s", esp_err_to_name(err));
        return err;
    }

    write_reg8(i2c_num, 0x00, 0x01);
    write_reg8(i2c_num, 0xFF, 0x00);
    write_reg8(i2c_num, 0x80, 0x00);

    // Configure GPIO / Status Interrupt to 0x04 (NEW SAMPLE READY)
    write_reg8(i2c_num, 0x0A, 0x04);
    write_reg8(i2c_num, 0x0B, 0x01); // Clear interrupt

    uint8_t config_control = 0;
    read_reg8(i2c_num, 0x01, &config_control);
    write_reg8(i2c_num, 0x01, config_control | 0x0E);

    write_reg8(i2c_num, 0x44, 0x00);
    write_reg8(i2c_num, 0x45, 0x20);
    write_reg8(i2c_num, 0x01, 0xFF);

    init_spads(i2c_num);
    load_tuning_settings(i2c_num);

    esp_err_t cal_err = perform_ref_calibration(i2c_num);
    if (cal_err != ESP_OK) {
        ESP_LOGW(TAG, "Ref calibration warning: %s. Continuing init.", esp_err_to_name(cal_err));
    }

    ESP_LOGI(TAG, "VL53L0X sensor initialized successfully (Model ID: 0x%02X)", model_id);
    return ESP_OK;
}

esp_err_t vl53l0x_read_range_mm(i2c_port_t i2c_num, uint16_t *out_mm)
{
    if (out_mm == NULL) return ESP_ERR_INVALID_ARG;

    // Ensure interrupt config is active (New Sample Ready)
    write_reg8(i2c_num, 0x0A, 0x04);

    // Single-shot sequence
    write_reg8(i2c_num, 0x80, 0x01);
    write_reg8(i2c_num, 0xFF, 0x01);
    write_reg8(i2c_num, 0x00, 0x00);
    write_reg8(i2c_num, 0x91, s_stop_variable);
    write_reg8(i2c_num, 0x00, 0x01);
    write_reg8(i2c_num, 0xFF, 0x00);
    write_reg8(i2c_num, 0x80, 0x00);

    // Start single-shot conversion
    esp_err_t err = write_reg8(i2c_num, 0x00, 0x01);
    if (err != ESP_OK) return err;

    uint8_t status = 0;
    int timeout = 40;
    while (timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        err = read_reg8(i2c_num, 0x13, &status);
        if (err == ESP_OK && (status & 0x07) != 0) {
            break;
        }
        timeout--;
    }

    if (timeout == 0) {
        write_reg8(i2c_num, 0x00, 0x00);
        write_reg8(i2c_num, 0x0B, 0x01);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t range_status_reg = 0;
    read_reg8(i2c_num, 0x14, &range_status_reg);
    uint8_t range_status = (range_status_reg >> 3) & 0x0F;

    uint16_t range_mm = 0;
    err = read_reg16(i2c_num, 0x1E, &range_mm);
    write_reg8(i2c_num, 0x0B, 0x01); // Clear interrupt flag

    if (err != ESP_OK) return err;

    // Status 0 and status 11 (0x0B) are valid range status codes in VL53L0X hardware.
    // Non-matching statuses (1: Sigma Fail, 2: Signal Fail, 4: Phase Fail) indicate out-of-range.
    bool status_valid = (range_status == 0 || range_status == 11);
    if (!status_valid || range_mm < 20 || range_mm > 2000) {
        *out_mm = 0xFFFF; // Mark as no target in range
    } else {
        *out_mm = range_mm;
    }
    return ESP_OK;
}

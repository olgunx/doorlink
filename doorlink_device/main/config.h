#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SCAN_INTERVAL_UNITS      0x00A0  /* 100 ms, 0.625 ms units */
#define BLE_SCAN_WINDOW_UNITS        0x0030  /* 30 ms, 0.625 ms units (gives Wi-Fi airtime) */
#define BLE_SCAN_ACTIVE              0
#define BLE_SCAN_FILTER_DUPLICATES   0

#define MAX_TRACKED_DEVICES          32
#define MAX_DEVICE_NAME_LEN          32
#define MAX_MFG_DATA_LEN             32
#define RSSI_SAMPLE_WINDOW           8
#define MIN_TRACKED_RSSI_DBM         (-85)
#define DEVICE_STALE_TIMEOUT_MS      12000
#define DEVICE_SWEEP_PERIOD_MS       1000
#define BEACON_DETECTION_LOG_PERIOD_MS 100
#define BEACON_RSSI_THRESHOLD_DBM    (-85)
#define BEACON_ARRIVED_RSSI_DBM      (-73)
#define BEACON_AVERAGE_WINDOW        8
#define BEACON_PATTERN_WINDOW        (BEACON_AVERAGE_WINDOW * 2)
#define BEACON_ARRIVED_MIN_SAMPLES   (BEACON_AVERAGE_WINDOW + 2)
#define BEACON_STABLE_DELTA_DBM      1
#define BEACON_MOVEMENT_DELTA_DBM    3
#define BEACON_ARRIVED_STABLE_COUNT  5

#define ALIVE_LED_GPIO               12
#define DETECTION_LED_GPIO           13
#define STATUS_LED_ACTIVE_LOW        0
#define ALIVE_LED_PERIOD_MS          500
#define DETECTION_LED_HOLD_MS        3000
#define BLE_LOST_LOG_MS              8000
#define DETECTION_LED_CLOSER_PERIOD_MS 200
#define DETECTION_LED_AWAY_PERIOD_MS 1000
#define DETECTION_LED_FAST_PERIOD_MS 80
#define UNLOCK_BLINK_HOLD_MS         3000

#define RELAY_GPIO                   2
#define RELAY_ACTIVE_LOW             1
#define RELAY_TRIGGER_MS             1000

#define VL6180X_I2C_PORT             I2C_NUM_0
#define VL6180X_I2C_SDA_GPIO         18
#define VL6180X_I2C_SCL_GPIO         19
#define VL6180X_I2C_CLK_SPEED_HZ     100000
#define VL6180X_PRESENCE_THRESHOLD_MM 150
#define VL6180X_POLL_MS              200

typedef struct {
    uint16_t manufacturer_id;
    const char *name_text;
} mfg_test_entry_t;

/* Optional test filter. Enable to focus on one manufacturer payload. */
#define ENABLE_NAME_FILTER           0
#define NAME_FILTER_PREFIX           "Xperia"
#define ENABLE_MFG_FILTER            1

/* Test entry fields. Serialized to manufacturer data bytes at runtime. */
#define MFG_TEST_ENTRY               { .manufacturer_id = 0x0143, .name_text = "7836F9853359AD65" }

typedef enum {
    PROXIMITY_VERY_NEAR = 0,
    PROXIMITY_NEAR,
    PROXIMITY_FAR,
} proximity_class_t;

typedef enum {
    DEVICE_ABSENT = 0,
    DEVICE_PRESENT,
} presence_state_t;

typedef struct {
    const char *name_prefix;
    const uint8_t *mfg_pattern;
    size_t mfg_len;
    bool match_uuid;
    ble_uuid_any_t target_uuid;
} filter_config_t;

#ifdef __cplusplus
}
#endif

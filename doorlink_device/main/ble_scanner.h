#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIGHTHOUSE_USER_ID_LEN 4
#define LIGHTHOUSE_TOKEN_LEN   16

typedef struct {
    uint8_t user_id[LIGHTHOUSE_USER_ID_LEN];
    uint8_t token[LIGHTHOUSE_TOKEN_LEN];
    int rssi;
} lighthouse_ble_claim_t;

typedef void (*lighthouse_claim_detected_cb_t)(const lighthouse_ble_claim_t *claim);

void ble_scanner_set_claim_detected_cb(lighthouse_claim_detected_cb_t cb);
esp_err_t ble_scanner_set_static_uuid_beacon(const uint8_t *uuid_16_bytes);
esp_err_t ble_scanner_init(void);

#ifdef __cplusplus
}
#endif

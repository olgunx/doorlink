#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble_scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENROLLMENT_MAX_DEVICES 10
#define ENROLLMENT_PUBKEY_LEN  65

typedef struct {
    uint8_t user_id[LIGHTHOUSE_USER_ID_LEN];
    uint8_t pubkey[ENROLLMENT_PUBKEY_LEN];
    bool active;
} enrolled_device_t;

esp_err_t enrollment_mgr_init(void);
bool enrollment_mgr_check_admin_pass(const char *password);
esp_err_t enrollment_mgr_set_admin_pass(const char *password);
esp_err_t enrollment_mgr_add_device(const uint8_t *user_id, const uint8_t *pubkey);
esp_err_t enrollment_mgr_revoke_device(const uint8_t *user_id);
size_t enrollment_mgr_get_devices(enrolled_device_t *out_devices, size_t max_devices);
bool enrollment_mgr_is_device_active(const uint8_t *user_id, uint8_t *out_pubkey);

#ifdef __cplusplus
}
#endif

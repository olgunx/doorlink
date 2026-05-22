#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "host/ble_hs.h"
#include "proximity_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ble_addr_t addr;
    char name[MAX_DEVICE_NAME_LEN];
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t detected_count;
    int last_rssi;
    int smoothed_rssi;
    proximity_class_t proximity;
    presence_state_t state;
    rssi_smoother_t rssi;
    bool in_use;
} device_t;

typedef struct {
    const ble_addr_t *addr;
    int rssi;
    const char *name;
    const uint8_t *mfg_data;
    size_t mfg_len;
} advertisement_t;

void device_tracker_init(void);
void device_tracker_on_advertisement(const advertisement_t *adv);
void device_tracker_sweep(void);

void on_device_detected(device_t *dev);
void on_device_updated(device_t *dev);
void on_device_lost(device_t *dev);

const char *device_addr_to_str(const ble_addr_t *addr, char *buf, size_t len);
const char *proximity_class_to_str(proximity_class_t proximity);

#ifdef __cplusplus
}
#endif

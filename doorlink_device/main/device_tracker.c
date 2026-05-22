#include "device_tracker.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "device_tracker";

static device_t s_devices[MAX_TRACKED_DEVICES];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

static bool prefix_matches(const char *value, const char *prefix)
{
    if (prefix == NULL || prefix[0] == '\0') {
        return true;
    }

    if (value == NULL || value[0] == '\0') {
        return false;
    }

    return strncmp(value, prefix, strlen(prefix)) == 0;
}

static const char *bytes_to_hex(const uint8_t *data, size_t data_len, char *buf, size_t len)
{
    if (data == NULL || data_len == 0 || len == 0) {
        return "(none)";
    }

    size_t offset = 0;
    for (size_t i = 0; i < data_len && offset + 3 < len; ++i) {
        offset += snprintf(buf + offset, len - offset, "%02X", data[i]);
    }
    buf[offset] = '\0';
    return buf;
}

const char *device_addr_to_str(const ble_addr_t *addr, char *buf, size_t len)
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X/%u",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0], addr->type);
    return buf;
}

static device_t *find_device(const ble_addr_t *addr)
{
    for (size_t i = 0; i < MAX_TRACKED_DEVICES; ++i) {
        if (s_devices[i].in_use && addr_equal(&s_devices[i].addr, addr)) {
            return &s_devices[i];
        }
    }

    return NULL;
}

static device_t *alloc_device(const ble_addr_t *addr)
{
    for (size_t i = 0; i < MAX_TRACKED_DEVICES; ++i) {
        if (!s_devices[i].in_use) {
            device_t *dev = &s_devices[i];
            memset(dev, 0, sizeof(*dev));
            dev->addr = *addr;
            dev->state = DEVICE_ABSENT;
            dev->proximity = PROXIMITY_FAR;
            dev->in_use = true;
            proximity_rssi_init(&dev->rssi);
            return dev;
        }
    }

    return NULL;
}

static device_t *evict_stale_device(void)
{
    device_t *candidate = NULL;

    for (size_t i = 0; i < MAX_TRACKED_DEVICES; ++i) {
        device_t *dev = &s_devices[i];
        if (!dev->in_use || dev->state == DEVICE_PRESENT) {
            continue;
        }

        if (candidate == NULL || dev->last_seen_ms < candidate->last_seen_ms) {
            candidate = dev;
        }
    }

    if (candidate != NULL) {
        memset(candidate, 0, sizeof(*candidate));
    }

    return candidate;
}

void device_tracker_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
}

void device_tracker_on_advertisement(const advertisement_t *adv)
{
    if (adv->rssi < MIN_TRACKED_RSSI_DBM) {
        return;
    }

    if (ENABLE_NAME_FILTER && !prefix_matches(adv->name, NAME_FILTER_PREFIX)) {
        return;
    }

    const uint32_t seen_ms = now_ms();
    device_t *dev = find_device(adv->addr);
    if (dev == NULL) {
        dev = alloc_device(adv->addr);
        if (dev == NULL) {
            dev = evict_stale_device();
            if (dev == NULL) {
                ESP_LOGW(TAG, "Device table full");
                return;
            }

            dev->addr = *adv->addr;
            dev->state = DEVICE_ABSENT;
            dev->proximity = PROXIMITY_FAR;
            dev->in_use = true;
            proximity_rssi_init(&dev->rssi);
        }
    }

    if (dev->first_seen_ms == 0) {
        dev->first_seen_ms = seen_ms;
    }

    if (dev->state == DEVICE_ABSENT) {
        dev->state = DEVICE_PRESENT;
        char addr[32];
        char mfg_hex[(MAX_MFG_DATA_LEN * 2) + 1];
        ESP_LOGI(TAG, "DETECTED addr=%s name=%s rssi=%d smoothed=%d proximity=%s count=%u mfg=%s",
                 device_addr_to_str(&dev->addr, addr, sizeof(addr)),
                 adv->name[0] ? adv->name : "(none)",
                 adv->rssi,
                 adv->rssi,
                 proximity_class_to_str(proximity_classify(adv->rssi)),
                 (unsigned)1,
                 bytes_to_hex(adv->mfg_data, adv->mfg_len, mfg_hex, sizeof(mfg_hex)));
    }

    dev->last_seen_ms = seen_ms;
    dev->last_rssi = adv->rssi;
    dev->smoothed_rssi = proximity_rssi_update(&dev->rssi, adv->rssi);
    dev->proximity = proximity_classify(dev->smoothed_rssi);
    dev->detected_count++;

    if (adv->name != NULL && adv->name[0] != '\0') {
        strlcpy(dev->name, adv->name, sizeof(dev->name));
    }
}

void device_tracker_sweep(void)
{
    const uint32_t current_ms = now_ms();

    for (size_t i = 0; i < MAX_TRACKED_DEVICES; ++i) {
        device_t *dev = &s_devices[i];
        if (!dev->in_use) {
            continue;
        }

        if (dev->state == DEVICE_PRESENT &&
            current_ms - dev->last_seen_ms > DEVICE_STALE_TIMEOUT_MS) {
            dev->state = DEVICE_ABSENT;

            char addr[32];
            ESP_LOGI(TAG, "LOST addr=%s name=%s last_seen_ms_ago=%" PRIu32 " count=%u",
                     device_addr_to_str(&dev->addr, addr, sizeof(addr)),
                     dev->name[0] ? dev->name : "(none)",
                     current_ms - dev->last_seen_ms,
                     (unsigned)dev->detected_count);
        }

        if (current_ms - dev->last_seen_ms > (DEVICE_STALE_TIMEOUT_MS * 2U)) {
            memset(dev, 0, sizeof(*dev));
        }
    }
}

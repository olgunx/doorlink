#include "ble_scanner.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_scanner";
static uint8_t s_own_addr_type;
static lighthouse_claim_detected_cb_t s_claim_cb;
static bool s_adv_disabled;
static uint8_t s_challenge_nonce[LIGHTHOUSE_CHALLENGE_LEN];
static bool s_challenge_set;
static volatile uint32_t s_disc_count;
static volatile uint32_t s_claim_count;
static volatile bool s_mac_rotation_pending = false;
static ble_uuid16_t s_static_uuid16 = BLE_UUID16_INIT(0xFCD2);
static bool s_host_synced = false;

#define RADIO_STATE_SCANNING    (1 << 0)
#define RADIO_STATE_ADVERTISING (1 << 1)
static volatile uint8_t s_radio_state = 0;

static void start_scan(void);
static void start_adv(void);
static void refresh_adv(void);

extern void trigger_manual_unlock(void);

void ble_scanner_set_claim_detected_cb(lighthouse_claim_detected_cb_t cb)
{
    s_claim_cb = cb;
}

esp_err_t ble_scanner_set_static_uuid_beacon(const uint8_t *uuid_16_bytes)
{
    if (uuid_16_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ble_scanner_set_challenge_beacon(const uint8_t *challenge_8_bytes)
{
    if (challenge_8_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_challenge_nonce, challenge_8_bytes, LIGHTHOUSE_CHALLENGE_LEN);
    s_challenge_set = true;
    refresh_adv();
    return ESP_OK;
}

static bool extract_claim(const struct ble_hs_adv_fields *fields, lighthouse_ble_claim_t *claim)
{
    const size_t expected_len = LIGHTHOUSE_USER_ID_LEN + LIGHTHOUSE_CHALLENGE_LEN + LIGHTHOUSE_RESPONSE_LEN;
    const uint16_t expected_company_id_le = 0x0143;
    if (fields->mfg_data == NULL) {
        return false;
    }

    const uint8_t *p = NULL;
    if (fields->mfg_data_len == expected_len + 2) {
        // Layout A: [company_id_le(2)] + [user_id(4) + challenge(8) + response(16)].
        const uint16_t company_id = (uint16_t)fields->mfg_data[0] | ((uint16_t)fields->mfg_data[1] << 8);
        if (company_id != expected_company_id_le) {
            return false;
        }
        p = fields->mfg_data + 2;
    } else if (fields->mfg_data_len == expected_len) {
        // Layout B: [user_id(4) + challenge(8) + response(16)].
        p = fields->mfg_data;
    } else {
        return false;
    }

    memcpy(claim->user_id, p, LIGHTHOUSE_USER_ID_LEN);
    memcpy(claim->challenge, p + LIGHTHOUSE_USER_ID_LEN, LIGHTHOUSE_CHALLENGE_LEN);
    memcpy(claim->response, p + LIGHTHOUSE_USER_ID_LEN + LIGHTHOUSE_CHALLENGE_LEN, LIGHTHOUSE_RESPONSE_LEN);
    s_claim_count++;
    return true;
}

static int ble_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        lighthouse_ble_claim_t claim = {0};
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        s_disc_count++;

        if (rc == 0) {
            if (fields.uuids16 != NULL) {
                for (int i = 0; i < fields.num_uuids16; i++) {
                    if (fields.uuids16[i].value == 0xFCD3) {
                        trigger_manual_unlock();
                    }
                }
            }
            if (fields.uuids128 != NULL) {
                static const uint8_t manual_uuid128[16] = {
                    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                    0x00, 0x10, 0x00, 0x00, 0xd3, 0xfc, 0x00, 0x00
                };
                for (int i = 0; i < fields.num_uuids128; i++) {
                    if (memcmp(fields.uuids128[i].value, manual_uuid128, 16) == 0) {
                        trigger_manual_unlock();
                    }
                }
            }
        }

        if (rc != 0 || !extract_claim(&fields, &claim)) {
            return 0;
        }
        claim.rssi = event->disc.rssi;
        if (s_claim_cb != NULL) {
            s_claim_cb(&claim);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        __atomic_fetch_and(&s_radio_state, ~RADIO_STATE_SCANNING, __ATOMIC_SEQ_CST);
        if (s_mac_rotation_pending) {
            if (s_radio_state == 0) {
                s_mac_rotation_pending = false;
                ble_addr_t rnd_addr;
                if (ble_hs_id_gen_rnd(1, &rnd_addr) == 0) {
                    ble_hs_id_set_rnd(rnd_addr.val);
                    s_own_addr_type = BLE_OWN_ADDR_RANDOM;
                }
                start_scan();
                start_adv();
            }
        } else {
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        __atomic_fetch_and(&s_radio_state, ~RADIO_STATE_ADVERTISING, __ATOMIC_SEQ_CST);
        if (s_mac_rotation_pending) {
            if (s_radio_state == 0) {
                s_mac_rotation_pending = false;
                ble_addr_t rnd_addr;
                if (ble_hs_id_gen_rnd(1, &rnd_addr) == 0) {
                    ble_hs_id_set_rnd(rnd_addr.val);
                    s_own_addr_type = BLE_OWN_ADDR_RANDOM;
                }
                start_scan();
                start_adv();
            }
        } else {
            start_adv();
        }
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params scan_params = {
        .itvl = BLE_SCAN_INTERVAL_UNITS,
        .window = BLE_SCAN_WINDOW_UNITS,
        .filter_policy = 0,
        .limited = 0,
        .passive = BLE_SCAN_ACTIVE ? 0 : 0,
        .filter_duplicates = BLE_SCAN_FILTER_DUPLICATES,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &scan_params, ble_gap_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        __atomic_fetch_or(&s_radio_state, RADIO_STATE_SCANNING, __ATOMIC_SEQ_CST);
    } else {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
    }
}

static void start_adv(void)
{
    if (s_adv_disabled) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.uuids16 = &s_static_uuid16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    if (s_challenge_set) {
        static uint8_t challenge_mfg_buf[2 + LIGHTHOUSE_CHALLENGE_LEN];
        challenge_mfg_buf[0] = 0x44;
        challenge_mfg_buf[1] = 0x01;
        memcpy(&challenge_mfg_buf[2], s_challenge_nonce, LIGHTHOUSE_CHALLENGE_LEN);
        fields.mfg_data = challenge_mfg_buf;
        fields.mfg_data_len = sizeof(challenge_mfg_buf);
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble adv disabled (set_fields rc=%d); scanner remains active", rc);
        s_adv_disabled = true;
        return;
    }


    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        __atomic_fetch_or(&s_radio_state, RADIO_STATE_ADVERTISING, __ATOMIC_SEQ_CST);
    } else {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
    }
}

static void refresh_adv(void)
{
    if (s_adv_disabled || !s_host_synced) {
        return;
    }

    s_mac_rotation_pending = true;

    if (s_radio_state & RADIO_STATE_ADVERTISING) {
        if (ble_gap_adv_stop() != 0) {
            __atomic_fetch_and(&s_radio_state, ~RADIO_STATE_ADVERTISING, __ATOMIC_SEQ_CST);
        }
    }
    if (s_radio_state & RADIO_STATE_SCANNING) {
        if (ble_gap_disc_cancel() != 0) {
            __atomic_fetch_and(&s_radio_state, ~RADIO_STATE_SCANNING, __ATOMIC_SEQ_CST);
        }
    }

    if (s_radio_state == 0) {
        s_mac_rotation_pending = false;
        ble_addr_t rnd_addr;
        if (ble_hs_id_gen_rnd(1, &rnd_addr) == 0) {
            ble_hs_id_set_rnd(rnd_addr.val);
            s_own_addr_type = BLE_OWN_ADDR_RANDOM;
        }
        start_scan();
        start_adv();
    }
}

static void ble_on_sync(void)
{
    s_host_synced = true;
    ble_addr_t rnd_addr;
    if (ble_hs_id_gen_rnd(1, &rnd_addr) == 0) {
        ble_hs_id_set_rnd(rnd_addr.val);
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        ESP_LOGW(TAG, "failed to gen random addr, fallback to auto");
        ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    start_scan();
    start_adv();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scanner_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

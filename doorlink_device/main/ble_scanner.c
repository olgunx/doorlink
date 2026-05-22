#include "ble_scanner.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_scanner";
static uint8_t s_own_addr_type;
static lighthouse_claim_detected_cb_t s_claim_cb;
static ble_uuid128_t s_static_uuid;
static bool s_uuid_set;
static bool s_adv_disabled;

static void start_scan(void);
static void start_adv(void);

void ble_scanner_set_claim_detected_cb(lighthouse_claim_detected_cb_t cb)
{
    s_claim_cb = cb;
}

esp_err_t ble_scanner_set_static_uuid_beacon(const uint8_t *uuid_16_bytes)
{
    if (uuid_16_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_static_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(s_static_uuid.value, uuid_16_bytes, sizeof(s_static_uuid.value));
    s_uuid_set = true;
    return ESP_OK;
}

static bool extract_claim(const struct ble_hs_adv_fields *fields, lighthouse_ble_claim_t *claim)
{
    const size_t expected_len = LIGHTHOUSE_USER_ID_LEN + LIGHTHOUSE_TOKEN_LEN;
    const uint16_t expected_company_id_le = 0x0143;
    if (fields->mfg_data == NULL) {
        return false;
    }

    const uint8_t *p = NULL;
    if (fields->mfg_data_len == expected_len + 2) {
        // Layout A: [company_id_le(2)] + [user_id(4) + token(16)].
        const uint16_t company_id = (uint16_t)fields->mfg_data[0] | ((uint16_t)fields->mfg_data[1] << 8);
        if (company_id != expected_company_id_le) {
            return false;
        }
        p = fields->mfg_data + 2;
    } else if (fields->mfg_data_len == expected_len) {
        // Layout B: [user_id(4) + token(16)] (some stacks expose data without company ID).
        p = fields->mfg_data;
    } else {
        return false;
    }

    memcpy(claim->user_id, p, LIGHTHOUSE_USER_ID_LEN);
    memcpy(claim->token, p + LIGHTHOUSE_USER_ID_LEN, LIGHTHOUSE_TOKEN_LEN);
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
        start_scan();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_adv();
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
        .passive = BLE_SCAN_ACTIVE ? 0 : 1,
        .filter_duplicates = BLE_SCAN_FILTER_DUPLICATES,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &scan_params, ble_gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
    }
}

static void start_adv(void)
{
    if (s_adv_disabled) {
        return;
    }
    if (!s_uuid_set) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_static_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble adv disabled (set_fields rc=%d); scanner remains active", rc);
        s_adv_disabled = true;
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
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

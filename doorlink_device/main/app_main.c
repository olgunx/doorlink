#include "ble_scanner.h"
#include "config.h"
#include "vl6180x.h"
#include "enrollment_mgr.h"
#include "web_console.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG = "lighthouse";
static const char *NVS_NS = "lighthouse";
static const uint32_t TOKEN_ROTATE_INTERVAL_MS = 10000;

// LSB-first memory representation for NimBLE: 9f82c41d-3b7a-4291-a1e6-b5293d0cfa82
static const uint8_t STATIC_UUID[16] = {0x82, 0xfa, 0x0c, 0x3d, 0x29, 0xb5, 0xe6, 0xa1, 0x91, 0x42, 0x7a, 0x3b, 0x1d, 0xc4, 0x82, 0x9f};

static volatile bool s_authorized;
static volatile bool s_arrived;
static volatile bool s_laser_detected;
static volatile uint32_t s_relay_active_until_ms;
static volatile uint32_t s_auth_window_started_ms;
static volatile uint32_t s_last_claim_ms;
static volatile int s_last_claim_rssi = -127;
static volatile uint32_t s_claim_rx_count;
static volatile uint32_t s_claim_match_count;
static volatile uint32_t s_last_mismatch_log_ms;
static uint8_t s_last_user_id[LIGHTHOUSE_USER_ID_LEN];
static uint8_t s_active_token[LIGHTHOUSE_TOKEN_LEN];

static uint32_t s_current_sequence_index = 0;
static char s_current_seed[64] = {0};

static nvs_handle_t s_nvs = 0;
static uint8_t s_vendor_ie_buf_a[sizeof(vendor_ie_data_t) + LIGHTHOUSE_TOKEN_LEN] __attribute__((aligned(4)));
static uint8_t s_vendor_ie_buf_b[sizeof(vendor_ie_data_t) + LIGHTHOUSE_TOKEN_LEN] __attribute__((aligned(4)));
static uint8_t *s_vendor_ie_active = NULL;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static void set_relay(bool on) { gpio_set_level(RELAY_GPIO, RELAY_ACTIVE_LOW ? !on : on); }

static esp_err_t update_hidden_ap_token_ie(void)
{
    // Safely remove the currently active IE before injecting the new one
    if (s_vendor_ie_active != NULL) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, s_vendor_ie_active);
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, s_vendor_ie_active);
    }

    uint8_t *next_buf = (s_vendor_ie_active == s_vendor_ie_buf_a) ? s_vendor_ie_buf_b : s_vendor_ie_buf_a;
    vendor_ie_data_t *v = (vendor_ie_data_t *)next_buf;
    v->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    // Stealth Token Injection 6 bytes payload
    v->length = 4 + 6;
    // Use Apple OUI (00 17 F2) to bypass filters without conflicting with WPA2 IEs
    v->vendor_oui[0] = 0x00;
    v->vendor_oui[1] = 0x17;
    v->vendor_oui[2] = 0xF2;
    v->vendor_oui_type = 0x42;
    memcpy(v->payload, s_active_token, 6);
    ESP_LOGI(TAG, "vendor IE update: eid=%02x len=%u tok0=%02x tok1=%02x tok2=%02x tok3=%02x",
             (unsigned int)v->element_id,
             (unsigned int)v->length,
             (unsigned int)s_active_token[0],
             (unsigned int)s_active_token[1],
             (unsigned int)s_active_token[2],
             (unsigned int)s_active_token[3]);
    esp_err_t err = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, v);
    esp_err_t err2 = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, v);
    if (err != ESP_OK || err2 != ESP_OK) {
        ESP_LOGW(TAG, "enable vendor IE failed: %s", esp_err_to_name(err));
    } else {
        s_vendor_ie_active = next_buf;
    }
    return err;
}

static void update_stealth_token(void)
{
    size_t len = sizeof(s_current_seed);
    if (nvs_get_str(s_nvs, "seed", s_current_seed, &len) != ESP_OK || strlen(s_current_seed) == 0) {
        return; // No seed provisioned yet
    }
    nvs_get_u32(s_nvs, "seq_idx", &s_current_sequence_index);

    uint8_t hash[32] = {0};
    for (uint32_t i = 0; i <= s_current_sequence_index; i++) {
        if (i == 0) {
            mbedtls_sha256((const unsigned char*)s_current_seed, strlen(s_current_seed), hash, 0);
        } else {
            mbedtls_sha256(hash, 32, hash, 0);
        }
    }

    memset(s_active_token, 0, LIGHTHOUSE_TOKEN_LEN);
    memcpy(s_active_token, hash, (LIGHTHOUSE_TOKEN_LEN < 6) ? LIGHTHOUSE_TOKEN_LEN : 6);

    s_authorized = false;
    s_auth_window_started_ms = 0;

    esp_err_t err = update_hidden_ap_token_ie();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stealth IE injection update failed");
    }
}

static void claim_detected(const lighthouse_ble_claim_t *claim)
{
    if (claim == NULL) {
        return;
    }
    s_claim_rx_count++;
    s_last_claim_ms = now_ms();
    s_last_claim_rssi = claim->rssi;
    s_arrived = claim->rssi >= BEACON_ARRIVED_RSSI_DBM;
    memcpy(s_last_user_id, claim->user_id, LIGHTHOUSE_USER_ID_LEN);
    
    if (!enrollment_mgr_is_device_active(claim->user_id, NULL)) {
        return; // Ignore claims from unenrolled devices
    }
    
    if (memcmp(claim->token, s_active_token, LIGHTHOUSE_TOKEN_LEN) == 0) {
        s_claim_match_count++;
        s_authorized = true;
        s_auth_window_started_ms = now_ms();
    } else {
        const uint32_t now = now_ms();
        if (s_last_mismatch_log_ms == 0 || (now - s_last_mismatch_log_ms) >= 2000) {
            s_last_mismatch_log_ms = now;
            ESP_LOGI(TAG, "claim mismatch claim=%02x%02x%02x%02x active=%02x%02x%02x%02x",
                     (unsigned int)claim->token[0],
                     (unsigned int)claim->token[1],
                     (unsigned int)claim->token[2],
                     (unsigned int)claim->token[3],
                     (unsigned int)s_active_token[0],
                     (unsigned int)s_active_token[1],
                     (unsigned int)s_active_token[2],
                     (unsigned int)s_active_token[3]);
        }
    }
}

static void status_task(void *arg)
{
    (void)arg;
    bool prev_comm = false;
    bool prev_auth = false;
    bool prev_arrived = false;
    bool prev_laser = false;
    bool prev_relay = false;
    uint32_t prev_rx = UINT32_MAX;
    uint32_t prev_match = UINT32_MAX;
    uint32_t last_log_ms = 0;
    while (true) {
        const uint32_t now = now_ms();
        const uint32_t last_claim_age_ms = s_last_claim_ms == 0 ? 0 : (now - s_last_claim_ms);
        const bool comm = s_last_claim_ms != 0 && last_claim_age_ms <= 10000;
        const bool auth = s_authorized;
        const bool arrived = s_arrived;
        const bool laser = s_laser_detected;
        const bool relay = s_relay_active_until_ms && (int32_t)(s_relay_active_until_ms - now) > 0;
        const uint32_t rx = s_claim_rx_count;
        const uint32_t match = s_claim_match_count;
        const bool changed = comm != prev_comm || auth != prev_auth || arrived != prev_arrived || laser != prev_laser ||
                             relay != prev_relay ||
                             rx != prev_rx || match != prev_match;
        const bool heartbeat = (now - last_log_ms) >= 10000;

        if (changed || heartbeat) {
            ESP_LOGI(TAG, "APP=%s AUTH=%s ARRIVED=%s LASER=%s RELAY=%s RX=%u MATCH=%u",
                     comm ? "OK" : "LOST",
                     auth ? "Y" : "N",
                     arrived ? "Y" : "N",
                     laser ? "Y" : "N",
                     relay ? "ON" : "OFF",
                     (unsigned int)rx,
                     (unsigned int)match);
            prev_comm = comm;
            prev_auth = auth;
            prev_arrived = arrived;
            prev_laser = laser;
            prev_relay = relay;
            prev_rx = rx;
            prev_match = match;
            last_log_ms = now;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void token_rotate_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TOKEN_ROTATE_INTERVAL_MS));
        update_stealth_token();
    }
}

static esp_err_t init_wifi_hidden_ap(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default");
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, "DL_DOOR", 7);
    ap.ap.ssid_len = 7;
    memcpy(ap.ap.password, "doorl1223", 9);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.ssid_hidden = 0;
    ap.ap.max_connection = 1;
    ap.ap.beacon_interval = 100;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    return ESP_OK;
}

static esp_err_t init_vl6180x_bus(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = VL6180X_I2C_SDA_GPIO,
        .scl_io_num = VL6180X_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = VL6180X_I2C_CLK_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(VL6180X_I2C_PORT, &i2c_conf), TAG, "i2c_param_config");
    esp_err_t err = i2c_driver_install(VL6180X_I2C_PORT, i2c_conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return vl6180x_init(VL6180X_I2C_PORT);
}

static void sensor_task(void *arg)
{
    (void)arg;
    while (true) {
        uint16_t distance_mm = 0;
        if (vl6180x_read_range_mm(VL6180X_I2C_PORT, &distance_mm) == ESP_OK) {
            s_laser_detected = distance_mm > 0 && distance_mm <= VL6180X_PRESENCE_THRESHOLD_MM;
        } else {
            s_laser_detected = false;
        }

        if (s_authorized && s_arrived && s_laser_detected) {
            s_relay_active_until_ms = now_ms() + RELAY_TRIGGER_MS;
            
            // Instant Burn: Fast-forward chain index
            s_current_sequence_index++;
            nvs_set_u32(s_nvs, "seq_idx", s_current_sequence_index);
            nvs_commit(s_nvs);
            update_stealth_token(); // Roll immediately
            
            s_authorized = false;
        }
        vTaskDelay(pdMS_TO_TICKS(VL6180X_POLL_MS));
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    bool relay_on = false;
    bool auth_led_on = false;
    
    gpio_reset_pin(ALIVE_LED_GPIO); // GPIO 12 Auth LED
    gpio_set_direction(ALIVE_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(DETECTION_LED_GPIO); // GPIO 13 Open LED
    gpio_set_direction(DETECTION_LED_GPIO, GPIO_MODE_OUTPUT);

    while (true) {
        bool should_on = s_relay_active_until_ms && (int32_t)(s_relay_active_until_ms - now_ms()) > 0;
        if (should_on != relay_on) {
            relay_on = should_on;
            set_relay(relay_on);
            gpio_set_level(DETECTION_LED_GPIO, relay_on ? 1 : 0);
        }
        if (s_authorized != auth_led_on) {
            auth_led_on = s_authorized;
            gpio_set_level(ALIVE_LED_GPIO, auth_led_on ? 1 : 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &s_nvs));
    ESP_ERROR_CHECK(enrollment_mgr_init());

    gpio_reset_pin(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    set_relay(false);

    ESP_ERROR_CHECK(init_vl6180x_bus());
    ESP_ERROR_CHECK(init_wifi_hidden_ap());
    update_stealth_token(); // Load NVS Seed & Init Token (requires Wi-Fi to be initialized first)
    ESP_ERROR_CHECK(web_console_init());
    ESP_ERROR_CHECK(ble_scanner_set_static_uuid_beacon(STATIC_UUID));
    ble_scanner_set_claim_detected_cb(claim_detected);
    ESP_ERROR_CHECK(ble_scanner_init());

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(relay_task, "relay_task", 2048, NULL, 5, NULL);
    xTaskCreate(token_rotate_task, "token_rotate_task", 3072, NULL, 5, NULL);
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
}

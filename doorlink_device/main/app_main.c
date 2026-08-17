#include "ble_scanner.h"
#include "config.h"
#include "device_key.h"
#include "proximity_engine.h"
#include "vl53l0x.h"
#include "vl6180x.h"
#include "enrollment_mgr.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "mbedtls/md.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

static const char *TAG = "lighthouse";
static const char *NVS_NS = "lighthouse";
static const uint32_t TOKEN_ROTATE_INTERVAL_MS = 15000;
static const uint32_t CLAIM_IDLE_TIMEOUT_MS = 20000;

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
static uint8_t s_previous_token[LIGHTHOUSE_TOKEN_LEN];
static volatile uint32_t s_previous_token_valid_until_ms;
static uint8_t s_active_challenge[LIGHTHOUSE_CHALLENGE_LEN];
static uint8_t s_previous_challenge[LIGHTHOUSE_CHALLENGE_LEN];
static volatile uint32_t s_previous_challenge_valid_until_ms;
static bool s_enrolled_pubkey_logged;
static bool s_device_pubkey_logged = false;
static volatile bool g_hw_failure = false;
static volatile bool g_sw_failure = false;

static nvs_handle_t s_nvs = 0;
static QueueHandle_t s_claim_queue = NULL;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

volatile uint32_t g_manual_unlock_until_ms = 0;

void trigger_manual_unlock(void) {
    g_manual_unlock_until_ms = now_ms() + 5000;
}

static const char *bytes_to_hex(const uint8_t *bytes, size_t len, char *buf, size_t buf_size)
{
    if (bytes == NULL || buf == NULL || buf_size == 0) {
        return "(null)";
    }
    size_t offset = 0;
    for (size_t i = 0; i < len && offset + 2 < buf_size; ++i) {
        offset += snprintf(buf + offset, buf_size - offset, "%02x", bytes[i]);
    }
    buf[offset] = '\0';
    return buf;
}
static void format_uptime(char *buf, size_t buf_size, uint32_t ms)
{
    const uint32_t hours = ms / 3600000U;
    const uint32_t minutes = (ms / 60000U) % 60U;
    const uint32_t seconds = (ms / 1000U) % 60U;
    const uint32_t millis = ms % 1000U;
    snprintf(buf, buf_size, "%02u:%02u:%02u.%03u",
             (unsigned int)hours,
             (unsigned int)minutes,
             (unsigned int)seconds,
             (unsigned int)millis);
}
static void set_relay(bool on) { gpio_set_level(RELAY_GPIO, RELAY_ACTIVE_LOW ? !on : on); }

void get_system_status_bytes(uint8_t *out_buf) {
    if (!out_buf) return;
    // Byte 0: Relay Status, Byte 1: Laser Status, Byte 2: Auth Window Active
    out_buf[0] = (s_relay_active_until_ms && (int32_t)(s_relay_active_until_ms - now_ms()) > 0) ? 1 : 0;
    out_buf[1] = s_laser_detected ? 1 : 0;
    out_buf[2] = s_authorized ? 1 : 0;
}

static void update_stealth_token(void)
{
    memcpy(s_previous_token, s_active_token, LIGHTHOUSE_TOKEN_LEN);
    memset(s_active_token, 0, LIGHTHOUSE_TOKEN_LEN);
    for (size_t i = 0; i < LIGHTHOUSE_TOKEN_LEN; i++) {
        s_active_token[i] = (uint8_t)esp_random();
    }
    s_previous_token_valid_until_ms = now_ms() + 10000;

    memcpy(s_previous_challenge, s_active_challenge, LIGHTHOUSE_CHALLENGE_LEN);
    for (size_t i = 0; i < LIGHTHOUSE_CHALLENGE_LEN; i += sizeof(uint32_t)) {
        uint32_t rnd = esp_random();
        size_t chunk = (LIGHTHOUSE_CHALLENGE_LEN - i) < sizeof(uint32_t) ? (LIGHTHOUSE_CHALLENGE_LEN - i) : sizeof(uint32_t);
        memcpy(&s_active_challenge[i], &rnd, chunk);
    }
    s_previous_challenge_valid_until_ms = now_ms() + 5000;

    ble_scanner_set_challenge_beacon(s_active_challenge);
}

static int ecdh_rng_wrapper(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static bool compute_expected_response(const uint8_t *user_id, const uint8_t *challenge, const uint8_t *peer_pubkey, uint8_t out[LIGHTHOUSE_RESPONSE_LEN])
{
    if (user_id == NULL || challenge == NULL || peer_pubkey == NULL || out == NULL) {
        return false;
    }

    uint8_t input[LIGHTHOUSE_USER_ID_LEN + LIGHTHOUSE_CHALLENGE_LEN];
    memcpy(input, user_id, LIGHTHOUSE_USER_ID_LEN);
    memcpy(input + LIGHTHOUSE_USER_ID_LEN, challenge, LIGHTHOUSE_CHALLENGE_LEN);

    uint8_t shared_secret[32] = {0};
    mbedtls_ecp_group grp;
    mbedtls_ecp_point peer_pub;
    mbedtls_mpi priv;
    mbedtls_mpi z;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&peer_pub);
    mbedtls_mpi_init(&priv);
    mbedtls_mpi_init(&z);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: ecp_group_load failed ret=%d", ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    const uint8_t *esp_private_key = device_key_get_private();
    if (esp_private_key == NULL) {
        ESP_LOGE(TAG, "compute_expected_response: ESP private key unavailable");
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    ret = mbedtls_mpi_read_binary(&priv, esp_private_key, DEVICE_KEY_PRIV_LEN);
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: mpi_read_binary(private) failed ret=%d", ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    ret = mbedtls_ecp_point_read_binary(&grp, &peer_pub, peer_pubkey, ENROLLMENT_PUBKEY_LEN);
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: point_read_binary(peer_pubkey) failed ret=%d", ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    ret = mbedtls_ecdh_compute_shared(&grp, &z, &peer_pub, &priv, ecdh_rng_wrapper, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: ecdh_compute_shared failed ret=%d", ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    ret = mbedtls_mpi_write_binary(&z, shared_secret, sizeof(shared_secret));
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: mpi_write_binary(shared_secret) failed ret=%d", ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&peer_pub);
        mbedtls_mpi_free(&priv);
        mbedtls_mpi_free(&z);
        return false;
    }

    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_point_free(&peer_pub);
    mbedtls_mpi_free(&priv);
    mbedtls_mpi_free(&z);

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return false;
    }

    uint8_t full_mac[32];
    ret = mbedtls_md_hmac(md_info, shared_secret, sizeof(shared_secret), input, sizeof(input), full_mac);
    if (ret != 0) {
        ESP_LOGE(TAG, "compute_expected_response: md_hmac failed ret=%d", ret);
        return false;
    }
    memcpy(out, full_mac, LIGHTHOUSE_RESPONSE_LEN);
    return true;
}

static void auth_task(void *arg)
{
    (void)arg;
    lighthouse_ble_claim_t claim;
    while (true) {
        if (xQueueReceive(s_claim_queue, &claim, portMAX_DELAY)) {
            uint8_t enrolled_pubkey[ENROLLMENT_PUBKEY_LEN] = {0};
            if (enrollment_mgr_is_device_active(claim.user_id, enrolled_pubkey)) {
                if (!s_enrolled_pubkey_logged) {
                    s_enrolled_pubkey_logged = true;
                    char pubkey_hex[ENROLLMENT_PUBKEY_LEN * 2 + 1];
                    ESP_LOGI(TAG, "enrolled pubkey=%s",
                             bytes_to_hex(enrolled_pubkey, ENROLLMENT_PUBKEY_LEN, pubkey_hex, sizeof(pubkey_hex)));
                }

                uint8_t expected_response[LIGHTHOUSE_RESPONSE_LEN] = {0};
                const bool response_ok = compute_expected_response(claim.user_id, claim.challenge, enrolled_pubkey, expected_response) &&
                                         memcmp(claim.response, expected_response, LIGHTHOUSE_RESPONSE_LEN) == 0;

                // Re-evaluate matches AFTER heavy crypto to avoid race with sensor_task burn
                const bool active_challenge_match = memcmp(claim.challenge, s_active_challenge, LIGHTHOUSE_CHALLENGE_LEN) == 0;
                const bool previous_challenge_match = s_previous_challenge_valid_until_ms != 0 &&
                                                      now_ms() <= s_previous_challenge_valid_until_ms &&
                                                      memcmp(claim.challenge, s_previous_challenge, LIGHTHOUSE_CHALLENGE_LEN) == 0;

                if ((active_challenge_match || previous_challenge_match) &&
                    response_ok) {
                    s_claim_match_count++;
                    s_authorized = true;
                    s_auth_window_started_ms = now_ms();
                } else {
                    const uint32_t now = now_ms();
                    if (s_last_mismatch_log_ms == 0 || (now - s_last_mismatch_log_ms) >= 2000) {
                        s_last_mismatch_log_ms = now;
                    }
                }
            }
            // Yield to allow IDLE task to run and reset watchdog in case of a heavy flood of BLE claims
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

#define ARRIVED_LEAVE_RSSI_DBM  (BEACON_ARRIVED_RSSI_DBM - 10)  /* -83 dBm: must drop 10 dB to un-arrive */

static rssi_smoother_t s_claim_rssi_smoother;
static bool s_smoother_initialized = false;

static void claim_detected(const lighthouse_ble_claim_t *claim)
{
    if (claim == NULL) {
        return;
    }
    if (!s_smoother_initialized) {
        proximity_rssi_init(&s_claim_rssi_smoother);
        s_smoother_initialized = true;
    }
    const int smoothed = proximity_rssi_update(&s_claim_rssi_smoother, claim->rssi);
    s_claim_rx_count++;
    s_last_claim_ms = now_ms();
    s_last_claim_rssi = smoothed;
    if (!s_arrived) {
        s_arrived = smoothed >= BEACON_ARRIVED_RSSI_DBM;
    } else {
        s_arrived = smoothed >= ARRIVED_LEAVE_RSSI_DBM;
    }
    memcpy(s_last_user_id, claim->user_id, LIGHTHOUSE_USER_ID_LEN);

    // Send to queue to offload heavy crypto from nimble_host task
    if (s_claim_queue != NULL) {
        xQueueOverwrite(s_claim_queue, claim);
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
    while (true) {
        const uint32_t now = now_ms();
        const uint32_t last_claim_age_ms = s_last_claim_ms == 0 ? 0 : (now - s_last_claim_ms);

        if (s_last_claim_ms != 0 && last_claim_age_ms > CLAIM_IDLE_TIMEOUT_MS) {
            s_arrived = false; // Reset arrived status if app goes out of range
            s_authorized = false; // Drop auth if phone leaves the area completely
        }
        
        if (s_authorized && s_auth_window_started_ms != 0 && (now - s_auth_window_started_ms) > CLAIM_IDLE_TIMEOUT_MS) {
            s_authorized = false; // Drop auth if we haven't received a valid cryptogram recently
        }

        const bool comm = s_last_claim_ms != 0 && last_claim_age_ms <= CLAIM_IDLE_TIMEOUT_MS;
        const bool auth = s_authorized;
        const bool arrived = s_arrived;
        const bool laser = s_laser_detected;
        const bool relay = s_relay_active_until_ms && (int32_t)(s_relay_active_until_ms - now) > 0;
        const uint32_t rx = s_claim_rx_count;
        const uint32_t match = s_claim_match_count;
        const bool changed = comm != prev_comm || auth != prev_auth || arrived != prev_arrived || laser != prev_laser ||
                             relay != prev_relay;

        if (changed) {
            char ts[16];
            format_uptime(ts, sizeof(ts), now);
            ESP_LOGI(TAG, "[%s] APP[%s]-AUTH[%s]-ARRV[%s]-LSR[%s]-REL[%s] RX=%u MTCH=%u",
                     ts,
                     comm ? "X" : " ",
                     auth ? "X" : " ",
                     arrived ? "X" : " ",
                     laser ? "X" : " ",
                     relay ? "X" : " ",
                     (unsigned int)rx,
                     (unsigned int)match);
            prev_comm = comm;
            prev_auth = auth;
            prev_arrived = arrived;
            prev_laser = laser;
            prev_relay = relay;
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

static esp_err_t init_sensor_device(void)
{
#if CONFIG_SENSOR_TYPE == SENSOR_TYPE_VL53L0X
    return vl53l0x_init(SENSOR_I2C_PORT);
#else
    return vl6180x_init(SENSOR_I2C_PORT);
#endif
}

static esp_err_t read_sensor_range_mm(uint16_t *out_mm)
{
#if CONFIG_SENSOR_TYPE == SENSOR_TYPE_VL53L0X
    return vl53l0x_read_range_mm(SENSOR_I2C_PORT, out_mm);
#else
    return vl6180x_read_range_mm(SENSOR_I2C_PORT, out_mm);
#endif
}

static esp_err_t init_sensor_bus(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SENSOR_I2C_SDA_GPIO,
        .scl_io_num = SENSOR_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_I2C_CLK_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(SENSOR_I2C_PORT, &i2c_conf), TAG, "i2c_param_config");
    esp_err_t err = i2c_driver_install(SENSOR_I2C_PORT, i2c_conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return init_sensor_device();
}

static void sensor_task(void *arg)
{
    (void)arg;
    while (true) {
        if (g_hw_failure) {
            // Attempt to re-initialize if it was previously failed
            esp_err_t init_err = init_sensor_device();
            if (init_err == ESP_OK) {
                g_hw_failure = false;
                ESP_LOGI(TAG, "Laser sensor recovered and initialized!");
            } else {
                ESP_LOGW(TAG, "Laser sensor re-init attempt failed: %s", esp_err_to_name(init_err));
            }
        }

        uint16_t distance_mm = 0;
        esp_err_t measurement_err = g_hw_failure
                                        ? ESP_ERR_INVALID_STATE
                                        : read_sensor_range_mm(&distance_mm);
        if (measurement_err == ESP_OK) {
            bool in_range = (distance_mm >= 30 && distance_mm <= SENSOR_PRESENCE_THRESHOLD_MM);
            s_laser_detected = in_range;
            g_hw_failure = false;
            if (distance_mm == 0xFFFF) {
                ESP_LOGI(TAG, "Laser distance: Out of range (detected: 0)");
            } else {
                ESP_LOGI(TAG, "Laser distance: %u mm (detected: %d)", distance_mm, s_laser_detected);
            }
        } else {
            s_laser_detected = false;
            if (measurement_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Laser read error: %s", esp_err_to_name(measurement_err));
            }
            g_hw_failure = (measurement_err != ESP_ERR_TIMEOUT && measurement_err != ESP_OK);
        }

        bool manual_trigger = g_manual_unlock_until_ms && (int32_t)(g_manual_unlock_until_ms - now_ms()) > 0;

        if (s_authorized && s_arrived && (s_laser_detected || manual_trigger)) {
            s_relay_active_until_ms = now_ms() + RELAY_TRIGGER_MS;

            update_stealth_token(); // Roll immediately
            g_manual_unlock_until_ms = 0;
            s_previous_token_valid_until_ms = 0;
            s_previous_challenge_valid_until_ms = 0;

            s_authorized = false;
            s_auth_window_started_ms = 0;
            s_arrived = false;
            s_last_claim_ms = 0; // require a fresh claim/token exchange after burn
            proximity_rssi_init(&s_claim_rssi_smoother);
            char ts[16];
            format_uptime(ts, sizeof(ts), now_ms());
            ESP_LOGI(TAG, "[%s] challenge burned; waiting for new claim", ts);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    bool relay_on = false;
    bool auth_led_on = false;
    uint32_t red_blink_counter = 0;
    bool red_led_state = false;
    uint32_t green_blink_counter = 0;
    bool green_led_state = false;
    
    gpio_reset_pin(ALIVE_LED_GPIO); // GPIO 12 Auth LED
    gpio_set_direction(ALIVE_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(DETECTION_LED_GPIO); // GPIO 13 Open LED
    gpio_set_direction(DETECTION_LED_GPIO, GPIO_MODE_OUTPUT);
    
    gpio_reset_pin(GREEN_LED_GPIO);
    gpio_set_level(GREEN_LED_GPIO, 1); // Active low, 1 is OFF
    gpio_set_direction(GREEN_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RED_LED_GPIO);
    gpio_set_level(RED_LED_GPIO, 1); // Active low, 1 is OFF
    gpio_set_direction(RED_LED_GPIO, GPIO_MODE_OUTPUT);

    while (true) {
        // SW failure takes highest priority — blink BOTH LEDs together
        if (g_sw_failure) {
            red_blink_counter++;
            if (red_blink_counter >= 12) { // Toggle every 240ms (12 * 20ms) — fast alarm
                red_blink_counter = 0;
                red_led_state = !red_led_state;
                gpio_set_level(RED_LED_GPIO, red_led_state ? 0 : 1);
                gpio_set_level(GREEN_LED_GPIO, red_led_state ? 0 : 1);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

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
        
        // Red LED HW Failure Blinking
        if (g_hw_failure) {
            red_blink_counter++;
            if (red_blink_counter >= 25) { // Toggle every 500ms (25 * 20ms)
                red_blink_counter = 0;
                red_led_state = !red_led_state;
                gpio_set_level(RED_LED_GPIO, red_led_state ? 0 : 1); // Active low
            }
        } else {
            red_blink_counter = 0;
            red_led_state = false;
            gpio_set_level(RED_LED_GPIO, 1); // Active low, 1 is OFF
        }
        
        // Green LED Stage Blinking
        const uint32_t now = now_ms();
        const uint32_t last_claim_age_ms = s_last_claim_ms == 0 ? 0 : (now - s_last_claim_ms);
        const bool comm = s_last_claim_ms != 0 && last_claim_age_ms <= CLAIM_IDLE_TIMEOUT_MS;
        
        uint8_t stage_count = 0;
        if (comm) stage_count++;
        if (s_authorized) stage_count++;
        if (s_arrived) stage_count++;
        if (s_laser_detected) stage_count++;
        
        uint32_t green_blink_threshold = 0;
        if (stage_count == 1) green_blink_threshold = 50;      // 1000ms (slow)
        else if (stage_count == 2) green_blink_threshold = 25; // 500ms (medium)
        else if (stage_count >= 3) green_blink_threshold = 5;  // 100ms (fast)
        
        if (relay_on) {
            gpio_set_level(GREEN_LED_GPIO, 0); // Solid ON (Active low)
            green_blink_counter = 0;
            green_led_state = true;
        } else if (stage_count > 0) {
            green_blink_counter++;
            if (green_blink_counter >= green_blink_threshold) {
                green_blink_counter = 0;
                green_led_state = !green_led_state;
                gpio_set_level(GREEN_LED_GPIO, green_led_state ? 0 : 1);
            }
        } else {
            gpio_set_level(GREEN_LED_GPIO, 1); // Solid OFF
            green_blink_counter = 0;
            green_led_state = false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS flash erase failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        g_sw_failure = true;
    }

    if (!g_sw_failure) {
        ret = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
    }

    if (!g_sw_failure) {
        ret = device_key_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Device key init failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
    }

    if (!s_device_pubkey_logged) {
        s_device_pubkey_logged = true;
        const uint8_t *device_pubkey = device_key_get_public();
        if (device_pubkey != NULL) {
            char pubkey_hex[DEVICE_KEY_PUB_LEN * 2 + 1];
            ESP_LOGI(TAG, "device pubkey=%s",
                     bytes_to_hex(device_pubkey, DEVICE_KEY_PUB_LEN, pubkey_hex, sizeof(pubkey_hex)));
        }
    }

    if (!g_sw_failure) {
        ret = enrollment_mgr_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Enrollment manager init failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
    }

    gpio_reset_pin(RELAY_GPIO);
    set_relay(false); // Set logical level HIGH (secure) BEFORE enabling output
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    esp_err_t sensor_err = init_sensor_bus();
    if (sensor_err != ESP_OK) {
        ESP_LOGW(TAG, "Laser sensor not detected: %s. Continuing without hardware sensor.", esp_err_to_name(sensor_err));
        g_hw_failure = true;
    }

    if (!g_sw_failure) {
        update_stealth_token(); // Initialize the current challenge beacon
        ret = ble_scanner_set_static_uuid_beacon(STATIC_UUID);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE beacon UUID set failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
    }

    if (!g_sw_failure) {
        ble_scanner_set_claim_detected_cb(claim_detected);
        ret = ble_scanner_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE scanner init failed: %s", esp_err_to_name(ret));
            g_sw_failure = true;
        }
    }

    if (!g_sw_failure) {
        s_claim_queue = xQueueCreate(1, sizeof(lighthouse_ble_claim_t));
        if (s_claim_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create claim queue (heap exhausted)");
            g_sw_failure = true;
        }
    }

    if (g_sw_failure) {
        ESP_LOGE(TAG, "CRITICAL: Software failure detected. System running in degraded mode. Check LEDs.");
    }

    // Always start these tasks — relay_task drives LED failure indicators even in degraded mode
    if (!g_sw_failure) {
        xTaskCreate(auth_task, "auth_task", 8192, NULL, 5, NULL);
        xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
        xTaskCreate(token_rotate_task, "token_rotate_task", 3072, NULL, 5, NULL);
        xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
    }
    xTaskCreate(relay_task, "relay_task", 2048, NULL, 5, NULL);
}

#include "enrollment_mgr.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "enrollment_mgr";
static const char *NVS_NS = "enroll_store";
static const char *KEY_ADMIN_PASS = "admin_pass";
static const char *KEY_DEVICES = "devices";

static nvs_handle_t s_handle;
static enrolled_device_t s_device_cache[ENROLLMENT_MAX_DEVICES];
static bool s_cache_valid = false;

static void update_cache(void)
{
    memset(s_device_cache, 0, sizeof(s_device_cache));
    s_cache_valid = false;
    size_t size = sizeof(s_device_cache);
    if (nvs_get_blob(s_handle, KEY_DEVICES, s_device_cache, &size) == ESP_OK) {
        s_cache_valid = true;
    }
}

esp_err_t enrollment_mgr_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    // Check if admin password exists, if not set default to "admin123"
    size_t required_size;
    err = nvs_get_str(s_handle, KEY_ADMIN_PASS, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Setting default admin password");
        nvs_set_str(s_handle, KEY_ADMIN_PASS, "admin");
        nvs_commit(s_handle);
    }

    // Initialize devices blob if it doesn't exist
    err = nvs_get_blob(s_handle, KEY_DEVICES, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        enrolled_device_t empty_list[ENROLLMENT_MAX_DEVICES] = {0};
        nvs_set_blob(s_handle, KEY_DEVICES, empty_list, sizeof(empty_list));
        nvs_commit(s_handle);
    }
    
    update_cache();
    size_t active_count = 0;
    for (int i = 0; i < ENROLLMENT_MAX_DEVICES; i++) {
        if (s_device_cache[i].active) {
            active_count++;
        }
    }
    ESP_LOGI(TAG, "enrollment cache ready active_devices=%u", (unsigned int)active_count);

    return ESP_OK;
}

bool enrollment_mgr_check_admin_pass(const char *password)
{
    if (!password) return false;
    char stored_pass[64];
    size_t size = sizeof(stored_pass);
    if (nvs_get_str(s_handle, KEY_ADMIN_PASS, stored_pass, &size) == ESP_OK) {
        return strcmp(password, stored_pass) == 0;
    }
    return false;
}

esp_err_t enrollment_mgr_set_admin_pass(const char *password)
{
    if (!password || strlen(password) >= 64) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_str(s_handle, KEY_ADMIN_PASS, password);
    if (err == ESP_OK) {
        nvs_commit(s_handle);
    }
    return err;
}

esp_err_t enrollment_mgr_add_device(const uint8_t *user_id, const uint8_t *pubkey)
{
    enrolled_device_t devices[ENROLLMENT_MAX_DEVICES];
    size_t size = sizeof(devices);
    esp_err_t err = nvs_get_blob(s_handle, KEY_DEVICES, devices, &size);
    if (err != ESP_OK) return err;

    int empty_slot = -1;
    for (int i = 0; i < ENROLLMENT_MAX_DEVICES; i++) {
        if (devices[i].active && memcmp(devices[i].user_id, user_id, LIGHTHOUSE_USER_ID_LEN) == 0) {
            // Device exists, update pubkey
            memcpy(devices[i].pubkey, pubkey, ENROLLMENT_PUBKEY_LEN);
            err = nvs_set_blob(s_handle, KEY_DEVICES, devices, sizeof(devices));
            if (err == ESP_OK) {
                nvs_commit(s_handle);
                update_cache();
                ESP_LOGI(TAG, "Updated existing device slot for user %02X%02X%02X%02X",
                         user_id[0], user_id[1], user_id[2], user_id[3]);
            }
            return err;
        }
        if (!devices[i].active && empty_slot == -1) {
            empty_slot = i;
        }
    }

    if (empty_slot == -1) return ESP_ERR_NO_MEM; // Enrollment list full

    memcpy(devices[empty_slot].user_id, user_id, LIGHTHOUSE_USER_ID_LEN);
    memcpy(devices[empty_slot].pubkey, pubkey, ENROLLMENT_PUBKEY_LEN);
    devices[empty_slot].active = true;

    err = nvs_set_blob(s_handle, KEY_DEVICES, devices, sizeof(devices));
    if (err == ESP_OK) {
        nvs_commit(s_handle);
        update_cache();
        ESP_LOGI(TAG, "Enrolled new device for user %02X%02X%02X%02X",
                 user_id[0], user_id[1], user_id[2], user_id[3]);
    }
    return err;
}

esp_err_t enrollment_mgr_revoke_device(const uint8_t *user_id)
{
    enrolled_device_t devices[ENROLLMENT_MAX_DEVICES];
    size_t size = sizeof(devices);
    esp_err_t err = nvs_get_blob(s_handle, KEY_DEVICES, devices, &size);
    if (err != ESP_OK) return err;

    bool found = false;
    for (int i = 0; i < ENROLLMENT_MAX_DEVICES; i++) {
        if (devices[i].active && memcmp(devices[i].user_id, user_id, LIGHTHOUSE_USER_ID_LEN) == 0) {
            devices[i].active = false;
            found = true;
            break;
        }
    }

    if (found) {
        err = nvs_set_blob(s_handle, KEY_DEVICES, devices, sizeof(devices));
        if (err == ESP_OK) {
            nvs_commit(s_handle);
            update_cache();
        }
    }
    return err;
}

size_t enrollment_mgr_get_devices(enrolled_device_t *out_devices, size_t max_devices)
{
    if (out_devices == NULL || max_devices == 0) {
        return 0;
    }

    enrolled_device_t devices[ENROLLMENT_MAX_DEVICES] = {0};
    size_t size = sizeof(devices);
    if (nvs_get_blob(s_handle, KEY_DEVICES, devices, &size) != ESP_OK) {
        return 0;
    }

    size_t count = 0;
    for (int i = 0; i < ENROLLMENT_MAX_DEVICES && count < max_devices; i++) {
        if (devices[i].active) {
            out_devices[count++] = devices[i];
        }
    }
    return count;
}

bool enrollment_mgr_is_device_active(const uint8_t *user_id, uint8_t *out_pubkey)
{
    if (user_id == NULL) {
        return false;
    }

    enrolled_device_t devices[ENROLLMENT_MAX_DEVICES] = {0};
    size_t size = sizeof(devices);
    if (nvs_get_blob(s_handle, KEY_DEVICES, devices, &size) != ESP_OK) {
        return false;
    }

    for (int i = 0; i < ENROLLMENT_MAX_DEVICES; i++) {
        if (devices[i].active && memcmp(devices[i].user_id, user_id, LIGHTHOUSE_USER_ID_LEN) == 0) {
            if (out_pubkey) {
                memcpy(out_pubkey, devices[i].pubkey, ENROLLMENT_PUBKEY_LEN);
            }
            return true;
        }
    }
    return false;
}

size_t enrollment_mgr_get_user_ids(uint8_t *out_buf, size_t max_len)
{
    if (out_buf == NULL || max_len == 0) {
        return 0;
    }
    enrolled_device_t devices[ENROLLMENT_MAX_DEVICES] = {0};
    size_t size = sizeof(devices);
    if (nvs_get_blob(s_handle, KEY_DEVICES, devices, &size) != ESP_OK) {
        return 0;
    }
    size_t count = 0;
    for (int i = 0; i < ENROLLMENT_MAX_DEVICES; i++) {
        if (devices[i].active && (count * LIGHTHOUSE_USER_ID_LEN + LIGHTHOUSE_USER_ID_LEN) <= max_len) {
            memcpy(&out_buf[count * LIGHTHOUSE_USER_ID_LEN], devices[i].user_id, LIGHTHOUSE_USER_ID_LEN);
            count++;
        }
    }
    return count * LIGHTHOUSE_USER_ID_LEN;
}

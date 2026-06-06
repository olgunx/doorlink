#include "device_key.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_key";
static const char *NVS_NS = "device_key";
static const char *KEY_PRIV = "priv";
static const char *KEY_PUB = "pub";

static bool s_loaded = false;
static uint8_t s_private_key[DEVICE_KEY_PRIV_LEN];
static uint8_t s_public_key[DEVICE_KEY_PUB_LEN];

static const char *bytes_to_hex(const uint8_t *bytes, size_t len, char *buf, size_t buf_size)
{
    size_t offset = 0;
    for (size_t i = 0; i < len && offset + 2 < buf_size; ++i) {
        offset += snprintf(buf + offset, buf_size - offset, "%02x", bytes[i]);
    }
    buf[offset] = '\0';
    return buf;
}

static void clear_state(void)
{
    memset(s_private_key, 0, sizeof(s_private_key));
    memset(s_public_key, 0, sizeof(s_public_key));
    s_loaded = false;
}

static bool validate_keypair(const uint8_t *priv_buf, const uint8_t *pub_buf)
{
    if (priv_buf == NULL || pub_buf == NULL) {
        return false;
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    bool ok = false;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);

    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        goto cleanup;
    }
    if (mbedtls_mpi_read_binary(&d, priv_buf, DEVICE_KEY_PRIV_LEN) != 0) {
        goto cleanup;
    }
    if (mbedtls_ecp_point_read_binary(&grp, &q, pub_buf, DEVICE_KEY_PUB_LEN) != 0) {
        goto cleanup;
    }
    if (mbedtls_ecp_check_pubkey(&grp, &q) != 0) {
        goto cleanup;
    }
    if (mbedtls_ecp_check_privkey(&grp, &d) != 0) {
        goto cleanup;
    }
    ok = true;

cleanup:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&q);
    return ok;
}

static esp_err_t generate_keypair(void)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "doorlink_device_key";
    uint8_t priv_buf[DEVICE_KEY_PRIV_LEN];
    uint8_t pub_buf[DEVICE_KEY_PUB_LEN];
    esp_err_t err = ESP_FAIL;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed ret=%d", ret);
        goto cleanup;
    }

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "ecp_group_load failed ret=%d", ret);
        goto cleanup;
    }

    ret = mbedtls_ecp_gen_keypair(&grp, &d, &q, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "ecp_gen_keypair failed ret=%d", ret);
        goto cleanup;
    }

    ret = mbedtls_mpi_write_binary(&d, priv_buf, sizeof(priv_buf));
    if (ret != 0) {
        ESP_LOGE(TAG, "mpi_write_binary(priv) failed ret=%d", ret);
        goto cleanup;
    }

    size_t pub_len = 0;
    ret = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &pub_len, pub_buf, sizeof(pub_buf));
    if (ret != 0 || pub_len != sizeof(pub_buf)) {
        ESP_LOGE(TAG, "ecp_point_write_binary failed ret=%d pub_len=%u", ret, (unsigned int)pub_len);
        goto cleanup;
    }

    nvs_handle_t handle;
    ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ret = nvs_set_blob(handle, KEY_PRIV, priv_buf, sizeof(priv_buf));
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, KEY_PUB, pub_buf, sizeof(pub_buf));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "persist keypair failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    memcpy(s_private_key, priv_buf, sizeof(s_private_key));
    memcpy(s_public_key, pub_buf, sizeof(s_public_key));
    s_loaded = true;
    char pub_hex[DEVICE_KEY_PUB_LEN * 2 + 1];
    ESP_LOGI(TAG, "generated ESP keypair pub=%s", bytes_to_hex(s_public_key, sizeof(s_public_key), pub_hex, sizeof(pub_hex)));
    err = ESP_OK;

cleanup:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&q);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return err;
}

esp_err_t device_key_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t priv_len = sizeof(s_private_key);
    size_t pub_len = sizeof(s_public_key);
    ret = nvs_get_blob(handle, KEY_PRIV, s_private_key, &priv_len);
    if (ret == ESP_OK && priv_len == sizeof(s_private_key)) {
        ret = nvs_get_blob(handle, KEY_PUB, s_public_key, &pub_len);
        if (ret == ESP_OK && pub_len == sizeof(s_public_key)) {
            if (!validate_keypair(s_private_key, s_public_key)) {
                ESP_LOGW(TAG, "stored device keypair invalid; regenerating");
                clear_state();
            } else {
                s_loaded = true;
                nvs_close(handle);
                return ESP_OK;
            }
        }
    }
    nvs_close(handle);

    clear_state();
    esp_err_t gen = generate_keypair();
    if (gen == ESP_OK) {
        char pub_hex[DEVICE_KEY_PUB_LEN * 2 + 1];
        ESP_LOGI(TAG, "device key ready pub=%s", bytes_to_hex(s_public_key, sizeof(s_public_key), pub_hex, sizeof(pub_hex)));
    }
    return gen;
}

const uint8_t *device_key_get_private(void)
{
    return s_loaded ? s_private_key : NULL;
}

const uint8_t *device_key_get_public(void)
{
    return s_loaded ? s_public_key : NULL;
}

size_t device_key_get_public_hex(char *out, size_t out_size)
{
    if (!s_loaded || out == NULL || out_size == 0) {
        return 0;
    }
    return strlen(bytes_to_hex(s_public_key, sizeof(s_public_key), out, out_size));
}

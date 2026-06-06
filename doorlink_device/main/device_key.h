#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_KEY_PRIV_LEN 32
#define DEVICE_KEY_PUB_LEN 65

esp_err_t device_key_init(void);
const uint8_t *device_key_get_private(void);
const uint8_t *device_key_get_public(void);
size_t device_key_get_public_hex(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

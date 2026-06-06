#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_console_init(void);
size_t web_console_get_esp_public_key_hex(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

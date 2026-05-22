#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int samples[RSSI_SAMPLE_WINDOW];
    uint8_t sample_count;
    uint8_t next_index;
    int32_t sample_sum;
} rssi_smoother_t;

void proximity_rssi_init(rssi_smoother_t *smoother);
int proximity_rssi_update(rssi_smoother_t *smoother, int rssi);
proximity_class_t proximity_classify(int smoothed_rssi);
const char *proximity_class_to_str(proximity_class_t proximity);

#ifdef __cplusplus
}
#endif

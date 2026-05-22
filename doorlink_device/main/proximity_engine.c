#include "proximity_engine.h"

#include <string.h>

void proximity_rssi_init(rssi_smoother_t *smoother)
{
    memset(smoother, 0, sizeof(*smoother));
}

int proximity_rssi_update(rssi_smoother_t *smoother, int rssi)
{
    if (smoother->sample_count < RSSI_SAMPLE_WINDOW) {
        smoother->samples[smoother->next_index] = rssi;
        smoother->sample_sum += rssi;
        smoother->sample_count++;
    } else {
        smoother->sample_sum -= smoother->samples[smoother->next_index];
        smoother->samples[smoother->next_index] = rssi;
        smoother->sample_sum += rssi;
    }

    smoother->next_index = (smoother->next_index + 1) % RSSI_SAMPLE_WINDOW;
    return smoother->sample_sum / smoother->sample_count;
}

proximity_class_t proximity_classify(int smoothed_rssi)
{
    if (smoothed_rssi > -50) {
        return PROXIMITY_VERY_NEAR;
    }

    if (smoothed_rssi >= -75) {
        return PROXIMITY_NEAR;
    }

    return PROXIMITY_FAR;
}

const char *proximity_class_to_str(proximity_class_t proximity)
{
    switch (proximity) {
    case PROXIMITY_VERY_NEAR:
        return "VERY_NEAR";
    case PROXIMITY_NEAR:
        return "NEAR";
    case PROXIMITY_FAR:
        return "FAR";
    default:
        return "UNKNOWN";
    }
}

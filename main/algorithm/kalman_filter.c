#include "algorithm/kalman_filter.h"

void kalman_filter_init(kalman_filter_1d_t *filter, float process_noise, float measurement_noise, float initial_value) {
    if (!filter) {
        return;
    }
    filter->q = process_noise;
    filter->r = measurement_noise;
    filter->x = initial_value;
    filter->p = 1.0f;
    filter->k = 0.0f;
    filter->initialized = 1;
}

float kalman_filter_update(kalman_filter_1d_t *filter, float measurement) {
    if (!filter) {
        return measurement;
    }
    if (!filter->initialized) {
        kalman_filter_init(filter, 0.01f, 0.1f, measurement);
    }

    filter->p += filter->q;
    filter->k = filter->p / (filter->p + filter->r);
    filter->x += filter->k * (measurement - filter->x);
    filter->p *= (1.0f - filter->k);
    return filter->x;
}

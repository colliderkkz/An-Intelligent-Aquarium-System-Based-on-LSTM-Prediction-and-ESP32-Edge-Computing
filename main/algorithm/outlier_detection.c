#include "algorithm/outlier_detection.h"

static float absf_local(float value) {
    return value < 0.0f ? -value : value;
}

bool outlier_detection_is_spike(float current_value, float previous_value, float threshold) {
    return absf_local(current_value - previous_value) > threshold;
}

size_t outlier_detection_filter_spikes(const float *input, float *output, size_t count, float threshold) {
    if (!input || !output || count == 0U) {
        return 0U;
    }

    output[0] = input[0];
    for (size_t i = 1; i < count; i++) {
        output[i] = outlier_detection_is_spike(input[i], output[i - 1], threshold) ? output[i - 1] : input[i];
    }
    return count;
}

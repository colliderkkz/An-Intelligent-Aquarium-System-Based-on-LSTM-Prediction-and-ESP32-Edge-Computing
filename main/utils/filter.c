#include "utils/filter.h"

float filter_clamp(float value, float min_value, float max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

float filter_moving_average(const float *values, size_t count) {
    if (!values || count == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }
    return sum / (float)count;
}

float filter_median(float *values, size_t count) {
    if (!values || count == 0) {
        return 0.0f;
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (values[j] < values[i]) {
                float tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }

    if ((count % 2U) == 0U) {
        size_t right = count / 2U;
        return (values[right - 1U] + values[right]) * 0.5f;
    }
    return values[count / 2U];
}

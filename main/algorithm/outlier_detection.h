#ifndef OUTLIER_DETECTION_H
#define OUTLIER_DETECTION_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool outlier_detection_is_spike(float current_value, float previous_value, float threshold);
size_t outlier_detection_filter_spikes(const float *input, float *output, size_t count, float threshold);

#ifdef __cplusplus
}
#endif

#endif // OUTLIER_DETECTION_H

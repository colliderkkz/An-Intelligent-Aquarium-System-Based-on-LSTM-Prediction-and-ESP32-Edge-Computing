#ifndef FILTER_H
#define FILTER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

float filter_clamp(float value, float min_value, float max_value);
float filter_moving_average(const float *values, size_t count);
float filter_median(float *values, size_t count);

#ifdef __cplusplus
}
#endif

#endif // FILTER_H

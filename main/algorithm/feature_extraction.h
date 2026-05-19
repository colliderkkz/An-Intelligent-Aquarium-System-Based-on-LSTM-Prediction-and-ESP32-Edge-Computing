#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float ph;
    float tds_ppm;
    float turbidity_ntu;
    float light_percent;
    float co2_ppm;
    float water_level_percent;
    uint32_t runtime_minutes;
    uint8_t hour;
    uint8_t weekday;
} sensor_feature_input_t;

typedef struct {
    float values[10];
} sensor_feature_vector_t;

void feature_extraction_build_vector(const sensor_feature_input_t *input, sensor_feature_vector_t *vector);

#ifdef __cplusplus
}
#endif

#endif // FEATURE_EXTRACTION_H

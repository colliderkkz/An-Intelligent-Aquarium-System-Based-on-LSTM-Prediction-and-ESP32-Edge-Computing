#ifndef DATA_FUSION_H
#define DATA_FUSION_H

#include "algorithm/kalman_filter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float ph;
    float tds_ppm;
    float turbidity_ntu;
    float light_percent;
    float water_level_percent;
} fused_sensor_data_t;

typedef struct {
    kalman_filter_1d_t temperature_filter;
    kalman_filter_1d_t ph_filter;
    kalman_filter_1d_t tds_filter;
    kalman_filter_1d_t turbidity_filter;
    int initialized;
} data_fusion_ctx_t;

void data_fusion_init(data_fusion_ctx_t *ctx);
void data_fusion_update(data_fusion_ctx_t *ctx, const fused_sensor_data_t *raw, fused_sensor_data_t *fused);

#ifdef __cplusplus
}
#endif

#endif // DATA_FUSION_H

#include "algorithm/data_fusion.h"

void data_fusion_init(data_fusion_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    kalman_filter_init(&ctx->temperature_filter, 0.01f, 0.3f, 25.0f);
    kalman_filter_init(&ctx->ph_filter, 0.01f, 0.2f, 7.0f);
    kalman_filter_init(&ctx->tds_filter, 1.0f, 10.0f, 200.0f);
    kalman_filter_init(&ctx->turbidity_filter, 1.0f, 10.0f, 5.0f);
    ctx->initialized = 1;
}

void data_fusion_update(data_fusion_ctx_t *ctx, const fused_sensor_data_t *raw, fused_sensor_data_t *fused) {
    if (!ctx || !raw || !fused) {
        return;
    }
    if (!ctx->initialized) {
        data_fusion_init(ctx);
    }

    fused->temperature_c = kalman_filter_update(&ctx->temperature_filter, raw->temperature_c);
    fused->ph = kalman_filter_update(&ctx->ph_filter, raw->ph);
    fused->tds_ppm = kalman_filter_update(&ctx->tds_filter, raw->tds_ppm);
    fused->turbidity_ntu = kalman_filter_update(&ctx->turbidity_filter, raw->turbidity_ntu);
    fused->light_percent = raw->light_percent;
    fused->water_level_percent = raw->water_level_percent;
}

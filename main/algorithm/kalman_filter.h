#ifndef KALMAN_FILTER_H
#define KALMAN_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float q;
    float r;
    float x;
    float p;
    float k;
    int initialized;
} kalman_filter_1d_t;

void kalman_filter_init(kalman_filter_1d_t *filter, float process_noise, float measurement_noise, float initial_value);
float kalman_filter_update(kalman_filter_1d_t *filter, float measurement);

#ifdef __cplusplus
}
#endif

#endif // KALMAN_FILTER_H

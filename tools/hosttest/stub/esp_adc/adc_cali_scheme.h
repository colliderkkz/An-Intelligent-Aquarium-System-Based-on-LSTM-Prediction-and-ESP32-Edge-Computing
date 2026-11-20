#ifndef STUB_ADC_CALI_SCHEME_H
#define STUB_ADC_CALI_SCHEME_H

#include "esp_adc/adc_cali.h"
#include "hal/adc_types.h"

/* 桩：假设使用 curve fitting 校准 */
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#define ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED  0

typedef struct {
    adc_unit_t unit_id;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

typedef struct {
    adc_unit_t unit_id;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_line_fitting_config_t;

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config,
                                               adc_cali_handle_t *ret_handle);
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t *config,
                                              adc_cali_handle_t *ret_handle);

#endif

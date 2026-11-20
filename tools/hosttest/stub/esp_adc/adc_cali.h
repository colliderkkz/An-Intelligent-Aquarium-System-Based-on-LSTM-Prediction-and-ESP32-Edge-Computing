#ifndef STUB_ADC_CALI_H
#define STUB_ADC_CALI_H

#include "esp_err.h"

typedef struct adc_cali_scheme_t *adc_cali_handle_t;

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *voltage);
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t handle);

#endif

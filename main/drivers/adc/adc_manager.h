#ifndef ADC_MANAGER_H
#define ADC_MANAGER_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t adc_manager_init(void);
esp_err_t adc_manager_config_channel(adc_channel_t channel);
esp_err_t adc_manager_read_raw(adc_channel_t channel, int *out_val);
esp_err_t adc_manager_read_voltage(adc_channel_t channel, int *out_mv);

#ifdef __cplusplus
}
#endif

#endif // ADC_MANAGER_H

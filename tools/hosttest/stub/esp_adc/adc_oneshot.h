#ifndef STUB_ADC_ONESHOT_H
#define STUB_ADC_ONESHOT_H

#include "esp_err.h"
#include "hal/adc_types.h"

typedef struct adc_oneshot_unit_ctx_t *adc_oneshot_unit_handle_t;

typedef struct {
    adc_unit_t unit_id;
    adc_oneshot_clk_src_t clk_src;
    adc_ulp_mode_t ulp_mode;
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *init_config,
                               adc_oneshot_unit_handle_t *ret_unit);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,
                                     adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *config);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int *out_raw);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle);

#endif

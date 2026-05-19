#ifndef MHZ19B_H
#define MHZ19B_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t co2_ppm;
} mhz19b_data_t;

esp_err_t mhz19b_init(void);
esp_err_t mhz19b_read(mhz19b_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // MHZ19B_H

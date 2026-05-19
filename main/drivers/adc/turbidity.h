#ifndef TURBIDITY_H
#define TURBIDITY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t turbidity_init(void);
esp_err_t turbidity_read(float *ntu);
esp_err_t turbidity_read_compensated(float temperature_c, float *ntu);

#ifdef __cplusplus
}
#endif

#endif // TURBIDITY_H

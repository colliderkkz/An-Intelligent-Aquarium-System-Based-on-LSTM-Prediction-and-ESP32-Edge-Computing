#include "algorithm/feature_extraction.h"

void feature_extraction_build_vector(const sensor_feature_input_t *input, sensor_feature_vector_t *vector) {
    if (!input || !vector) {
        return;
    }

    vector->values[0] = input->temperature_c;
    vector->values[1] = input->ph;
    vector->values[2] = input->tds_ppm;
    vector->values[3] = input->turbidity_ntu;
    vector->values[4] = input->light_percent;
    vector->values[5] = input->co2_ppm;
    vector->values[6] = input->water_level_percent;
    vector->values[7] = (float)input->runtime_minutes;
    vector->values[8] = (float)input->hour;
    vector->values[9] = (float)input->weekday;
}

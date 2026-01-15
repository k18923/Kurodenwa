#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t ring_control_init(void);
esp_err_t ring_control_start(void);
esp_err_t ring_control_stop(void);
bool ring_control_is_ringing(void);

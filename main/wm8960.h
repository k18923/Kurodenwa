#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t wm8960_init(void);
esp_err_t wm8960_write_reg(uint8_t reg, uint16_t val);

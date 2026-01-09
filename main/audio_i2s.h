#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_i2s_init(void);
void audio_i2s_push_hfp_audio(const uint8_t *data, size_t len);

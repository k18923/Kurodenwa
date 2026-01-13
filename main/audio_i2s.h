#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_i2s_init(void);
void audio_i2s_push_hfp_audio(const uint8_t *data, size_t len);
void audio_i2s_toggle_tone(void);
void audio_i2s_set_hfp_enabled(bool enabled);
void audio_i2s_clear_buffer(void);
void audio_i2s_reset_hfp_state(void);

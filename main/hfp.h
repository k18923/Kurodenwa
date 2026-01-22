#pragma once

#include "esp_err.h"
#include "esp_hf_client_api.h"
#include <stdbool.h>

esp_err_t hfp_init(void);
void hfp_set_auto_answer(bool enabled);
bool hfp_get_auto_answer(void);
esp_err_t hfp_hangup_call(void);
esp_err_t hfp_answer_call(void);
esp_err_t hfp_dial_number(const char *number);

typedef void (*hfp_call_status_callback_t)(esp_hf_call_status_t status);
typedef void (*hfp_call_setup_callback_t)(esp_hf_call_setup_status_t status);

void hfp_set_call_status_callback(hfp_call_status_callback_t callback);
void hfp_set_call_setup_callback(hfp_call_setup_callback_t callback);
esp_hf_client_audio_state_t hfp_get_audio_state(void);

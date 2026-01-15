#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t dial_hook_init(void);

typedef enum {
    DIAL_HOOK_ON = 0,
    DIAL_HOOK_OFF = 1,
} dial_hook_state_t;

typedef void (*dial_hook_state_callback_t)(dial_hook_state_t state);
typedef void (*dial_hook_digit_callback_t)(int digit);
typedef void (*dial_hook_dial_start_callback_t)(void);

dial_hook_state_t dial_hook_get_state(void);
void dial_hook_set_state_callback(dial_hook_state_callback_t callback);
void dial_hook_set_digit_callback(dial_hook_digit_callback_t callback);
void dial_hook_set_dial_start_callback(dial_hook_dial_start_callback_t callback);
int dial_hook_get_digit(void);
bool dial_hook_is_dialing(void);

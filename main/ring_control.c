#include "ring_control.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define RING_FR_GPIO GPIO_NUM_5
#define RING_RM_GPIO GPIO_NUM_19
#define RING_TOGGLE_PERIOD_US 20000
#define RING_ON_DURATION_US 1000000
#define RING_OFF_DURATION_US 2000000

static const char *TAG = "ring_ctl";
static esp_timer_handle_t s_ring_timer;
static esp_timer_handle_t s_cadence_timer;
static bool s_ringing = false;
static bool s_fr_level = false;
static bool s_ring_on_phase = false;
static bool s_toggle_active = false;

static void ring_timer_cb(void *arg) {
    (void)arg;
    s_fr_level = !s_fr_level;
    gpio_set_level(RING_FR_GPIO, s_fr_level);
}

static void ring_set_output(bool enabled) {
    if (enabled) {
        gpio_set_level(RING_RM_GPIO, 1);
        s_fr_level = false;
        gpio_set_level(RING_FR_GPIO, s_fr_level);
        if (!s_toggle_active) {
            if (esp_timer_start_periodic(s_ring_timer, RING_TOGGLE_PERIOD_US) == ESP_OK) {
                s_toggle_active = true;
            }
        }
        return;
    }

    if (s_toggle_active) {
        esp_timer_stop(s_ring_timer);
        s_toggle_active = false;
    }
    gpio_set_level(RING_FR_GPIO, 0);
    gpio_set_level(RING_RM_GPIO, 0);
    s_fr_level = false;
}

static void ring_cadence_cb(void *arg) {
    (void)arg;
    if (!s_ringing) {
        return;
    }
    s_ring_on_phase = !s_ring_on_phase;
    ring_set_output(s_ring_on_phase);
    int64_t next_us = s_ring_on_phase ? RING_ON_DURATION_US : RING_OFF_DURATION_US;
    esp_timer_start_once(s_cadence_timer, next_us);
}

esp_err_t ring_control_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RING_FR_GPIO) | (1ULL << RING_RM_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO init failed");

    gpio_set_level(RING_FR_GPIO, 0);
    gpio_set_level(RING_RM_GPIO, 0);

    esp_timer_create_args_t timer_args = {
        .callback = &ring_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ring_toggle",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ring_timer), TAG, "Timer create failed");

    esp_timer_create_args_t cadence_args = {
        .callback = &ring_cadence_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ring_cadence",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&cadence_args, &s_cadence_timer), TAG, "Cadence timer create failed");

    s_ringing = false;
    s_fr_level = false;
    s_ring_on_phase = false;
    s_toggle_active = false;
    ESP_LOGI(TAG, "Ring control ready (FR=GPIO%d, RM=GPIO%d)", RING_FR_GPIO, RING_RM_GPIO);
    return ESP_OK;
}

esp_err_t ring_control_start(void) {
    if (s_ringing) {
        return ESP_OK;
    }

    s_ringing = true;
    s_ring_on_phase = true;
    ring_set_output(true);
    esp_timer_start_once(s_cadence_timer, RING_ON_DURATION_US);
    ESP_LOGI(TAG, "Ringing started");
    return ESP_OK;
}

esp_err_t ring_control_stop(void) {
    if (!s_ringing) {
        return ESP_OK;
    }

    s_ringing = false;
    s_ring_on_phase = false;
    esp_timer_stop(s_cadence_timer);
    ring_set_output(false);
    ESP_LOGI(TAG, "Ringing stopped");
    return ESP_OK;
}

bool ring_control_is_ringing(void) {
    return s_ringing;
}

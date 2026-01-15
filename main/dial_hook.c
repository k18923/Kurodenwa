#include "dial_hook.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define HOOK_GPIO GPIO_NUM_21
#define PULSE_THRESHOLD_MS 200
#define MIN_PULSE_WIDTH_MS 10
#define BRIDGE_TIME_MS 10
#define INTER_DIGIT_TIMEOUT_MS 300

#define DIGIT_QUEUE_SIZE 16
#define EDGE_QUEUE_LEN 16

typedef struct {
    bool is_rising;
    bool is_falling;
    uint32_t time_ms;
} hook_edge_event_t;

static const char *TAG = "dial_hook";

static QueueHandle_t s_edge_queue;
static esp_timer_handle_t s_on_hook_timer;
static esp_timer_handle_t s_bridge_timer;
static esp_timer_handle_t s_inter_digit_timer;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static dial_hook_state_t s_state = DIAL_HOOK_ON;
static bool s_in_pulse = false;
static uint32_t s_pulse_start_ms = 0;
static int s_pulse_count = 0;
static bool s_is_dialing = false;

static int s_digit_queue[DIGIT_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;

static dial_hook_state_callback_t s_state_cb;
static dial_hook_digit_callback_t s_digit_cb;
static dial_hook_dial_start_callback_t s_dial_start_cb;

static void enqueue_digit(int digit) {
    int next_head = (s_queue_head + 1) % DIGIT_QUEUE_SIZE;
    if (next_head == s_queue_tail) {
        return;
    }
    s_digit_queue[s_queue_head] = digit;
    s_queue_head = next_head;
}

static void set_state(dial_hook_state_t state) {
    dial_hook_state_callback_t cb = NULL;
    bool changed = false;

    portENTER_CRITICAL(&s_lock);
    if (s_state != state) {
        s_state = state;
        cb = s_state_cb;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (changed && cb) {
        cb(state);
    }
}

static void finalize_digit(void) {
    int digit = -1;
    dial_hook_digit_callback_t cb = NULL;

    portENTER_CRITICAL(&s_lock);
    if (s_pulse_count > 0) {
        digit = s_pulse_count % 10;
        enqueue_digit(digit);
        s_pulse_count = 0;
        s_is_dialing = false;
        cb = s_digit_cb;
    }
    portEXIT_CRITICAL(&s_lock);

    if (digit >= 0 && cb) {
        cb(digit);
    }
}

static void on_hook_timer_cb(void *arg) {
    (void)arg;
    if (gpio_get_level(HOOK_GPIO)) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_in_pulse = false;
    s_pulse_count = 0;
    s_is_dialing = false;
    portEXIT_CRITICAL(&s_lock);

    if (esp_timer_is_active(s_inter_digit_timer)) {
        esp_timer_stop(s_inter_digit_timer);
    }

    set_state(DIAL_HOOK_ON);
}

static void bridge_timer_cb(void *arg) {
    (void)arg;
    if (!gpio_get_level(HOOK_GPIO)) {
        return;
    }

    bool pulse_detected = false;
    uint32_t pulse_start_ms = 0;
    dial_hook_state_t next_state = DIAL_HOOK_OFF;

    portENTER_CRITICAL(&s_lock);
    if (s_in_pulse) {
        pulse_detected = true;
        pulse_start_ms = s_pulse_start_ms;
    }
    portEXIT_CRITICAL(&s_lock);

    if (pulse_detected) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t total_ms = now_ms - pulse_start_ms;
        uint32_t pulse_width_ms = (total_ms > BRIDGE_TIME_MS) ? (total_ms - BRIDGE_TIME_MS) : 0;
        if (pulse_width_ms >= MIN_PULSE_WIDTH_MS) {
            portENTER_CRITICAL(&s_lock);
            s_pulse_count++;
            s_is_dialing = true;
            portEXIT_CRITICAL(&s_lock);
            esp_timer_stop(s_inter_digit_timer);
            esp_timer_start_once(s_inter_digit_timer, INTER_DIGIT_TIMEOUT_MS * 1000ULL);
        }
    }

    set_state(next_state);
}

static void inter_digit_timer_cb(void *arg) {
    (void)arg;
    finalize_digit();
}

static void handle_falling_edge(uint32_t now_ms) {
    bool dial_start = false;
    if (esp_timer_is_active(s_bridge_timer)) {
        esp_timer_stop(s_bridge_timer);
    } else {
        portENTER_CRITICAL(&s_lock);
        if (!s_in_pulse) {
            s_in_pulse = true;
            s_pulse_start_ms = now_ms;
            dial_start = true;
        }
        portEXIT_CRITICAL(&s_lock);

        if (!esp_timer_is_active(s_on_hook_timer)) {
            esp_timer_start_once(s_on_hook_timer, PULSE_THRESHOLD_MS * 1000ULL);
        }
    }

    if (dial_start && s_dial_start_cb) {
        s_dial_start_cb();
    }
}

static void handle_rising_edge(void) {
    if (!esp_timer_is_active(s_bridge_timer)) {
        esp_timer_start_once(s_bridge_timer, BRIDGE_TIME_MS * 1000ULL);
    }
    if (esp_timer_is_active(s_on_hook_timer)) {
        esp_timer_stop(s_on_hook_timer);
    }
}

static void hook_gpio_isr(void *arg) {
    (void)arg;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int level = gpio_get_level(HOOK_GPIO);
    hook_edge_event_t event = {
        .is_rising = level == 1,
        .is_falling = level == 0,
        .time_ms = now_ms,
    };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_edge_queue, &event, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void dial_hook_task(void *arg) {
    (void)arg;
    hook_edge_event_t event;
    while (true) {
        if (xQueueReceive(s_edge_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event.is_falling) {
                handle_falling_edge(event.time_ms);
            }
            if (event.is_rising) {
                handle_rising_edge();
            }
        }
    }
}

esp_err_t dial_hook_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << HOOK_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO config failed");

    s_edge_queue = xQueueCreate(EDGE_QUEUE_LEN, sizeof(hook_edge_event_t));
    if (!s_edge_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t on_hook_args = {
        .callback = &on_hook_timer_cb,
        .name = "hook_on",
    };
    esp_timer_create_args_t bridge_args = {
        .callback = &bridge_timer_cb,
        .name = "hook_bridge",
    };
    esp_timer_create_args_t digit_args = {
        .callback = &inter_digit_timer_cb,
        .name = "dial_digit",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&on_hook_args, &s_on_hook_timer), TAG, "Timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_create(&bridge_args, &s_bridge_timer), TAG, "Timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_create(&digit_args, &s_inter_digit_timer), TAG, "Timer create failed");

    bool pin_high = gpio_get_level(HOOK_GPIO);
    s_state = pin_high ? DIAL_HOOK_OFF : DIAL_HOOK_ON;

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(HOOK_GPIO, hook_gpio_isr, NULL), TAG, "ISR handler add failed");

    xTaskCreate(dial_hook_task, "dial_hook", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Dial/Hook ready (GPIO%d)", HOOK_GPIO);
    return ESP_OK;
}

dial_hook_state_t dial_hook_get_state(void) {
    dial_hook_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

void dial_hook_set_state_callback(dial_hook_state_callback_t callback) {
    portENTER_CRITICAL(&s_lock);
    s_state_cb = callback;
    portEXIT_CRITICAL(&s_lock);
}

void dial_hook_set_digit_callback(dial_hook_digit_callback_t callback) {
    portENTER_CRITICAL(&s_lock);
    s_digit_cb = callback;
    portEXIT_CRITICAL(&s_lock);
}

void dial_hook_set_dial_start_callback(dial_hook_dial_start_callback_t callback) {
    portENTER_CRITICAL(&s_lock);
    s_dial_start_cb = callback;
    portEXIT_CRITICAL(&s_lock);
}

int dial_hook_get_digit(void) {
    int digit = -1;
    portENTER_CRITICAL(&s_lock);
    if (s_queue_head != s_queue_tail) {
        digit = s_digit_queue[s_queue_tail];
        s_queue_tail = (s_queue_tail + 1) % DIGIT_QUEUE_SIZE;
    }
    portEXIT_CRITICAL(&s_lock);
    return digit;
}

bool dial_hook_is_dialing(void) {
    bool dialing;
    portENTER_CRITICAL(&s_lock);
    dialing = s_is_dialing;
    portEXIT_CRITICAL(&s_lock);
    return dialing;
}

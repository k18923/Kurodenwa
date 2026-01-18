#include "audio_i2s.h"
#include "dial_hook.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart_vfs.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "hfp.h"
#include "ring_control.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "kurodenwa";
static const gpio_num_t LED_GPIO = GPIO_NUM_13;

#define DIAL_BUFFER_SIZE 32
#define NUMBER_COMPLETE_TIMEOUT_MS 3000
#define HANGUP_RETRY_DELAY_MS 800
#define HANGUP_RETRY_MAX 3

typedef enum {
    CALL_STATE_IDLE = 0,
    CALL_STATE_RINGING,
    CALL_STATE_OFFHOOK_IDLE,
    CALL_STATE_DIALING,
    CALL_STATE_OUTBOUND_RING,
    CALL_STATE_TALKING,
} call_state_t;

static call_state_t s_call_state = CALL_STATE_IDLE;
static dial_hook_state_t s_hook_state = DIAL_HOOK_ON;
static esp_hf_call_status_t s_hfp_call_status = ESP_HF_CALL_STATUS_NO_CALLS;
static esp_hf_call_setup_status_t s_hfp_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
static esp_timer_handle_t s_number_timer;
static char s_dial_buffer[DIAL_BUFFER_SIZE];
static size_t s_dial_len = 0;
static bool s_ring_suppressed = false;
static bool s_ring_enabled = true;
static esp_timer_handle_t s_hangup_timer;
static bool s_hangup_pending = false;
static int s_hangup_retries = 0;

static const char *call_state_to_str(call_state_t state) {
    switch (state) {
    case CALL_STATE_IDLE:
        return "IDLE";
    case CALL_STATE_RINGING:
        return "RINGING";
    case CALL_STATE_OFFHOOK_IDLE:
        return "OFFHOOK_IDLE";
    case CALL_STATE_DIALING:
        return "DIALING";
    case CALL_STATE_OUTBOUND_RING:
        return "OUTBOUND_RING";
    case CALL_STATE_TALKING:
        return "TALKING";
    default:
        return "UNKNOWN";
    }
}

static const char *hook_state_to_str(dial_hook_state_t state) {
    return state == DIAL_HOOK_OFF ? "OFF_HOOK" : "ON_HOOK";
}

static const char *hfp_call_status_to_str(esp_hf_call_status_t status) {
    switch (status) {
    case ESP_HF_CALL_STATUS_NO_CALLS:
        return "NO_CALLS";
    case ESP_HF_CALL_STATUS_CALL_IN_PROGRESS:
        return "IN_PROGRESS";
    default:
        return "UNKNOWN";
    }
}

static const char *hfp_call_setup_to_str(esp_hf_call_setup_status_t status) {
    switch (status) {
    case ESP_HF_CALL_SETUP_STATUS_IDLE:
        return "IDLE";
    case ESP_HF_CALL_SETUP_STATUS_INCOMING:
        return "INCOMING";
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING:
        return "OUTGOING_DIALING";
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING:
        return "OUTGOING_ALERTING";
    default:
        return "UNKNOWN";
    }
}

static void dial_buffer_reset(void) {
    s_dial_len = 0;
    s_dial_buffer[0] = '\0';
}

static void number_timer_stop(void) {
    if (s_number_timer && esp_timer_is_active(s_number_timer)) {
        esp_timer_stop(s_number_timer);
    }
}

static void number_timer_restart(void) {
    if (!s_number_timer) {
        return;
    }
    number_timer_stop();
    esp_timer_start_once(s_number_timer, NUMBER_COMPLETE_TIMEOUT_MS * 1000ULL);
}

static void call_state_set(call_state_t next_state, const char *reason) {
    if (s_call_state == next_state) {
        return;
    }

    if (s_call_state == CALL_STATE_RINGING && next_state != CALL_STATE_RINGING) {
        esp_err_t err = ring_control_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Ring stop failed: %s", esp_err_to_name(err));
        }
        s_ring_suppressed = false;
    }

    ESP_LOGI(TAG, "Call state: %s -> %s (%s)",
             call_state_to_str(s_call_state),
             call_state_to_str(next_state),
             reason ? reason : "-");

    s_call_state = next_state;

    if (s_call_state == CALL_STATE_RINGING) {
        if (!s_ring_enabled) {
            ESP_LOGI(TAG, "Ring disabled");
            return;
        }
        if (!s_ring_suppressed) {
            esp_err_t err = ring_control_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Ring start failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Ring suppressed (auto-answer)");
        }
    }
}

static void call_state_after_call_end(const char *reason) {
    number_timer_stop();
    dial_buffer_reset();
    s_ring_suppressed = false;
    if (s_hook_state == DIAL_HOOK_OFF) {
        call_state_set(CALL_STATE_OFFHOOK_IDLE, reason);
    } else {
        call_state_set(CALL_STATE_IDLE, reason);
    }
}

static void handle_hook_off(void) {
    s_hook_state = DIAL_HOOK_OFF;
    ESP_LOGI(TAG, "Hook event: OFF_HOOK");

    if (s_call_state == CALL_STATE_TALKING) {
        return;
    }

    if (s_call_state == CALL_STATE_RINGING) {
        ESP_LOGI(TAG, "Answer incoming call (hook off)");
        hfp_answer_call();
        call_state_set(CALL_STATE_TALKING, "hook answer");
        return;
    }

    if (s_hfp_call_setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) {
        ESP_LOGI(TAG, "Answer incoming call (hook off)");
        hfp_answer_call();
        call_state_set(CALL_STATE_TALKING, "hook answer");
        return;
    }

    if (s_call_state == CALL_STATE_IDLE) {
        dial_buffer_reset();
        call_state_set(CALL_STATE_OFFHOOK_IDLE, "hook off");
        return;
    }

    if (s_call_state == CALL_STATE_RINGING) {
        call_state_set(CALL_STATE_OFFHOOK_IDLE, "ringing->offhook");
        return;
    }
}

static void handle_hook_on(void) {
    s_hook_state = DIAL_HOOK_ON;
    ESP_LOGI(TAG, "Hook event: ON_HOOK");

    number_timer_stop();
    dial_buffer_reset();

    if (s_hfp_call_status != ESP_HF_CALL_STATUS_NO_CALLS ||
        s_hfp_call_setup != ESP_HF_CALL_SETUP_STATUS_IDLE ||
        s_call_state == CALL_STATE_RINGING ||
        s_call_state == CALL_STATE_OUTBOUND_RING ||
        s_call_state == CALL_STATE_TALKING) {
        ESP_LOGI(TAG, "Hangup call (on-hook)");
        s_hangup_pending = true;
        s_hangup_retries = 0;
        hfp_hangup_call();
        if (s_hangup_timer) {
            esp_timer_start_once(s_hangup_timer, HANGUP_RETRY_DELAY_MS * 1000ULL);
        }
    }

    call_state_set(CALL_STATE_IDLE, "hook on");
}

static void handle_dial_start(void) {
    ESP_LOGI(TAG, "Dial event: START");
    if (s_call_state == CALL_STATE_OFFHOOK_IDLE) {
        call_state_set(CALL_STATE_DIALING, "dial start");
    }
}

static void handle_dial_digit(int digit) {
    ESP_LOGI(TAG, "Dial event: DIGIT %d", digit);
    if (s_call_state != CALL_STATE_OFFHOOK_IDLE && s_call_state != CALL_STATE_DIALING) {
        ESP_LOGI(TAG, "Dial ignored (state=%s)", call_state_to_str(s_call_state));
        return;
    }
    if (digit < 0 || digit > 9) {
        return;
    }
    if (s_dial_len >= DIAL_BUFFER_SIZE - 1) {
        ESP_LOGW(TAG, "Dial buffer full, ignoring digit");
        return;
    }

    s_dial_buffer[s_dial_len++] = (char)('0' + digit);
    s_dial_buffer[s_dial_len] = '\0';
    number_timer_restart();

    ESP_LOGI(TAG, "Dial buffer: %s", s_dial_buffer);
    call_state_set(CALL_STATE_OFFHOOK_IDLE, "digit end");
}

static void handle_number_complete(void) {
    if (s_dial_len == 0) {
        return;
    }
    if (s_call_state != CALL_STATE_OFFHOOK_IDLE && s_call_state != CALL_STATE_DIALING) {
        return;
    }

    ESP_LOGI(TAG, "Dial number: %s", s_dial_buffer);
    hfp_dial_number(s_dial_buffer);
    dial_buffer_reset();
    call_state_set(CALL_STATE_OUTBOUND_RING, "number complete");
}

static void number_timer_cb(void *arg) {
    (void)arg;
    handle_number_complete();
}

static void handle_call_setup(esp_hf_call_setup_status_t status) {
    s_hfp_call_setup = status;
    switch (status) {
    case ESP_HF_CALL_SETUP_STATUS_INCOMING:
        ESP_LOGI(TAG, "Call event: INCOMING");
        s_ring_suppressed = false;
        if (s_hook_state == DIAL_HOOK_OFF) {
            if (s_call_state == CALL_STATE_DIALING ||
                s_call_state == CALL_STATE_OUTBOUND_RING ||
                s_call_state == CALL_STATE_TALKING) {
                ESP_LOGI(TAG, "Reject incoming call (busy off-hook)");
                hfp_hangup_call();
                return;
            }
            ESP_LOGI(TAG, "Answer incoming call (off-hook)");
            hfp_answer_call();
            call_state_set(CALL_STATE_TALKING, "hook answer");
            return;
        }
        if (hfp_get_auto_answer()) {
            ESP_LOGI(TAG, "Auto-answer incoming call");
            hfp_answer_call();
            call_state_set(CALL_STATE_TALKING, "auto-answer");
            return;
        }
        call_state_set(CALL_STATE_RINGING, "incoming");
        break;
    case ESP_HF_CALL_SETUP_STATUS_IDLE:
        ESP_LOGI(TAG, "Call event: RING_STOP");
        if (s_call_state == CALL_STATE_RINGING) {
            call_state_after_call_end("ring stop");
        }
        if (s_hfp_call_status == ESP_HF_CALL_STATUS_NO_CALLS) {
            s_hangup_pending = false;
            if (s_hangup_timer && esp_timer_is_active(s_hangup_timer)) {
                esp_timer_stop(s_hangup_timer);
            }
        }
        break;
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING:
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING:
        ESP_LOGI(TAG, "Call event: OUTGOING");
        if (s_call_state != CALL_STATE_TALKING) {
            call_state_set(CALL_STATE_OUTBOUND_RING, "outgoing");
        }
        break;
    default:
        break;
    }
}

static void handle_call_status(esp_hf_call_status_t status) {
    s_hfp_call_status = status;
    switch (status) {
    case ESP_HF_CALL_STATUS_CALL_IN_PROGRESS:
        ESP_LOGI(TAG, "Call event: TALKING");
        call_state_set(CALL_STATE_TALKING, "call active");
        break;
    case ESP_HF_CALL_STATUS_NO_CALLS:
        ESP_LOGI(TAG, "Call event: PEER_HANGUP");
        call_state_after_call_end("call ended");
        s_hangup_pending = false;
        if (s_hangup_timer && esp_timer_is_active(s_hangup_timer)) {
            esp_timer_stop(s_hangup_timer);
        }
        break;
    default:
        break;
    }
}

static void hangup_timer_cb(void *arg) {
    (void)arg;
    if (!s_hangup_pending) {
        return;
    }
    if (s_hfp_call_status == ESP_HF_CALL_STATUS_NO_CALLS &&
        s_hfp_call_setup == ESP_HF_CALL_SETUP_STATUS_IDLE) {
        s_hangup_pending = false;
        return;
    }
    if (s_hangup_retries >= HANGUP_RETRY_MAX) {
        ESP_LOGW(TAG, "Hangup retries exceeded (call=%s setup=%s)",
                 hfp_call_status_to_str(s_hfp_call_status),
                 hfp_call_setup_to_str(s_hfp_call_setup));
        s_hangup_pending = false;
        return;
    }
    s_hangup_retries++;
    ESP_LOGW(TAG, "Retry hangup (%d/%d)", s_hangup_retries, HANGUP_RETRY_MAX);
    hfp_hangup_call();
    if (s_hangup_timer) {
        esp_timer_start_once(s_hangup_timer, HANGUP_RETRY_DELAY_MS * 1000ULL);
    }
}

static void dial_hook_state_cb(dial_hook_state_t state) {
    if (state == DIAL_HOOK_OFF) {
        handle_hook_off();
    } else {
        handle_hook_on();
    }
}

static void dial_hook_digit_cb(int digit) {
    handle_dial_digit(digit);
}

static void dial_hook_dial_start_cb(void) {
    handle_dial_start();
}

static int cmd_ring(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (ring_control_is_ringing()) {
        ESP_ERROR_CHECK(ring_control_stop());
    } else {
        ESP_ERROR_CHECK(ring_control_start());
    }
    return 0;
}

static int cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\n=== コマンド一覧 ===\n");
    printf("  t        音声テスト (440Hz)\n");
    printf("  r        ベル手動トグル\n");
    printf("  a        自動応答 ON/OFF\n");
    printf("  b        鳴動 ON/OFF\n");
    printf("  m        マイクモニタ ON/OFF（HFP未接続時）\n");
    printf("  s        状態表示\n");
    printf("  ?        ヘルプ表示\n");
    printf("====================\n\n");
    return 0;
}

static int cmd_auto_answer(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool enabled = !hfp_get_auto_answer();
    hfp_set_auto_answer(enabled);
    return 0;
}

static int cmd_ring_enable(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_ring_enabled = !s_ring_enabled;
    ESP_LOGI(TAG, "Ring %s", s_ring_enabled ? "ENABLED" : "DISABLED");
    if (!s_ring_enabled && ring_control_is_ringing()) {
        ring_control_stop();
    }
    return 0;
}

static int cmd_status(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\n=== 状態 ===\n");
    printf("  State        : %s\n", call_state_to_str(s_call_state));
    printf("  Hook         : %s\n", hook_state_to_str(s_hook_state));
    printf("  Auto-answer  : %s\n", hfp_get_auto_answer() ? "ON" : "OFF");
    printf("  Ring enabled : %s\n", s_ring_enabled ? "ON" : "OFF");
    printf("  Mic monitor  : %s\n", audio_i2s_get_mic_monitor() ? "ON" : "OFF");
    printf("  HFP call     : %s\n", hfp_call_status_to_str(s_hfp_call_status));
    printf("  HFP setup    : %s\n", hfp_call_setup_to_str(s_hfp_call_setup));
    printf("  Dial buffer  : %s\n", s_dial_buffer[0] ? s_dial_buffer : "-");
    printf("  Hangup retry : %s\n", s_hangup_pending ? "YES" : "NO");
    printf("===============\n\n");
    return 0;
}

static int cmd_tone(int argc, char **argv) {
    (void)argc;
    (void)argv;
    audio_i2s_toggle_tone();
    return 0;
}

static int cmd_mic_monitor(int argc, char **argv) {
    (void)argc;
    (void)argv;
    audio_i2s_toggle_mic_monitor();
    return 0;
}

static void register_console_commands(void) {
    const esp_console_cmd_t tone_cmd = {
        .command = "t",
        .help = "Toggle 440Hz test tone",
        .hint = NULL,
        .func = &cmd_tone,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&tone_cmd));

    const esp_console_cmd_t ring_cmd = {
        .command = "r",
        .help = "Toggle ring",
        .hint = NULL,
        .func = &cmd_ring,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ring_cmd));

    const esp_console_cmd_t auto_answer_cmd = {
        .command = "a",
        .help = "Toggle auto-answer",
        .hint = NULL,
        .func = &cmd_auto_answer,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&auto_answer_cmd));

    const esp_console_cmd_t ring_enable_cmd = {
        .command = "b",
        .help = "Toggle ring enable",
        .hint = NULL,
        .func = &cmd_ring_enable,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ring_enable_cmd));

    const esp_console_cmd_t mic_monitor_cmd = {
        .command = "m",
        .help = "Toggle mic monitor (idle only)",
        .hint = NULL,
        .func = &cmd_mic_monitor,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mic_monitor_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "s",
        .help = "Show status",
        .hint = NULL,
        .func = &cmd_status,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t help_cmd = {
        .command = "?",
        .help = "Show command list",
        .hint = NULL,
        .func = &cmd_help,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));
}

static void console_task(void *arg) {
    (void)arg;
    while (true) {
        char *line = linenoise("> ");
        if (line == NULL) {
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            int ret = 0;
            esp_err_t err = esp_console_run(line, &ret);
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Unknown command: %s", line);
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "Console error: %s", esp_err_to_name(err));
            }
        }
        linenoiseFree(line);
    }
}

static void console_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    uart_vfs_dev_use_driver(UART_NUM_0);

    const esp_console_config_t console_config = {
        .max_cmdline_args = 8,
        .max_cmdline_length = 128,
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));
    esp_console_register_help_command();
    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(10);
    linenoiseSetDumbMode(1);

    register_console_commands();
    xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Console ready. Type 't' to toggle tone.");
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Booting Kurodenwa (ESP32-DevKitC WROOM-32)");

    ESP_ERROR_CHECK(hfp_init());
    hfp_set_call_status_callback(handle_call_status);
    hfp_set_call_setup_callback(handle_call_setup);
    ESP_ERROR_CHECK(audio_i2s_init());
    ESP_ERROR_CHECK(ring_control_init());

    esp_timer_create_args_t number_timer_args = {
        .callback = &number_timer_cb,
        .name = "dial_number",
    };
    ESP_ERROR_CHECK(esp_timer_create(&number_timer_args, &s_number_timer));

    esp_timer_create_args_t hangup_timer_args = {
        .callback = &hangup_timer_cb,
        .name = "hangup_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hangup_timer_args, &s_hangup_timer));

    ESP_ERROR_CHECK(dial_hook_init());
    dial_hook_set_state_callback(dial_hook_state_cb);
    dial_hook_set_digit_callback(dial_hook_digit_cb);
    dial_hook_set_dial_start_callback(dial_hook_dial_start_cb);
    s_hook_state = dial_hook_get_state();
    if (s_hook_state == DIAL_HOOK_OFF) {
        call_state_set(CALL_STATE_OFFHOOK_IDLE, "initial hook");
    }

    console_init();

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_LOGI(TAG, "Init complete. Blinking LED on GPIO%d.", LED_GPIO);

    bool level = false;
    while (true) {
        gpio_set_level(LED_GPIO, level);
        level = !level;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

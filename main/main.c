#include "audio_i2s.h"
#include "dial_hook.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "ring_control.h"
#include "hfp.h"

static const char *TAG = "kurodenwa";
static const gpio_num_t LED_GPIO = GPIO_NUM_13;

static int cmd_tone(int argc, char **argv) {
    (void)argc;
    (void)argv;
    audio_i2s_toggle_tone();
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
    esp_vfs_dev_uart_use_driver(UART_NUM_0);

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
    ESP_ERROR_CHECK(audio_i2s_init());
    ESP_ERROR_CHECK(ring_control_init());
    ESP_ERROR_CHECK(dial_hook_init());

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

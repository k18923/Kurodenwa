#include "audio_i2s.h"
#include "dial_hook.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "ring_control.h"
#include "hfp.h"

static const char *TAG = "kurodenwa";
static const gpio_num_t LED_GPIO = GPIO_NUM_22;

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

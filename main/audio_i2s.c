#include "audio_i2s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "wm8960.h"

#define I2S_BCLK_GPIO GPIO_NUM_18
#define I2S_LRCLK_GPIO GPIO_NUM_19
#define I2S_DOUT_GPIO GPIO_NUM_21
#define I2S_DIN_GPIO GPIO_NUM_20

#define I2S_SAMPLE_RATE 16000
#define I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define I2S_CHANNEL_MODE I2S_SLOT_MODE_STEREO

#define AUDIO_STREAM_BUF_SIZE 8192
#define AUDIO_TASK_STACK_SIZE 4096

static const char *TAG = "audio_i2s";

static i2s_chan_handle_t s_tx_chan;
static StreamBufferHandle_t s_audio_stream;
static bool s_audio_ready = false;
static uint32_t s_drop_bytes = 0;

static void audio_i2s_task(void *arg) {
    (void)arg;
    uint8_t in_buf[320];
    int16_t out_buf[320];

    while (true) {
        size_t read_len = xStreamBufferReceive(s_audio_stream, in_buf, sizeof(in_buf), portMAX_DELAY);
        if (read_len < 2) {
            continue;
        }
        read_len &= ~1u;
        size_t samples = read_len / 2;
        for (size_t i = 0; i < samples; i++) {
            int16_t sample = ((int16_t *)in_buf)[i];
            out_buf[i * 2] = sample;
            out_buf[i * 2 + 1] = sample;
        }

        size_t bytes_to_write = samples * 2 * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t audio_i2s_init(void) {
    esp_err_t err = wm8960_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WM8960 init failed: %s", esp_err_to_name(err));
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS_PER_SAMPLE, I2S_CHANNEL_MODE),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRCLK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_audio_stream = xStreamBufferCreate(AUDIO_STREAM_BUF_SIZE, 1);
    if (!s_audio_stream) {
        ESP_LOGE(TAG, "Audio stream buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(audio_i2s_task, "audio_i2s_task", AUDIO_TASK_STACK_SIZE, NULL, 5, NULL);
    s_audio_ready = true;

    ESP_LOGI(TAG, "I2S ready (%d Hz, 16-bit stereo).", I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "HFP audio is assumed to be PCM; mSBC decode is not implemented yet.");
    return ESP_OK;
}

void audio_i2s_push_hfp_audio(const uint8_t *data, size_t len) {
    if (!s_audio_ready || !data || len == 0) {
        return;
    }
    size_t written = xStreamBufferSend(s_audio_stream, data, len, 0);
    if (written < len) {
        s_drop_bytes += (uint32_t)(len - written);
        if ((s_drop_bytes % 4096) < (len - written)) {
            ESP_LOGW(TAG, "Dropped audio bytes: %u", s_drop_bytes);
        }
    }
}

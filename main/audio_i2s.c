#include "audio_i2s.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define AUDIO_CODEC_PCM5102A 1
#define AUDIO_CODEC_WM8960 0
#define AUDIO_USE_MCLK 0

#if AUDIO_CODEC_WM8960
#include "wm8960.h"
#endif

#define I2S_BCLK_GPIO GPIO_NUM_18
#define I2S_LRCLK_GPIO GPIO_NUM_19
#define I2S_DOUT_GPIO GPIO_NUM_23
#define I2S_DIN_GPIO GPIO_NUM_20

#if AUDIO_USE_MCLK
#define I2S_MCLK_GPIO GPIO_NUM_25
#else
#define I2S_MCLK_GPIO I2S_GPIO_UNUSED
#endif

#define I2S_SAMPLE_RATE 48000
#define I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define I2S_CHANNEL_MODE I2S_SLOT_MODE_STEREO

#define AUDIO_STREAM_BUF_SIZE 8192
#define AUDIO_TASK_STACK_SIZE 4096
#define TONE_FREQ_HZ 440.0f
#define TONE_AMPLITUDE 12000.0f

static const char *TAG = "audio_i2s";

static i2s_chan_handle_t s_tx_chan;
static StreamBufferHandle_t s_audio_stream;
static bool s_audio_ready = false;
static uint32_t s_drop_bytes = 0;
static volatile bool s_tone_enabled = false;
static volatile bool s_tone_stopping = false;
static bool s_hfp_enabled = true;
static float s_tone_phase = 0.0f;
static float s_tone_gain = 0.0f;

#define TONE_FADE_SAMPLES 256
static uint32_t s_tone_debug_frames = 0;

static void audio_i2s_task(void *arg) {
  (void)arg;
  uint8_t in_buf[320];
  int16_t out_buf[320];
  const float phase_step =
      2.0f * (float)M_PI * TONE_FREQ_HZ / (float)I2S_SAMPLE_RATE;
  const float fade_step = 1.0f / (float)TONE_FADE_SAMPLES;

  while (true) {
    // トーンモードまたはフェードアウト中
    if (s_tone_enabled || s_tone_stopping) {
      const size_t samples = sizeof(out_buf) / (2 * sizeof(int16_t));

      // ゲインの目標値を決定
      float target_gain = s_tone_enabled ? 1.0f : 0.0f;

      for (size_t i = 0; i < samples; i++) {
        // 正弦波を生成（矩形波ではなく）
        float sine_val = sinf(s_tone_phase);
        int16_t sample = (int16_t)(sine_val * TONE_AMPLITUDE * s_tone_gain);

        out_buf[i * 2] = sample;
        out_buf[i * 2 + 1] = sample;

        s_tone_phase += phase_step;
        if (s_tone_phase >= 2.0f * (float)M_PI) {
          s_tone_phase -= 2.0f * (float)M_PI;
        }

        // ゲインをフェードイン/アウト
        if (s_tone_gain < target_gain) {
          s_tone_gain += fade_step;
          if (s_tone_gain > target_gain)
            s_tone_gain = target_gain;
        } else if (s_tone_gain > target_gain) {
          s_tone_gain -= fade_step;
          if (s_tone_gain < target_gain)
            s_tone_gain = target_gain;
        }
      }

      // デバッグ: 最初の数フレームでサンプル値をログ出力
      if (s_tone_debug_frames < 3 && s_tone_enabled) {
        ESP_LOGI(
            TAG,
            "Tone frame %u: samples[0..7] = %d, %d, %d, %d, %d, %d, %d, %d",
            (unsigned)s_tone_debug_frames, out_buf[0], out_buf[2], out_buf[4],
            out_buf[6], out_buf[8], out_buf[10], out_buf[12], out_buf[14]);
        s_tone_debug_frames++;
      }

      // フェードアウト完了チェック
      if (s_tone_stopping && s_tone_gain <= 0.0f) {
        s_tone_stopping = false;
        s_tone_phase = 0.0f;
        // ストリームバッファをクリアしてゴミデータを破棄
        if (s_audio_stream) {
          xStreamBufferReset(s_audio_stream);
        }
        ESP_LOGI(TAG, "Tone fade-out complete, buffer cleared");
        // ゼロ出力を書き込んで完全な無音を確保
        memset(out_buf, 0, sizeof(out_buf));
        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, out_buf, sizeof(out_buf), &bytes_written,
                          portMAX_DELAY);
        i2s_channel_write(s_tx_chan, out_buf, sizeof(out_buf), &bytes_written,
                          portMAX_DELAY);
      }

      size_t bytes_to_write = samples * 2 * sizeof(int16_t);
      size_t bytes_written = 0;
      esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write,
                                        &bytes_written, portMAX_DELAY);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
      }
      continue;
    }

    // HFPが無効の場合は無音を出力
    if (!s_hfp_enabled) {
      memset(out_buf, 0, sizeof(out_buf));
      size_t bytes_written = 0;
      i2s_channel_write(s_tx_chan, out_buf, sizeof(out_buf), &bytes_written,
                        portMAX_DELAY);
      vTaskDelay(pdMS_TO_TICKS(5)); // CPU負荷軽減
      continue;
    }

    size_t read_len = xStreamBufferReceive(s_audio_stream, in_buf,
                                           sizeof(in_buf), pdMS_TO_TICKS(20));
    if (read_len < 2) {
      memset(out_buf, 0, sizeof(out_buf));
      size_t bytes_written = 0;
      esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, sizeof(out_buf),
                                        &bytes_written, portMAX_DELAY);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
      }
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
    esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write,
                                      &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
    }
  }
}

esp_err_t audio_i2s_init(void) {
  esp_err_t err = ESP_OK;
#if AUDIO_CODEC_WM8960
  err = wm8960_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "WM8960 init failed: %s", esp_err_to_name(err));
  }
#endif

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
      .slot_cfg =
          {
              .data_bit_width = I2S_BITS_PER_SAMPLE,
              .slot_bit_width =
                  I2S_SLOT_BIT_WIDTH_16BIT, // 明示的に16ビットスロット
              .slot_mode = I2S_CHANNEL_MODE,
              .slot_mask = I2S_STD_SLOT_BOTH,
              .ws_width = 16,  // LRCLK幅 = 16ビット
              .ws_pol = false, // LRCLK極性: Lowで左チャンネル
              .bit_shift =
                  true, // Philips I2S: 1ビットシフト（MSBが1クロック遅れる）
          },
      .gpio_cfg =
          {
              .mclk = I2S_MCLK_GPIO,
              .bclk = I2S_BCLK_GPIO,
              .ws = I2S_LRCLK_GPIO,
              .dout = I2S_DOUT_GPIO,
              .din = I2S_DIN_GPIO,
              .invert_flags =
                  {
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

  xTaskCreate(audio_i2s_task, "audio_i2s_task", AUDIO_TASK_STACK_SIZE, NULL, 5,
              NULL);
  s_audio_ready = true;

  ESP_LOGI(TAG, "I2S ready (%d Hz, 16-bit stereo).", I2S_SAMPLE_RATE);
  ESP_LOGI(
      TAG,
      "HFP audio is assumed to be PCM; mSBC decode is not implemented yet.");
  return ESP_OK;
}

void audio_i2s_push_hfp_audio(const uint8_t *data, size_t len) {
  if (!s_audio_ready || !s_hfp_enabled || !data || len == 0) {
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

void audio_i2s_toggle_tone(void) {
  if (!s_tone_enabled) {
    // トーンON: 即座にフル音量で開始（デバッグ用にフェードイン無効）
    s_tone_stopping = false;
    s_tone_enabled = true;
    s_tone_phase = 0.0f;
    s_tone_gain = 1.0f; // フェードインを無効化: 即座にフル音量
    s_tone_debug_frames = 0;
    audio_i2s_clear_buffer();
    audio_i2s_set_hfp_enabled(false);
    ESP_LOGI(TAG, "Tone ON (gain=1.0)");
  } else {
    // トーンOFF: フェードアウト開始
    s_tone_enabled = false;
    s_tone_stopping = true; // フェードアウトモード
    // HFPは有効にしない - HFP音声接続時に自動で有効になる
    // audio_i2s_set_hfp_enabled(true);  // 削除
    ESP_LOGI(TAG, "Tone OFF (fade-out)");
  }
}

void audio_i2s_set_hfp_enabled(bool enabled) { s_hfp_enabled = enabled; }

void audio_i2s_clear_buffer(void) {
  if (!s_audio_ready || !s_audio_stream) {
    return;
  }
  xStreamBufferReset(s_audio_stream);
}

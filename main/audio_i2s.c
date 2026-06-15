#include "audio_i2s.h"

#include <math.h>
#include <string.h>

#include "hfp.h"
#include "driver/i2s_std.h"
#include "esp_hf_client_api.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define AUDIO_CODEC_PCM5102A 1
#define AUDIO_USE_MCLK 1 // 0にするとMCLK出力を無効化

#define I2S_BCLK_GPIO GPIO_NUM_32
#define I2S_LRCLK_GPIO GPIO_NUM_25
#define I2S_DOUT_GPIO GPIO_NUM_33
#define I2S_DIN_GPIO GPIO_NUM_34

#if AUDIO_USE_MCLK
#define I2S_MCLK_GPIO GPIO_NUM_0
#else
#define I2S_MCLK_GPIO I2S_GPIO_UNUSED
#endif

#define I2S_SAMPLE_RATE 48000
#define I2S_TX_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_32BIT
#define I2S_TX_SLOT_BITS I2S_SLOT_BIT_WIDTH_32BIT
#define I2S_TX_WS_WIDTH 32
#define I2S_RX_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_32BIT
#define I2S_RX_SLOT_BITS I2S_SLOT_BIT_WIDTH_32BIT
#define I2S_RX_WS_WIDTH 32
#define I2S_CHANNEL_MODE I2S_SLOT_MODE_STEREO

#define HFP_SAMPLE_RATE 8000
#if (I2S_SAMPLE_RATE % HFP_SAMPLE_RATE) == 0
#define HFP_UPSAMPLE_FACTOR (I2S_SAMPLE_RATE / HFP_SAMPLE_RATE)
#else
#define HFP_UPSAMPLE_FACTOR 1
#endif

#define HFP_IN_BUF_BYTES 320
#define HFP_IN_SAMPLES (HFP_IN_BUF_BYTES / 2)
#define HFP_OUT_BUF_SAMPLES (HFP_IN_SAMPLES * HFP_UPSAMPLE_FACTOR * 2) // interleaved stereo

#if (I2S_SAMPLE_RATE % HFP_SAMPLE_RATE) == 0
#define MIC_EXTRA_DOWNSAMPLE 1
#define MIC_DOWNSAMPLE_FACTOR ((I2S_SAMPLE_RATE / HFP_SAMPLE_RATE) * MIC_EXTRA_DOWNSAMPLE)
#else
#define MIC_DOWNSAMPLE_FACTOR 1
#endif

#define AUDIO_STREAM_BUF_SIZE 8192
#define AUDIO_TASK_STACK_SIZE 4096
#define MIC_STREAM_BUF_SIZE 65536
#define MIC_TASK_STACK_SIZE 4096
#define MIC_STREAM_CHUNK_BYTES 160
#define MIC_NOISE_GATE_THRESHOLD 5000  // ノイズゲート閾値を上げる（2000 -> 5000）
#define TONE_FREQ_HZ 440.0f
#define TONE_AMPLITUDE 12000.0f
#define HFP_GAIN 1.5f
#define HFP_DC_BLOCK_R 0.995f
#define MIC_DC_BLOCK_R 0.995f  // マイク用DCブロック係数
#define MIC_SHIFT_BITS 12
#define I2S_WRITE_TIMEOUT_MS 50
#define I2S_WRITE_FAIL_DELAY_MS 5
#define I2S_STARTUP_SKIP_MS 250

static const char *TAG = "audio_i2s";

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static StreamBufferHandle_t s_audio_stream;
static StreamBufferHandle_t s_mic_stream;
static bool s_audio_ready = false;
static TaskHandle_t s_audio_task = NULL;
static uint32_t s_drop_bytes = 0;
static uint32_t s_mic_drop_bytes = 0;
static volatile bool s_tone_enabled = false;
static volatile bool s_tone_stopping = false;
static bool s_hfp_enabled = true;
static volatile bool s_mic_enabled = false;
static volatile bool s_hfp_audio_active = false;
static float s_tone_phase = 0.0f;
static float s_tone_gain = 0.0f;

#define TONE_FADE_SAMPLES 256
static uint32_t s_tone_debug_frames = 0;

static uint8_t s_in_buf[HFP_IN_BUF_BYTES];
static int32_t s_out_buf[HFP_OUT_BUF_SAMPLES];
static int16_t s_prev_sample = 0;
static bool s_prev_sample_valid = false;
static float s_dc_x = 0.0f;
static float s_dc_y = 0.0f;
static float s_mic_dc_x = 0.0f;  // マイク用DCブロック状態
static float s_mic_dc_y = 0.0f;
static int64_t s_i2s_tx_ready_at_us = 0;
static bool s_tx_enabled = false;


void audio_i2s_reset_hfp_state(void) {
  s_prev_sample = 0;
  s_prev_sample_valid = false;
  s_dc_x = 0.0f;
  s_dc_y = 0.0f;
  s_mic_dc_x = 0.0f;  // マイクDCブロックもリセット
  s_mic_dc_y = 0.0f;
  s_i2s_tx_ready_at_us = 0;
  s_hfp_audio_active = false;
}

static int16_t process_hfp_sample(int16_t sample) {
  float y = (float)sample - s_dc_x + HFP_DC_BLOCK_R * s_dc_y;
  s_dc_x = (float)sample;
  s_dc_y = y;

  float z = y * HFP_GAIN;
  if (z > 32767.0f) {
    z = 32767.0f;
  } else if (z < -32768.0f) {
    z = -32768.0f;
  }
  return (int16_t)z;
}

static bool i2s_tx_ready(void) {
  if (s_i2s_tx_ready_at_us == 0) {
    s_i2s_tx_ready_at_us =
        esp_timer_get_time() + (int64_t)I2S_STARTUP_SKIP_MS * 1000;
  }
  if (esp_timer_get_time() < s_i2s_tx_ready_at_us) {
    vTaskDelay(pdMS_TO_TICKS(5));
    return false;
  }
  return true;
}
static void audio_i2s_task(void *arg) {
  (void)arg;
  uint8_t *in_buf = s_in_buf;
  int32_t *out_buf = s_out_buf;
  const float phase_step =
      2.0f * (float)M_PI * TONE_FREQ_HZ / (float)I2S_SAMPLE_RATE;
  const float fade_step = 1.0f / (float)TONE_FADE_SAMPLES;

  while (true) {
    if (!s_tx_enabled) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // トーンモードまたはフェードアウト中
    if (s_tone_enabled || s_tone_stopping) {
      const size_t samples = HFP_OUT_BUF_SAMPLES / 2;

      // ゲインの目標値を決定
      float target_gain = s_tone_enabled ? 1.0f : 0.0f;

      for (size_t i = 0; i < samples; i++) {
        // 正弦波を生成（矩形波ではなく）
        float sine_val = sinf(s_tone_phase);
        int16_t sample = (int16_t)(sine_val * TONE_AMPLITUDE * s_tone_gain);
        int32_t sample32 = ((int32_t)sample) << 16;

        out_buf[i * 2] = sample32;
        out_buf[i * 2 + 1] = sample32;

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
            "Tone frame %u: samples[0..7] = %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld",
            (unsigned)s_tone_debug_frames, (long)out_buf[0], (long)out_buf[2],
            (long)out_buf[4], (long)out_buf[6], (long)out_buf[8],
            (long)out_buf[10], (long)out_buf[12], (long)out_buf[14]);
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
        memset(out_buf, 0, sizeof(s_out_buf));
        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, out_buf, sizeof(s_out_buf), &bytes_written,
                          pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
        i2s_channel_write(s_tx_chan, out_buf, sizeof(s_out_buf), &bytes_written,
                          pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
      }

      size_t bytes_to_write = samples * 2 * sizeof(int32_t);
      size_t bytes_written = 0;
      esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write,
                                        &bytes_written,
                                        pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(I2S_WRITE_FAIL_DELAY_MS));
      }
      continue;
    }


    // HFPが無効の場合は送信を止める（切断時のブロック回避）
    if (!s_hfp_enabled) {
      vTaskDelay(pdMS_TO_TICKS(5)); // CPU負荷軽減
      continue;
    }

    size_t read_len = xStreamBufferReceive(s_audio_stream, in_buf,
                                           sizeof(s_in_buf), pdMS_TO_TICKS(20));
    if (!i2s_tx_ready()) {
      continue;
    }
    if (read_len < 2) {
      memset(out_buf, 0, sizeof(s_out_buf));
      size_t bytes_written = 0;
      esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, sizeof(s_out_buf),
                                        &bytes_written,
                                        pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(I2S_WRITE_FAIL_DELAY_MS));
      }
      continue;
    }
    read_len &= ~1u;
    size_t samples = read_len / 2;
    size_t out_idx = 0;
    for (size_t i = 0; i < samples; i++) {
      int16_t current = ((int16_t *)in_buf)[i];
      int16_t start = s_prev_sample_valid ? s_prev_sample : current;
      for (size_t r = 0; r < HFP_UPSAMPLE_FACTOR; r++) {
        float t = (HFP_UPSAMPLE_FACTOR > 1)
                      ? ((float)(r + 1) / (float)HFP_UPSAMPLE_FACTOR)
                      : 1.0f;
        float interp = (float)start + ((float)current - (float)start) * t;
        int16_t sample = process_hfp_sample((int16_t)interp);
        int32_t sample32 = ((int32_t)sample) << 16;
        out_buf[out_idx++] = sample32;
        out_buf[out_idx++] = sample32;
      }
      s_prev_sample = current;
      s_prev_sample_valid = true;
    }

    size_t bytes_to_write = out_idx * sizeof(int32_t);
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write,
                                      &bytes_written,
                                      pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(I2S_WRITE_FAIL_DELAY_MS));
    }
  }
}

static void mic_capture_task(void *arg) {
  (void)arg;

  // 32-bit stereo frames
  static int32_t rx_buf[256 * 2];
  static int16_t out_buf[256];

  int64_t acc = 0;
  uint32_t acc_count = 0;
  int16_t peak = 0;
  int64_t last_log_us = 0;
  int64_t last_tx_notify_us = 0;
  uint32_t raw_log_frames = 0;

  while (true) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, rx_buf, sizeof(rx_buf),
                                     &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
      continue;
    }
    if (!s_mic_enabled || !s_mic_stream || bytes_read == 0) {
      continue;
    }

    size_t frames = bytes_read / (sizeof(int32_t) * 2);
    size_t out_samples = 0;

    if (raw_log_frames < 8 && frames > 0) {
      ESP_LOGI(TAG,
               "Mic raw L/R[0]=%ld/%ld L/R[1]=%ld/%ld",
               (long)rx_buf[0],
               (long)rx_buf[1],
               (long)rx_buf[2],
               (long)rx_buf[3]);
      raw_log_frames++;
    }

    for (size_t i = 0; i < frames; i++) {
      int32_t l = rx_buf[i * 2];
      int32_t mixed = l;
      int16_t sample16 = (int16_t)(mixed >> MIC_SHIFT_BITS);

      // DCブロックフィルタを適用
      float y = (float)sample16 - s_mic_dc_x + MIC_DC_BLOCK_R * s_mic_dc_y;
      s_mic_dc_x = (float)sample16;
      s_mic_dc_y = y;
      int16_t filtered = (int16_t)lrintf(y);
      
      // クリッピング
      if (filtered > 32767) {
        filtered = 32767;
      } else if (filtered < -32768) {
        filtered = -32768;
      }

      acc += filtered;
      acc_count++;

      if (acc_count >= MIC_DOWNSAMPLE_FACTOR) {
        int32_t avg = (int32_t)(acc / (int64_t)MIC_DOWNSAMPLE_FACTOR);
        if (avg > 32767) {
          avg = 32767;
        } else if (avg < -32768) {
          avg = -32768;
        }
        if (abs(avg) > peak) {
          peak = abs(avg);
        }
        out_buf[out_samples++] = (int16_t)avg;
        acc = 0;
        acc_count = 0;

        if (out_samples >= (sizeof(out_buf) / sizeof(out_buf[0]))) {
          break;
        }
      }
    }

    if (out_samples == 0) {
      continue;
    }
    size_t bytes_to_send = out_samples * sizeof(int16_t);
    if (peak < MIC_NOISE_GATE_THRESHOLD) {
      memset(out_buf, 0, bytes_to_send);
    }
    size_t offset = 0;
    size_t written_total = 0;
    while (offset < bytes_to_send) {
      size_t chunk = bytes_to_send - offset;
      if (chunk > MIC_STREAM_CHUNK_BYTES) {
        chunk = MIC_STREAM_CHUNK_BYTES;
      }
      size_t written = xStreamBufferSend(
          s_mic_stream, ((uint8_t *)out_buf) + offset, chunk,
          pdMS_TO_TICKS(20));
      if (written < chunk) {
        s_mic_drop_bytes += (uint32_t)(chunk - written);
        if ((s_mic_drop_bytes % 2048) < (chunk - written)) {
          ESP_LOGW(TAG, "Dropped mic bytes: %u", s_mic_drop_bytes);
        }
        break;
      }
      written_total += written;
      offset += written;
    }
    int64_t now_us = esp_timer_get_time();
    if (written_total > 0 && s_hfp_audio_active &&
        hfp_get_audio_state() == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED &&
        now_us - last_tx_notify_us >= 20000) {
      esp_hf_client_outgoing_data_ready();
      last_tx_notify_us = now_us;
    }
    if (now_us - last_log_us > 1000000) {
      ESP_LOGI(TAG, "Mic peak: %d", peak);
      peak = 0;
      last_log_us = now_us;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

esp_err_t audio_i2s_init(void) {
  esp_err_t err = ESP_OK;

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_config_t tx_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
      .slot_cfg =
          {
              .data_bit_width = I2S_TX_BITS_PER_SAMPLE,
              .slot_bit_width = I2S_TX_SLOT_BITS,
              .slot_mode = I2S_CHANNEL_MODE,
              .slot_mask = I2S_STD_SLOT_BOTH,
              .ws_width = I2S_TX_WS_WIDTH,
              .ws_pol = false, // LRCLK極性: Lowで左チャンネル
              .bit_shift =
                  true, // Philips I2S: 1ビットシフト（MSBが1クロック遅れる）
#if SOC_I2S_HW_VERSION_1
              .msb_right = true,
#endif
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

  i2s_std_config_t rx_cfg = tx_cfg;
  tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  rx_cfg.slot_cfg.data_bit_width = I2S_RX_BITS_PER_SAMPLE;
  rx_cfg.slot_cfg.slot_bit_width = I2S_RX_SLOT_BITS;
  rx_cfg.slot_cfg.ws_width = I2S_RX_WS_WIDTH;
  rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  rx_cfg.gpio_cfg.din = I2S_DIN_GPIO;
  err = i2s_channel_init_std_mode(s_tx_chan, &tx_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
    return err;
  }
  err = i2s_channel_init_std_mode(s_rx_chan, &rx_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S std RX init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = i2s_channel_enable(s_tx_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
    return err;
  }
  s_tx_enabled = true;
  err = i2s_channel_enable(s_rx_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S RX channel enable failed: %s", esp_err_to_name(err));
    return err;
  }

  s_audio_stream = xStreamBufferCreate(AUDIO_STREAM_BUF_SIZE, 1);
  if (!s_audio_stream) {
    ESP_LOGE(TAG, "Audio stream buffer allocation failed");
    return ESP_ERR_NO_MEM;
  }
  s_mic_stream = xStreamBufferCreate(MIC_STREAM_BUF_SIZE, 1);
  if (!s_mic_stream) {
    ESP_LOGE(TAG, "Mic stream buffer allocation failed");
    return ESP_ERR_NO_MEM;
  }
  xTaskCreate(audio_i2s_task, "audio_i2s_task", AUDIO_TASK_STACK_SIZE, NULL, 5,
              &s_audio_task);
  xTaskCreate(mic_capture_task, "mic_capture_task", MIC_TASK_STACK_SIZE, NULL,
              5, NULL);
  s_audio_ready = true;

  ESP_LOGI(TAG, "I2S ready (%d Hz, 32-bit stereo).", I2S_SAMPLE_RATE);
  if (HFP_UPSAMPLE_FACTOR > 1) {
    ESP_LOGI(TAG, "HFP upsample: %d Hz -> %d Hz (x%d)", HFP_SAMPLE_RATE,
             I2S_SAMPLE_RATE, HFP_UPSAMPLE_FACTOR);
  }
#if (I2S_SAMPLE_RATE % HFP_SAMPLE_RATE) == 0
  ESP_LOGI(TAG, "Mic downsample: %d Hz -> %d Hz (/%d)", I2S_SAMPLE_RATE,
           HFP_SAMPLE_RATE, MIC_DOWNSAMPLE_FACTOR);
#else
  ESP_LOGW(TAG, "Mic sample rate mismatch: %d Hz vs %d Hz; no downsample",
           I2S_SAMPLE_RATE, HFP_SAMPLE_RATE);
#endif
#if (I2S_SAMPLE_RATE % HFP_SAMPLE_RATE) != 0
  ESP_LOGW(TAG, "HFP sample rate mismatch: %d Hz vs %d Hz; no upsample",
           HFP_SAMPLE_RATE, I2S_SAMPLE_RATE);
#endif
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

size_t audio_i2s_pop_mic_audio(uint8_t *dst, size_t len) {
  if (!s_audio_ready || !s_mic_enabled || !s_mic_stream || !dst || len == 0) {
    return 0;
  }
  return xStreamBufferReceive(s_mic_stream, dst, len, 0);
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


void audio_i2s_set_hfp_audio_active(bool active) {
  s_hfp_audio_active = active;
}

void audio_i2s_set_hfp_enabled(bool enabled) { s_hfp_enabled = enabled; }

void audio_i2s_set_mic_enabled(bool enabled) {
  s_mic_enabled = enabled;
  if (s_mic_stream) {
    xStreamBufferReset(s_mic_stream);
  }
}

void audio_i2s_set_tx_enabled(bool enabled) {
  if (!s_audio_ready || !s_tx_chan) {
    return;
  }
  if (enabled == s_tx_enabled) {
    return;
  }
  esp_err_t err = enabled ? i2s_channel_enable(s_tx_chan)
                          : i2s_channel_disable(s_tx_chan);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2S TX %s failed: %s", enabled ? "enable" : "disable",
             esp_err_to_name(err));
    return;
  }
  s_tx_enabled = enabled;
  ESP_LOGI(TAG, "I2S TX %s", enabled ? "enabled" : "disabled");
  if (s_audio_task) {
    if (enabled) {
      vTaskResume(s_audio_task);
    } else {
      vTaskSuspend(s_audio_task);
    }
  }
}

void audio_i2s_clear_buffer(void) {
  if (!s_audio_ready || !s_audio_stream) {
    return;
  }
  xStreamBufferReset(s_audio_stream);
}

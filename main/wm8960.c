#include "wm8960.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define WM8960_I2C_ADDR 0x1A

#define WM8960_REG_LINPUT_VOLUME 0x00
#define WM8960_REG_RINPUT_VOLUME 0x01
#define WM8960_REG_LOUT1_VOLUME 0x02
#define WM8960_REG_ROUT1_VOLUME 0x03
#define WM8960_REG_CLOCKING1 0x04
#define WM8960_REG_ADC_DAC_CTRL1 0x05
#define WM8960_REG_AUDIO_INTERFACE 0x07
#define WM8960_REG_CLOCKING2 0x08
#define WM8960_REG_AUDIO_INTERFACE2 0x09
#define WM8960_REG_LDAC_VOLUME 0x0A
#define WM8960_REG_RDAC_VOLUME 0x0B
#define WM8960_REG_POWER_MGMT1 0x19
#define WM8960_REG_POWER_MGMT2 0x1A
#define WM8960_REG_POWER_MGMT3 0x1B
#define WM8960_REG_RESET 0x0F

#define WM8960_I2C_PORT I2C_NUM_0
#define WM8960_I2C_SDA GPIO_NUM_2
#define WM8960_I2C_SCL GPIO_NUM_3

static const char *TAG = "wm8960";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;
static bool s_i2c_ready = false;

static esp_err_t wm8960_i2c_init(void) {
    if (s_i2c_ready) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = WM8960_I2C_PORT,
        .sda_io_num = WM8960_I2C_SDA,
        .scl_io_num = WM8960_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WM8960_I2C_ADDR,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    s_i2c_ready = true;
    return ESP_OK;
}

esp_err_t wm8960_write_reg(uint8_t reg, uint16_t val) {
    if (!s_i2c_ready) {
        esp_err_t err = wm8960_i2c_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    uint8_t data[2];
    data[0] = (reg << 1) | ((val >> 8) & 0x01);
    data[1] = val & 0xFF;

    return i2c_master_transmit(s_i2c_dev, data, sizeof(data), 100);
}

esp_err_t wm8960_init(void) {
    esp_err_t err = wm8960_i2c_init();
    if (err != ESP_OK) {
        return err;
    }

    err = wm8960_write_reg(WM8960_REG_RESET, 0x000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wm8960_write_reg(WM8960_REG_POWER_MGMT1, 0x0C0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Power mgmt1 precharge failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wm8960_write_reg(WM8960_REG_AUDIO_INTERFACE, 0x002);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio interface failed: %s", esp_err_to_name(err));
        return err;
    }
    err = wm8960_write_reg(WM8960_REG_AUDIO_INTERFACE2, 0x040);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio interface2 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wm8960_write_reg(WM8960_REG_ADC_DAC_CTRL1, 0x000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DAC unmute failed: %s", esp_err_to_name(err));
        return err;
    }

    wm8960_write_reg(WM8960_REG_LDAC_VOLUME, 0x1FF);
    wm8960_write_reg(WM8960_REG_RDAC_VOLUME, 0x1FF);

    wm8960_write_reg(0x22, 0x180);
    wm8960_write_reg(0x25, 0x180);

    wm8960_write_reg(WM8960_REG_POWER_MGMT1, 0x1C0);
    wm8960_write_reg(WM8960_REG_POWER_MGMT2, 0x1F9);
    wm8960_write_reg(WM8960_REG_POWER_MGMT3, 0x0FC);

    wm8960_write_reg(WM8960_REG_LOUT1_VOLUME, 0x179);
    wm8960_write_reg(WM8960_REG_ROUT1_VOLUME, 0x179);

    ESP_LOGI(TAG, "WM8960 initialized (basic I2S slave config).");
    return ESP_OK;
}

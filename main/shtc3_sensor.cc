#include "shtc3_sensor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "Shtc3";
constexpr uint32_t kI2cClockHz = 100000;
constexpr TickType_t kI2cTimeout = pdMS_TO_TICKS(120);
constexpr uint16_t kCmdWakeup = 0x3517;
constexpr uint16_t kCmdSleep = 0xB098;
constexpr uint16_t kCmdSoftReset = 0x805D;
constexpr uint16_t kCmdMeasureNormalNoClockStretch = 0x7866;
}  // namespace

bool Shtc3Sensor::Init() {
    i2c_config_t config = {};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = WEC_I2C_SDA_PIN;
    config.scl_io_num = WEC_I2C_SCL_PIN;
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.master.clk_speed = kI2cClockHz;

    esp_err_t err = i2c_param_config(WEC_I2C_HOST, &config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(WEC_I2C_HOST, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    WriteCommand(kCmdWakeup);
    vTaskDelay(pdMS_TO_TICKS(2));
    WriteCommand(kCmdSoftReset);
    vTaskDelay(pdMS_TO_TICKS(2));
    WriteCommand(kCmdSleep);
    ESP_LOGI(kTag, "SHTC3 ready on I2C addr 0x%02x", WEC_SHTC3_I2C_ADDR);
    return true;
}

bool Shtc3Sensor::Read(float* temperature_c, float* humidity_percent) {
    if (!initialized_ || !temperature_c || !humidity_percent) {
        return false;
    }

    if (WriteCommand(kCmdWakeup) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (WriteCommand(kCmdMeasureNormalNoClockStretch) != ESP_OK) {
        WriteCommand(kCmdSleep);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6] = {};
    const esp_err_t err = ReadBytes(data, sizeof(data));
    WriteCommand(kCmdSleep);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "read failed: %s", esp_err_to_name(err));
        return false;
    }

    const uint16_t raw_t = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    const uint16_t raw_rh = (static_cast<uint16_t>(data[3]) << 8) | data[4];
    if (!CheckCrc(raw_t, data[2]) || !CheckCrc(raw_rh, data[5])) {
        ESP_LOGW(kTag, "crc check failed");
        return false;
    }

    const float temperature = -45.0f + 175.0f * static_cast<float>(raw_t) / 65535.0f;
    const float humidity = 100.0f * static_cast<float>(raw_rh) / 65535.0f;
    if (!std::isfinite(temperature) || !std::isfinite(humidity)) {
        return false;
    }
    *temperature_c = temperature;
    *humidity_percent = std::min(100.0f, std::max(0.0f, humidity));
    return true;
}

esp_err_t Shtc3Sensor::WriteCommand(uint16_t command) {
    const uint8_t data[2] = {
        static_cast<uint8_t>(command >> 8),
        static_cast<uint8_t>(command & 0xff),
    };
    return i2c_master_write_to_device(WEC_I2C_HOST, WEC_SHTC3_I2C_ADDR, data,
                                      sizeof(data), kI2cTimeout);
}

esp_err_t Shtc3Sensor::ReadBytes(uint8_t* data, size_t len) {
    return i2c_master_read_from_device(WEC_I2C_HOST, WEC_SHTC3_I2C_ADDR, data, len,
                                       kI2cTimeout);
}

bool Shtc3Sensor::CheckCrc(uint16_t value, uint8_t crc) const {
    uint8_t calc = 0xff;
    uint8_t bytes[2] = {
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xff),
    };
    for (uint8_t byte : bytes) {
        calc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            calc = (calc & 0x80) ? static_cast<uint8_t>((calc << 1) ^ 0x31)
                                 : static_cast<uint8_t>(calc << 1);
        }
    }
    return calc == crc;
}

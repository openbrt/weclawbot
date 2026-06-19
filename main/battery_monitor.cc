#include "battery_monitor.h"

#include <algorithm>

#include <esp_log.h>
#include <sdkconfig.h>

#include "app_config.h"

#ifndef CONFIG_WEC_BATTERY_INSTALLED
#define CONFIG_WEC_BATTERY_INSTALLED 0
#endif

namespace {
constexpr char kTag[] = "Battery";
constexpr adc_channel_t kBatteryChannel = ADC_CHANNEL_3;  // GPIO4 on ESP32-S3.
constexpr float kReferenceVoltage = 3.3f;
constexpr float kDividerRatio = 3.0f;
constexpr float kBatteryPresentVoltage = 3.0f;
constexpr float kBatteryEmptyVoltage = 3.25f;
constexpr float kBatteryFullVoltage = 4.15f;

int PercentFromVoltage(float voltage) {
    const float normalized = (voltage - kBatteryEmptyVoltage) /
                             (kBatteryFullVoltage - kBatteryEmptyVoltage);
    return std::max(0, std::min(100, static_cast<int>(normalized * 100.0f + 0.5f)));
}
}  // namespace

bool BatteryMonitor::Init() {
    if (initialized_) {
        return true;
    }

    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &unit_);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    channel_config.atten = ADC_ATTEN_DB_12;
    err = adc_oneshot_config_channel(unit_, kBatteryChannel, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "adc channel config failed: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    ESP_LOGI(kTag, "battery voltage monitor ready on GPIO%d/ADC1_CH3",
             static_cast<int>(WEC_BATTERY_ADC_GPIO));
    return true;
}

BatteryReading BatteryMonitor::Read() {
    BatteryReading reading;
    if (!initialized_ && !Init()) {
        return reading;
    }

    int total = 0;
    int samples = 0;
    for (int i = 0; i < 8; ++i) {
        int raw = 0;
        if (adc_oneshot_read(unit_, kBatteryChannel, &raw) == ESP_OK) {
            total += raw;
            ++samples;
        }
    }
    if (samples == 0) {
        return reading;
    }

    const float raw = static_cast<float>(total) / static_cast<float>(samples);
    const float adc_voltage = raw * kReferenceVoltage / 4095.0f;
    reading.valid = true;
    reading.voltage = adc_voltage * kDividerRatio;
    reading.present = CONFIG_WEC_BATTERY_INSTALLED &&
                      reading.voltage >= kBatteryPresentVoltage;
    reading.percent = reading.present ? PercentFromVoltage(reading.voltage) : 0;
    return reading;
}

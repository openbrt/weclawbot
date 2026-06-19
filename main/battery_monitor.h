#pragma once

#include <esp_adc/adc_oneshot.h>

struct BatteryReading {
    bool valid = false;
    bool present = false;
    float voltage = 0.0f;
    int percent = 0;
};

class BatteryMonitor {
public:
    bool Init();
    BatteryReading Read();

private:
    adc_oneshot_unit_handle_t unit_ = nullptr;
    bool initialized_ = false;
};

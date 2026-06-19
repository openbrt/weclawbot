#pragma once

#include <esp_err.h>

class Shtc3Sensor {
public:
    bool Init();
    bool Read(float* temperature_c, float* humidity_percent);

private:
    esp_err_t WriteCommand(uint16_t command);
    esp_err_t ReadBytes(uint8_t* data, size_t len);
    bool CheckCrc(uint16_t value, uint8_t crc) const;

    bool initialized_ = false;
};

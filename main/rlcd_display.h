#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/spi_master.h>
#include <lvgl.h>

class RlcdDisplay {
public:
    bool Init();
    void Lock();
    void Unlock();
    void ShowMonoBitmap(const uint8_t* data, size_t len, int width, int height, int stride_bytes);
    lv_display_t* LvDisplay() const { return lv_display_; }

private:
    static void FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);

    void SendCommand(uint8_t command);
    void SendData(uint8_t data);
    void SendBuffer(const uint8_t* data, int len);
    void Reset();
    void InitPanel();
    void ClearBuffer(uint8_t color);
    void SetPixel(uint16_t x, uint16_t y, bool black);
    void PushFrame();

    spi_device_handle_t spi_ = nullptr;
    lv_display_t* lv_display_ = nullptr;
    uint8_t* frame_buffer_ = nullptr;
};

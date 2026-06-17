#include "rlcd_display.h"

#include <cstring>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "RlcdDisplay";

constexpr uint8_t kWhiteByte = 0xFF;
constexpr uint8_t kBlackByte = 0x00;
}  // namespace

bool RlcdDisplay::Init() {
    ESP_LOGI(kTag, "Initializing RLCD SPI");

    spi_bus_config_t bus_config = {};
    bus_config.miso_io_num = -1;
    bus_config.mosi_io_num = WEC_RLCD_MOSI_PIN;
    bus_config.sclk_io_num = WEC_RLCD_SCK_PIN;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = WEC_RLCD_FRAME_BYTES + 16;

    esp_err_t err = spi_bus_initialize(WEC_RLCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev_config = {};
    dev_config.clock_speed_hz = 40 * 1000 * 1000;
    dev_config.mode = 0;
    dev_config.spics_io_num = WEC_RLCD_CS_PIN;
    dev_config.queue_size = 1;

    err = spi_bus_add_device(WEC_RLCD_SPI_HOST, &dev_config, &spi_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    gpio_config_t io_config = {};
    io_config.intr_type = GPIO_INTR_DISABLE;
    io_config.mode = GPIO_MODE_OUTPUT;
    io_config.pin_bit_mask = (1ULL << WEC_RLCD_DC_PIN) | (1ULL << WEC_RLCD_RST_PIN);
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.pull_up_en = GPIO_PULLUP_ENABLE;
    err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "gpio_config failed: %s", esp_err_to_name(err));
        return false;
    }

    frame_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(
        WEC_RLCD_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!frame_buffer_) {
        ESP_LOGE(kTag, "Unable to allocate %d-byte RLCD frame buffer", WEC_RLCD_FRAME_BYTES);
        return false;
    }
    ClearBuffer(kWhiteByte);

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return false;
    }

    lv_display_ = lv_display_create(WEC_RLCD_WIDTH, WEC_RLCD_HEIGHT);
    if (!lv_display_) {
        ESP_LOGE(kTag, "lv_display_create failed");
        return false;
    }
    lv_display_set_flush_cb(lv_display_, FlushCallback);
    lv_display_set_user_data(lv_display_, this);

    const size_t rows = 48;
    const size_t draw_buffer_size = WEC_RLCD_WIDTH * rows * sizeof(uint16_t);
    auto* draw_buffer = static_cast<uint8_t*>(
        heap_caps_malloc(draw_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!draw_buffer) {
        draw_buffer = static_cast<uint8_t*>(
            heap_caps_malloc(draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!draw_buffer) {
        ESP_LOGE(kTag, "Unable to allocate LVGL draw buffer");
        return false;
    }
    lv_display_set_buffers(lv_display_, draw_buffer, nullptr, draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    InitPanel();
    PushFrame();
    return true;
}

void RlcdDisplay::Lock() {
    lvgl_port_lock(0);
}

void RlcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void RlcdDisplay::ShowMonoBitmap(const uint8_t* data, size_t len, int width, int height, int stride_bytes) {
    if (!data || width <= 0 || height <= 0 || stride_bytes <= 0) {
        return;
    }
    const size_t needed = static_cast<size_t>(stride_bytes) * static_cast<size_t>(height);
    if (len < needed) {
        return;
    }

    ClearBuffer(kWhiteByte);
    const int x0 = (WEC_RLCD_WIDTH - width) / 2;
    const int y0 = (WEC_RLCD_HEIGHT - height) / 2;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * stride_bytes;
        const int dst_y = y0 + y;
        if (dst_y < 0 || dst_y >= WEC_RLCD_HEIGHT) {
            continue;
        }
        for (int x = 0; x < width; ++x) {
            const int dst_x = x0 + x;
            if (dst_x < 0 || dst_x >= WEC_RLCD_WIDTH) {
                continue;
            }
            const bool black = (row[x >> 3] & (0x80 >> (x & 7))) != 0;
            SetPixel(static_cast<uint16_t>(dst_x), static_cast<uint16_t>(dst_y), black);
        }
    }
    PushFrame();
}

void RlcdDisplay::FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p) {
    auto* self = static_cast<RlcdDisplay*>(lv_display_get_user_data(disp));
    auto* pixels = reinterpret_cast<uint16_t*>(color_p);

    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            const uint16_t rgb565 = *pixels++;
            self->SetPixel(static_cast<uint16_t>(x), static_cast<uint16_t>(y), rgb565 < 0x7FFF);
        }
    }

    self->PushFrame();
    lv_display_flush_ready(disp);
}

void RlcdDisplay::SendCommand(uint8_t command) {
    gpio_set_level(WEC_RLCD_DC_PIN, 0);
    spi_transaction_t tx = {};
    tx.length = 8;
    tx.tx_buffer = &command;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &tx));
}

void RlcdDisplay::SendData(uint8_t data) {
    gpio_set_level(WEC_RLCD_DC_PIN, 1);
    spi_transaction_t tx = {};
    tx.length = 8;
    tx.tx_buffer = &data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &tx));
}

void RlcdDisplay::SendBuffer(const uint8_t* data, int len) {
    gpio_set_level(WEC_RLCD_DC_PIN, 1);
    spi_transaction_t tx = {};
    tx.length = len * 8;
    tx.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &tx));
}

void RlcdDisplay::Reset() {
    gpio_set_level(WEC_RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(WEC_RLCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(WEC_RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void RlcdDisplay::InitPanel() {
    Reset();

    SendCommand(0xD6);
    SendData(0x17);
    SendData(0x02);

    SendCommand(0xD1);
    SendData(0x01);

    SendCommand(0xC0);
    SendData(0x11);
    SendData(0x04);

    SendCommand(0xC1);
    SendData(0x69);
    SendData(0x69);
    SendData(0x69);
    SendData(0x69);

    SendCommand(0xC2);
    SendData(0x19);
    SendData(0x19);
    SendData(0x19);
    SendData(0x19);

    SendCommand(0xC4);
    SendData(0x4B);
    SendData(0x4B);
    SendData(0x4B);
    SendData(0x4B);

    SendCommand(0xC5);
    SendData(0x19);
    SendData(0x19);
    SendData(0x19);
    SendData(0x19);

    SendCommand(0xD8);
    SendData(0x80);
    SendData(0xE9);

    SendCommand(0xB2);
    SendData(0x02);

    SendCommand(0xB3);
    SendData(0xE5);
    SendData(0xF6);
    SendData(0x05);
    SendData(0x46);
    SendData(0x77);
    SendData(0x77);
    SendData(0x77);
    SendData(0x77);
    SendData(0x76);
    SendData(0x45);

    SendCommand(0xB4);
    SendData(0x05);
    SendData(0x46);
    SendData(0x77);
    SendData(0x77);
    SendData(0x77);
    SendData(0x77);
    SendData(0x76);
    SendData(0x45);

    SendCommand(0x62);
    SendData(0x32);
    SendData(0x03);
    SendData(0x1F);

    SendCommand(0xB7);
    SendData(0x13);

    SendCommand(0xB0);
    SendData(0x64);

    SendCommand(0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    SendCommand(0xC9);
    SendData(0x00);

    SendCommand(0x36);
    SendData(0x48);

    SendCommand(0x3A);
    SendData(0x11);

    SendCommand(0xB9);
    SendData(0x20);

    SendCommand(0xB8);
    SendData(0x29);

    SendCommand(0x21);

    SendCommand(0x2A);
    SendData(0x12);
    SendData(0x2A);

    SendCommand(0x2B);
    SendData(0x00);
    SendData(0xC7);

    SendCommand(0x35);
    SendData(0x00);

    SendCommand(0xD0);
    SendData(0xFF);

    SendCommand(0x38);
    SendCommand(0x29);
}

void RlcdDisplay::ClearBuffer(uint8_t color) {
    std::memset(frame_buffer_, color, WEC_RLCD_FRAME_BYTES);
}

void RlcdDisplay::SetPixel(uint16_t x, uint16_t y, bool black) {
    if (x >= WEC_RLCD_WIDTH || y >= WEC_RLCD_HEIGHT) {
        return;
    }

    const uint16_t inv_y = WEC_RLCD_HEIGHT - 1 - y;
    const uint16_t block_y = inv_y >> 2;
    const uint8_t local_y = inv_y & 3;
    const uint16_t byte_x = x >> 1;
    const uint8_t local_x = x & 1;
    const uint32_t index = byte_x * (WEC_RLCD_HEIGHT >> 2) + block_y;
    const uint8_t mask = 1 << (7 - ((local_y << 1) | local_x));

    if (black) {
        frame_buffer_[index] &= ~mask;
    } else {
        frame_buffer_[index] |= mask;
    }
}

void RlcdDisplay::PushFrame() {
    SendCommand(0x2A);
    SendData(0x12);
    SendData(0x2A);

    SendCommand(0x2B);
    SendData(0x00);
    SendData(0xC7);

    SendCommand(0x2C);
    SendBuffer(frame_buffer_, WEC_RLCD_FRAME_BYTES);
}

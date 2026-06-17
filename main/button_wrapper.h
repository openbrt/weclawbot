#pragma once

#include <functional>

#include <button_gpio.h>
#include <button_types.h>
#include <driver/gpio.h>
#include <iot_button.h>

class GpioButton {
public:
    GpioButton(gpio_num_t gpio_num,
               bool active_high = false,
               uint16_t long_press_time = 0,
               uint16_t short_press_time = 0,
               bool enable_power_save = false);
    ~GpioButton();

    GpioButton(const GpioButton&) = delete;
    GpioButton& operator=(const GpioButton&) = delete;

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);

private:
    button_handle_t button_handle_ = nullptr;
    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
};

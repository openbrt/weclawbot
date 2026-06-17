#include "button_wrapper.h"

#include <esp_check.h>

GpioButton::GpioButton(gpio_num_t gpio_num,
                       bool active_high,
                       uint16_t long_press_time,
                       uint16_t short_press_time,
                       bool enable_power_save) {
    if (gpio_num == GPIO_NUM_NC) {
        return;
    }

    button_config_t button_config = {
        .long_press_time = long_press_time,
        .short_press_time = short_press_time,
    };
    button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = static_cast<uint8_t>(active_high ? 1 : 0),
        .enable_power_save = enable_power_save,
        .disable_pull = false,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
}

GpioButton::~GpioButton() {
    if (button_handle_ != nullptr) {
        iot_button_delete(button_handle_);
    }
}

void GpioButton::OnPressDown(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_down_ = std::move(callback);
    iot_button_register_cb(button_handle_, BUTTON_PRESS_DOWN, nullptr, [](void*, void* usr_data) {
        auto* button = static_cast<GpioButton*>(usr_data);
        if (button->on_press_down_) {
            button->on_press_down_();
        }
    }, this);
}

void GpioButton::OnPressUp(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_up_ = std::move(callback);
    iot_button_register_cb(button_handle_, BUTTON_PRESS_UP, nullptr, [](void*, void* usr_data) {
        auto* button = static_cast<GpioButton*>(usr_data);
        if (button->on_press_up_) {
            button->on_press_up_();
        }
    }, this);
}

void GpioButton::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = std::move(callback);
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void*, void* usr_data) {
        auto* button = static_cast<GpioButton*>(usr_data);
        if (button->on_long_press_) {
            button->on_long_press_();
        }
    }, this);
}

void GpioButton::OnClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_click_ = std::move(callback);
    iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void*, void* usr_data) {
        auto* button = static_cast<GpioButton*>(usr_data);
        if (button->on_click_) {
            button->on_click_();
        }
    }, this);
}

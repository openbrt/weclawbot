#include "button_manager.h"

#include <cstdio>
#include <memory>

#include <driver/gpio.h>
#include <esp_log.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "ButtonManager";
constexpr uint16_t kLeftLongMs = 3000;
constexpr uint16_t kRightLongMs = 5000;

void PrintButtonEvent(const char* name, gpio_num_t gpio, const char* event) {
    std::printf(
        "WEC:{\"scope\":\"button\",\"stage\":\"%s\",\"name\":\"%s\",\"gpio\":%d,"
        "\"ok\":true,\"type\":\"event\"}\n",
        event,
        name,
        static_cast<int>(gpio));
    std::fflush(stdout);
}
}  // namespace

bool ButtonManager::Init() {
    if (initialized_) {
        return true;
    }

    left_button_ = std::make_unique<GpioButton>(
        WEC_KEY_BUTTON_GPIO, /*active_high=*/false, /*long_press_time=*/kLeftLongMs);
    right_button_ = std::make_unique<GpioButton>(
        WEC_BOOT_BUTTON_GPIO, /*active_high=*/false, /*long_press_time=*/kRightLongMs);

    left_button_->OnPressDown([]() {
        PrintButtonEvent("left", WEC_KEY_BUTTON_GPIO, "press_down");
    });
    left_button_->OnPressUp([]() {
        PrintButtonEvent("left", WEC_KEY_BUTTON_GPIO, "press_up");
    });
    left_button_->OnClick([this]() {
        PrintButtonEvent("left", WEC_KEY_BUTTON_GPIO, "short_press");
        HandleLeftShortPress();
    });
    left_button_->OnLongPress([this]() {
        PrintButtonEvent("left", WEC_KEY_BUTTON_GPIO, "long_press");
        HandleLeftLongPress();
    });

    right_button_->OnPressDown([]() {
        PrintButtonEvent("right", WEC_BOOT_BUTTON_GPIO, "press_down");
    });
    right_button_->OnPressUp([]() {
        PrintButtonEvent("right", WEC_BOOT_BUTTON_GPIO, "press_up");
    });
    right_button_->OnClick([this]() {
        PrintButtonEvent("right", WEC_BOOT_BUTTON_GPIO, "short_press");
        HandleRightShortPress();
    });
    right_button_->OnLongPress([this]() {
        PrintButtonEvent("right", WEC_BOOT_BUTTON_GPIO, "long_press");
        HandleRightLongPress();
    });

    initialized_ = true;
    ESP_LOGI(kTag,
             "Buttons ready via espressif/button: left KEY GPIO%d prev/clear-text(%ums), "
             "right BOOT GPIO%d next/full-clear(%ums)",
             static_cast<int>(WEC_KEY_BUTTON_GPIO),
             kLeftLongMs,
             static_cast<int>(WEC_BOOT_BUTTON_GPIO),
             kRightLongMs);
    return true;
}

void ButtonManager::HandleLeftShortPress() {
    if (!ui_.ShowPreviousNotePage(notes_.All(), notes_.CurrentIndex())) {
        RenderCurrentOrEmpty();
    }
}

void ButtonManager::HandleRightShortPress() {
    if (!ui_.ShowNextNotePage(notes_.All(), notes_.CurrentIndex())) {
        RenderCurrentOrEmpty();
    }
}

void ButtonManager::HandleLeftLongPress() {
    ClearTextNotes();
}

void ButtonManager::HandleRightLongPress() {
    ClearAllAndRelogin();
}

void ButtonManager::RenderCurrentOrEmpty() {
    if (!notes_.Empty()) {
        ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    } else if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
    } else {
        ui_.ShowEmptyNotes();
    }
}

void ButtonManager::ClearTextNotes() {
    notes_.ClearAll();
    RenderCurrentOrEmpty();
}

void ButtonManager::ClearAllAndRelogin() {
    notes_.ClearAll();
    notes_.ClearIdlePhoto();
    ui_.ShowBoot("已全清，正在准备扫码");
    bot_.ClearSavedCredentials();
}

#include "button_manager.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <driver/gpio.h>
#include <esp_log.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "ButtonManager";
constexpr uint16_t kLeftLongMs = 3000;
constexpr uint16_t kRightLongMs = 5000;
constexpr size_t kMaxManualNotePages = 3;

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
    ShowPreviousManualScreen();
}

void ButtonManager::HandleRightShortPress() {
    ShowNextManualScreen();
}

void ButtonManager::HandleLeftLongPress() {
    ClearTextNotes();
}

void ButtonManager::HandleRightLongPress() {
    ClearAllAndRelogin();
}

void ButtonManager::ShowPreviousManualScreen() {
    switch (ui_.DashboardView()) {
        case UiDashboardView::kCalendar:
            if (ShowLastNotePage() || ShowPhoto()) {
                return;
            }
            ShowCalendar();
            return;
        case UiDashboardView::kPhoto:
            ShowCalendar();
            return;
        case UiDashboardView::kNote:
            if (ui_.NotePage() > 0 && ShowNotePage(ui_.NotePage() - 1)) {
                return;
            }
            if (ShowPhoto()) {
                return;
            }
            ShowCalendar();
            return;
        case UiDashboardView::kOther:
            if (ShowLastNotePage() || ShowPhoto()) {
                return;
            }
            ShowCalendar();
            return;
    }
}

void ButtonManager::ShowNextManualScreen() {
    switch (ui_.DashboardView()) {
        case UiDashboardView::kCalendar:
            if (ShowPhoto() || ShowNotePage(0)) {
                return;
            }
            ShowCalendar();
            return;
        case UiDashboardView::kPhoto:
            if (ShowNotePage(0)) {
                return;
            }
            ShowCalendar();
            return;
        case UiDashboardView::kNote: {
            const size_t page_count = ManualNotePageCount();
            if (ui_.NotePage() + 1 < page_count && ShowNotePage(ui_.NotePage() + 1)) {
                return;
            }
            ShowCalendar();
            return;
        }
        case UiDashboardView::kOther:
            ShowCalendar();
            return;
    }
}

bool ButtonManager::ShowCalendar() {
    if (bot_.UsesCustomAgent() && bot_.AgentPaired()) {
        ui_.ShowAgentDashboard();
    } else {
        ui_.ShowEmptyNotes();
    }
    return true;
}

bool ButtonManager::ShowPhoto() {
    if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
        return true;
    }
    return false;
}

bool ButtonManager::ShowNotePage(size_t page) {
    const size_t page_count = ManualNotePageCount();
    if (page_count == 0) {
        return false;
    }
    page = std::min(page, page_count - 1);
    return ui_.ShowNotePage(notes_.All(), notes_.CurrentIndex(), page);
}

bool ButtonManager::ShowLastNotePage() {
    const size_t page_count = ManualNotePageCount();
    return page_count > 0 && ShowNotePage(page_count - 1);
}

size_t ButtonManager::ManualNotePageCount() const {
    const Note* note = notes_.Current();
    if (!note) {
        return 0;
    }
    const size_t page_count = ui_.NotePageCountFor(*note);
    return std::min(page_count, kMaxManualNotePages);
}

void ButtonManager::RenderCurrentOrEmpty() {
    if (!notes_.Empty()) {
        ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    } else if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
    } else if (bot_.UsesCustomAgent() && bot_.AgentPaired()) {
        ui_.ShowAgentDashboard();
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
    if (bot_.WechatIngressEnabled()) {
        ui_.ShowBoot("已全清，正在准备微信扫码");
        bot_.ClearSavedCredentials();
    } else {
        ui_.ShowAgentDashboard("已全清，等待智能体");
    }
}

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <string>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !CONFIG_TINYUSB_CDC_ENABLED && \
    (CONFIG_USJ_ENABLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || \
     CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#define WEC_HAS_USB_SERIAL_JTAG 1
#include <driver/usb_serial_jtag.h>
#endif

#include "app_config.h"
#include "battery_monitor.h"
#include "button_manager.h"
#include "note_store.h"
#include "rlcd_display.h"
#include "serial_config.h"
#include "shtc3_sensor.h"
#include "ui.h"
#if CONFIG_WEC_PRODUCT_DISK
#include "usb_product_disk.h"
#endif
#include "wechat_bot.h"
#include "wifi_manager.h"

namespace {
constexpr char kTag[] = "WeClawBot";

RlcdDisplay display;
Ui ui(display);
NoteStore notes;
WifiManager wifi;
WechatBot bot(ui, notes);
ButtonManager buttons(ui, notes, bot);
SerialConfig serial_config(ui, wifi, notes, bot);
Shtc3Sensor shtc3;
BatteryMonitor battery;

bool UsbHostPowerConnected() {
#if WEC_HAS_USB_SERIAL_JTAG
    return usb_serial_jtag_is_connected();
#else
    return false;
#endif
}

void ConfigureLocalTimezone() {
    setenv("TZ", "CST-8", 1);
    tzset();
}

int64_t MonotonicSeconds() {
    return esp_timer_get_time() / 1000000;
}

void BotTask(void*) {
    bot.Start();
    vTaskDelete(nullptr);
}

bool StartTask(TaskFunction_t fn,
               const char* name,
               uint32_t stack_depth,
               void* arg,
               UBaseType_t priority,
               TaskHandle_t* handle = nullptr) {
    const BaseType_t ok = xTaskCreate(fn, name, stack_depth, arg, priority, handle);
    std::printf(
        "WEC:{\"scope\":\"task\",\"stage\":\"create\",\"name\":\"%s\","
        "\"stack\":%u,\"ok\":%s,\"internal_free\":%u,"
        "\"internal_largest\":%u,\"type\":\"event\"}\n",
        name,
        static_cast<unsigned>(stack_depth),
        ok == pdPASS ? "true" : "false",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    std::fflush(stdout);
    return ok == pdPASS;
}

void RenderCurrentOrEmpty() {
    if (!notes.Empty()) {
        ui.ShowNotes(notes.All(), notes.CurrentIndex());
    } else if (const Note* photo = notes.IdlePhoto()) {
        ui.ShowIdlePhoto(*photo);
    } else if (bot.UsesCustomAgent() && bot.AgentPaired()) {
        ui.ShowAgentDashboard();
    } else {
        ui.ShowEmptyNotes();
    }
}

enum class DashboardSlot {
    kCalendar = 0,
    kPhoto = 1,
    kNote = 2,
};

bool DashboardSlotAvailable(DashboardSlot slot) {
    switch (slot) {
        case DashboardSlot::kCalendar:
            return true;
        case DashboardSlot::kPhoto:
            return notes.HasIdlePhoto();
        case DashboardSlot::kNote:
            return !notes.Empty();
    }
    return false;
}

DashboardSlot DashboardSlotFromUi(UiDashboardView view) {
    switch (view) {
        case UiDashboardView::kPhoto:
            return DashboardSlot::kPhoto;
        case UiDashboardView::kNote:
            return DashboardSlot::kNote;
        case UiDashboardView::kCalendar:
        case UiDashboardView::kOther:
            return DashboardSlot::kCalendar;
    }
    return DashboardSlot::kCalendar;
}

DashboardSlot NextDashboardSlot(DashboardSlot current) {
    for (int offset = 1; offset <= 3; ++offset) {
        const auto candidate = static_cast<DashboardSlot>((static_cast<int>(current) + offset) % 3);
        if (DashboardSlotAvailable(candidate)) {
            return candidate;
        }
    }
    return DashboardSlot::kCalendar;
}

const char* DashboardSlotName(DashboardSlot slot) {
    switch (slot) {
        case DashboardSlot::kCalendar:
            return "calendar";
        case DashboardSlot::kPhoto:
            return "photo";
        case DashboardSlot::kNote:
            return "note";
    }
    return "calendar";
}

const char* DashboardViewName(UiDashboardView view) {
    switch (view) {
        case UiDashboardView::kCalendar:
            return "calendar";
        case UiDashboardView::kPhoto:
            return "photo";
        case UiDashboardView::kNote:
            return "note";
        case UiDashboardView::kOther:
            return "other";
    }
    return "other";
}

void ShowDashboardSlot(DashboardSlot slot) {
    switch (slot) {
        case DashboardSlot::kCalendar:
            if (bot.UsesCustomAgent() && bot.AgentPaired()) {
                ui.ShowAgentDashboard();
            } else {
                ui.ShowEmptyNotes();
            }
            return;
        case DashboardSlot::kPhoto:
            if (const Note* photo = notes.IdlePhoto()) {
                ui.ShowIdlePhoto(*photo);
                return;
            }
            if (bot.UsesCustomAgent() && bot.AgentPaired()) {
                ui.ShowAgentDashboard();
            } else {
                ui.ShowEmptyNotes();
            }
            return;
        case DashboardSlot::kNote:
            if (!notes.Empty()) {
                ui.ShowNotes(notes.All(), notes.CurrentIndex());
                return;
            }
            if (bot.UsesCustomAgent() && bot.AgentPaired()) {
                ui.ShowAgentDashboard();
            } else {
                ui.ShowEmptyNotes();
            }
            return;
    }
}

void AdvanceDashboardCarousel() {
    const UiDashboardView ui_view = ui.DashboardView();
    if (ui_view == UiDashboardView::kNote && !notes.Empty() &&
        ui.NotePage() + 1 < ui.NotePageCount()) {
        if (ui.ShowNextNotePage(notes.All(), notes.CurrentIndex())) {
            std::printf(
                "WEC:{\"scope\":\"ui\",\"stage\":\"carousel_note_page\","
                "\"page\":%u,\"page_count\":%u,\"interval_sec\":%u,"
                "\"ok\":true,\"type\":\"event\"}\n",
                static_cast<unsigned>(ui.NotePage() + 1),
                static_cast<unsigned>(ui.NotePageCount()),
                static_cast<unsigned>(CONFIG_WEC_AUTO_PAGE_SEC));
            std::fflush(stdout);
        }
        return;
    }

    const DashboardSlot from = DashboardSlotFromUi(ui_view);
    const DashboardSlot to = NextDashboardSlot(from);
    ShowDashboardSlot(to);
    std::printf(
        "WEC:{\"scope\":\"ui\",\"stage\":\"carousel_advance\","
        "\"from\":\"%s\",\"to\":\"%s\",\"interval_sec\":%u,"
        "\"ok\":true,\"type\":\"event\"}\n",
        DashboardSlotName(from), DashboardSlotName(to),
        static_cast<unsigned>(CONFIG_WEC_AUTO_PAGE_SEC));
    std::fflush(stdout);
}

void ProcessDashboardTimers() {
    static int64_t last_screensaver_activity = 0;
    static int64_t last_dashboard_turn = 0;
    static UiDashboardView last_dashboard_view = UiDashboardView::kOther;
    static bool last_agent_ready = false;

    const int64_t now = std::time(nullptr);
    const int64_t now_mono = MonotonicSeconds();
    if (last_dashboard_turn == 0) {
        last_dashboard_turn = now_mono;
    }
    const int64_t last = bot.LastActivity();
    const bool agent_ready = bot.AgentMqttConnected();
    if (ui.ScreensaverActive()) {
        const int64_t started = ui.ScreensaverStartedAt();
        if (started > 0 && now >= started + CONFIG_WEC_SCREENSAVER_RETURN_SEC) {
            ui.SetScreensaverActive(false);
            RenderCurrentOrEmpty();
            last_dashboard_turn = now_mono;
            last_dashboard_view = ui.DashboardView();
        }
        last_agent_ready = agent_ready;
    } else if (last > 0 && last != last_screensaver_activity &&
        now > last + CONFIG_WEC_SCREENSAVER_IDLE_SEC) {
        if (notes.Empty() && notes.HasIdlePhoto()) {
            RenderCurrentOrEmpty();
        } else {
            ui.ShowScreensaver(bot.Connected(), notes.Count());
        }
        last_screensaver_activity = last;
        last_dashboard_turn = now_mono;
        last_dashboard_view = ui.DashboardView();
        last_agent_ready = agent_ready;
    } else {
        const UiDashboardView dashboard_view = ui.DashboardView();
        const bool has_dashboard_content = notes.Count() > 0 || notes.HasIdlePhoto();
        if (dashboard_view != last_dashboard_view) {
            last_dashboard_view = dashboard_view;
            last_dashboard_turn = now_mono;
        }
        if (agent_ready && !last_agent_ready &&
            dashboard_view != UiDashboardView::kOther && has_dashboard_content) {
            last_dashboard_turn = now_mono - CONFIG_WEC_AUTO_PAGE_SEC;
            std::printf(
                "WEC:{\"scope\":\"ui\",\"stage\":\"carousel_ready\","
                "\"view\":\"%s\",\"agent_ready\":true,"
                "\"interval_sec\":%u,\"ok\":true,\"type\":\"event\"}\n",
                DashboardViewName(dashboard_view),
                static_cast<unsigned>(CONFIG_WEC_AUTO_PAGE_SEC));
            std::fflush(stdout);
        }
        last_agent_ready = agent_ready;
        if (agent_ready &&
            dashboard_view != UiDashboardView::kOther && has_dashboard_content &&
            now_mono >= last_dashboard_turn + CONFIG_WEC_AUTO_PAGE_SEC) {
            AdvanceDashboardCarousel();
            last_dashboard_view = ui.DashboardView();
            last_dashboard_turn = now_mono;
        }
    }
}

void EnvironmentTask(void*) {
    bool sensor_ready = false;
    bool have_reading = false;
    bool runtime_active = false;
    int tick = 0;
    while (true) {
        ui.SetNetworkStatus(wifi.Connected());
        // USB host presence can change while the board remains on battery.
        // This only repaints the status bar when the state actually changes.
        const bool usb_host_connected = UsbHostPowerConnected();
        ui.SetUsbHostPowerStatus(usb_host_connected);

        const bool wechat_connected = bot.Connected();
        const bool agent_mqtt_connected = bot.AgentMqttConnected();
        if (!wechat_connected && !agent_mqtt_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!runtime_active) {
            runtime_active = true;
            tick = 0;
            std::printf(
                "WEC:{\"scope\":\"task\",\"stage\":\"environment_active\","
                "\"wechat_connected\":%s,\"agent_mqtt_connected\":%s,"
                "\"ok\":true,\"type\":\"event\"}\n",
                wechat_connected ? "true" : "false",
                agent_mqtt_connected ? "true" : "false");
            std::fflush(stdout);
        }

        ProcessDashboardTimers();
        if (tick % 60 == 0) {
            bot.RetryTimeSync();
            const BatteryReading battery_reading = battery.Read();
            if (battery_reading.valid) {
                ui.SetBatteryStatus(battery_reading.present, battery_reading.percent);
                std::printf(
                    "WEC:{\"scope\":\"power\",\"stage\":\"battery_read\","
                    "\"present\":%s,\"percent\":%d,\"voltage\":%.2f,"
                    "\"usb_host_connected\":%s,"
                    "\"ok\":true,\"type\":\"event\"}\n",
                    battery_reading.present ? "true" : "false",
                    battery_reading.percent,
                    battery_reading.voltage,
                    usb_host_connected ? "true" : "false");
            } else {
                std::printf(
                    "WEC:{\"scope\":\"power\",\"stage\":\"battery_read\","
                    "\"ok\":false,\"type\":\"event\"}\n");
            }

            if (!sensor_ready) {
                sensor_ready = shtc3.Init();
            }
            float temperature = 0.0f;
            float humidity = 0.0f;
            bool ok = sensor_ready && shtc3.Read(&temperature, &humidity);
            if (!ok && sensor_ready) {
                vTaskDelay(pdMS_TO_TICKS(120));
                ok = shtc3.Read(&temperature, &humidity);
            }
            if (ok) {
                const float raw_temperature = temperature;
                const float raw_humidity = humidity;
                temperature += static_cast<float>(CONFIG_WEC_TEMPERATURE_OFFSET_DECIC) / 10.0f;
                humidity = std::clamp(
                    humidity + static_cast<float>(CONFIG_WEC_HUMIDITY_OFFSET_PERCENT),
                    0.0f,
                    100.0f);
                have_reading = true;
                ui.SetEnvironment(temperature, humidity, true);
                std::printf(
                    "WEC:{\"scope\":\"sensor\",\"stage\":\"shtc3_read\","
                    "\"temperature_c\":%.1f,\"humidity_percent\":%.1f,"
                    "\"raw_temperature_c\":%.1f,\"raw_humidity_percent\":%.1f,"
                    "\"temperature_offset_decic\":%d,\"humidity_offset_percent\":%d,"
                    "\"ok\":true,\"type\":\"event\"}\n",
                    temperature, humidity, raw_temperature, raw_humidity,
                    CONFIG_WEC_TEMPERATURE_OFFSET_DECIC,
                    CONFIG_WEC_HUMIDITY_OFFSET_PERCENT);
            } else {
                if (!have_reading) {
                    ui.SetEnvironment(0.0f, 0.0f, false);
                } else {
                    ui.Tick();
                }
                std::printf(
                    "WEC:{\"scope\":\"sensor\",\"stage\":\"shtc3_read\","
                    "\"ok\":false,\"type\":\"event\"}\n");
            }
        } else {
            ui.Tick();
        }
        tick = (tick + 1) % 60;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

extern "C" void app_main(void) {
    ConfigureLocalTimezone();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!display.Init()) {
        ESP_LOGE(kTag, "Display init failed");
        return;
    }

    ui.ShowBoot("正在启动");
    notes.Load();
    buttons.Init();
#if CONFIG_WEC_PRODUCT_DISK
    StartUsbProductDisk();
#endif
    serial_config.Start();

    wifi.LoadCredentials();
    if (!wifi.HasConfiguredSsid()) {
        ui.ShowUsbConfig("串口发送 WEC:SET 写入 Wi-Fi");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ui.ShowWifi(wifi.ConfiguredSsid().c_str(), "连接中...");
    if (!wifi.Connect()) {
        std::string message = "Wi-Fi 连接失败";
        if (wifi.LastDisconnectReasonText()[0] != '\0') {
            message += ": ";
            message += wifi.LastDisconnectReasonText();
        }
        ui.ShowUsbConfig(message.c_str());
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ui.ShowWifi(wifi.ConfiguredSsid().c_str(), wifi.IpAddress().c_str());
    ui.SetNetworkStatus(true);
    vTaskDelay(pdMS_TO_TICKS(700));

    StartTask(EnvironmentTask, "environment", 4096, nullptr, 2);
    vTaskDelay(pdMS_TO_TICKS(200));
    StartTask(BotTask, "wechat_bot", 12288, nullptr, 5);
}

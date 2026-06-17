#include <cstdio>
#include <ctime>
#include <string>

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "button_manager.h"
#include "note_store.h"
#include "rlcd_display.h"
#include "serial_config.h"
#include "ui.h"
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

void BotTask(void*) {
    bot.Start();
    vTaskDelete(nullptr);
}

void RenderCurrentOrEmpty() {
    if (!notes.Empty()) {
        ui.ShowNotes(notes.All(), notes.CurrentIndex());
    } else if (const Note* photo = notes.IdlePhoto()) {
        ui.ShowIdlePhoto(*photo);
    } else {
        ui.ShowEmptyNotes();
    }
}

void ScreensaverTask(void*) {
    int64_t last_screensaver_activity = 0;
    int64_t last_note_page_turn = std::time(nullptr);
    while (true) {
        const int64_t now = std::time(nullptr);
        const int64_t last = bot.LastActivity();
        if (ui.ScreensaverActive()) {
            const int64_t started = ui.ScreensaverStartedAt();
            if (started > 0 && now >= started + CONFIG_WEC_SCREENSAVER_RETURN_SEC) {
                ui.SetScreensaverActive(false);
                RenderCurrentOrEmpty();
                last_note_page_turn = now;
            }
        } else if (last > 0 && last != last_screensaver_activity &&
            now > last + CONFIG_WEC_SCREENSAVER_IDLE_SEC) {
            if (notes.Empty() && notes.HasIdlePhoto()) {
                RenderCurrentOrEmpty();
            } else {
                ui.ShowScreensaver(bot.Connected(), notes.Count());
            }
            last_screensaver_activity = last;
            last_note_page_turn = now;
        } else if (ui.NoteViewActive() && notes.Count() > 0 &&
                   now >= last_note_page_turn + CONFIG_WEC_AUTO_PAGE_SEC) {
            if (ui.ShowNextNotePage(notes.All(), notes.CurrentIndex())) {
                std::printf(
                    "WEC:{\"scope\":\"ui\",\"stage\":\"auto_page_turn\","
                    "\"page\":%u,\"page_count\":%u,\"interval_sec\":%u,"
                    "\"ok\":true,\"type\":\"event\"}\n",
                    static_cast<unsigned>(ui.NotePage() + 1),
                    static_cast<unsigned>(ui.NotePageCount()),
                    static_cast<unsigned>(CONFIG_WEC_AUTO_PAGE_SEC));
            }
            last_note_page_turn = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}  // namespace

extern "C" void app_main(void) {
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
    vTaskDelay(pdMS_TO_TICKS(700));

    xTaskCreate(BotTask, "wechat_bot", 12288, nullptr, 5, nullptr);
    xTaskCreate(ScreensaverTask, "screensaver", 4096, nullptr, 2, nullptr);
}

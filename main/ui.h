#pragma once

#include <ctime>
#include <string>
#include <vector>

#include <lvgl.h>

#include "note_store.h"
#include "rlcd_display.h"

class Ui {
public:
    explicit Ui(RlcdDisplay& display) : display_(display) {}

    void ShowBoot(const char* line);
    void ShowWifi(const char* ssid, const char* status);
    void ShowQr(const char* qr_url, int seconds_left);
    void ShowQrStatus(const char* status, int seconds_left);
    void ShowLoginSuccess();
    void ShowUsbConfig(const char* status);
    void ShowIdleHome(const char* status, const char* footer);
    void ShowThinking(const char* status, const char* footer);
    void ShowNotes(const std::vector<Note>& notes, size_t index);
    bool ShowNextNotePage(const std::vector<Note>& notes, size_t index);
    bool ShowPreviousNotePage(const std::vector<Note>& notes, size_t index);
    void ShowEmptyNotes();
    void ShowIdlePhoto(const Note& photo);
    void ShowPlatformPrompt(const char* kind, const char* file_name);
    void ShowScreensaver(bool connected, size_t note_count);
    void ShowError(const char* title, const char* detail);
    void SetEnvironment(float temperature_c, float humidity_percent, bool valid);
    void SetNetworkStatus(bool connected);
    void SetBatteryStatus(bool present, int percent);
    void Tick();

    bool ScreensaverActive() const { return screensaver_active_; }
    bool NoteViewActive() const { return note_view_active_; }
    size_t NotePage() const { return note_page_; }
    size_t NotePageCount() const { return note_page_count_; }
    int64_t ScreensaverStartedAt() const { return screensaver_started_at_; }
    void SetScreensaverActive(bool active) {
        screensaver_active_ = active;
        if (!active) {
            screensaver_started_at_ = 0;
        }
    }

private:
    void ClearLocked();
    void ReleaseQrAssets();
    void ReleaseContentAssets();
    bool RenderNotePageLocked(const Note& note, size_t page, size_t* page_count);
    bool RenderContentBitmapLocked(const Note& note, size_t page);
    bool RenderBitmapLocked(const std::vector<uint8_t>& frame,
                            int width,
                            int height,
                            int stride,
                            int x,
                            int y);
    void ShowCalendarHomeLocked(const char* header_detail = "",
                                const char* footer = "微信发送文字、清单或照片即可上屏",
                                bool thinking = false);
    void UpdateStatusLabelsLocked();
    void UpdateNetworkIndicatorLocked();
    void DrawCalendarLocked(const std::tm& tm);
    void DrawQrPanelLocked(const char* status, int seconds_left);
    void HeaderLocked(const char* left, const char* right);
    void FooterLocked(const char* text);
    lv_obj_t* LabelLocked(lv_obj_t* parent, const char* text, const lv_font_t* font,
                          lv_color_t color, int x, int y, int w);
    std::string NowString(const char* fallback = "--:--") const;
    void UpdateBatteryIndicatorLocked();
    void ClearPlugIndicatorLocked();

    RlcdDisplay& display_;
    bool screensaver_active_ = false;
    bool calendar_home_active_ = false;
    bool note_view_active_ = false;
    bool environment_valid_ = false;
    bool network_connected_ = false;
    bool battery_present_ = false;
    bool battery_status_known_ = false;
    int battery_percent_ = 0;
    bool calendar_time_ready_ = false;
    bool calendar_thinking_ = false;
    bool qr_calendar_active_ = false;
    float temperature_c_ = 0.0f;
    float humidity_percent_ = 0.0f;
    size_t note_page_ = 0;
    size_t note_page_count_ = 1;
    int64_t screensaver_started_at_ = 0;
    std::string header_detail_;
    std::string calendar_footer_;
    std::string qr_status_;
    std::string last_header_text_;
    std::string last_home_time_;
    std::string last_home_date_;
    std::string last_home_sensor_;
    void* red_block_pet_ = nullptr;
    lv_obj_t* header_right_label_ = nullptr;
    lv_obj_t* network_signal_[3] = {};
    lv_obj_t* battery_outline_ = nullptr;
    lv_obj_t* battery_nub_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_mark_label_ = nullptr;
    lv_obj_t* plug_parts_[5] = {};
    lv_obj_t* footer_label_ = nullptr;
    lv_obj_t* home_time_label_ = nullptr;
    lv_obj_t* home_time_shadow_label_ = nullptr;
    lv_obj_t* home_date_label_ = nullptr;
    lv_obj_t* home_sensor_label_ = nullptr;
    lv_obj_t* home_sensor_shadow_label_ = nullptr;
    lv_obj_t* qr_status_label_ = nullptr;
    lv_image_dsc_t* qr_dsc_ = nullptr;
    uint8_t* qr_buf_ = nullptr;
    lv_image_dsc_t* content_dsc_ = nullptr;
    uint8_t* content_buf_ = nullptr;
};

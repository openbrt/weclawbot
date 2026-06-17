#pragma once

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
    void HeaderLocked(const char* left, const char* right);
    void FooterLocked(const char* text);
    lv_obj_t* LabelLocked(lv_obj_t* parent, const char* text, const lv_font_t* font,
                          lv_color_t color, int x, int y, int w);
    std::string NowString(const char* fallback = "--:--") const;

    RlcdDisplay& display_;
    bool screensaver_active_ = false;
    bool note_view_active_ = false;
    size_t note_page_ = 0;
    size_t note_page_count_ = 1;
    int64_t screensaver_started_at_ = 0;
    void* red_block_pet_ = nullptr;
    lv_obj_t* qr_status_label_ = nullptr;
    lv_image_dsc_t* qr_dsc_ = nullptr;
    uint8_t* qr_buf_ = nullptr;
    lv_image_dsc_t* content_dsc_ = nullptr;
    uint8_t* content_buf_ = nullptr;
};

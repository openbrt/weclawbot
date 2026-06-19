#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "app_config.h"
#include "qrcodegen.h"
#include "red_block_pet.h"

extern const lv_font_t font_puhui_14_1;
extern const lv_font_t font_puhui_16_4;
extern const lv_font_t font_puhui_20_4;
extern const lv_font_t font_puhui_30_4;

namespace {
constexpr char kTag[] = "Ui";

lv_color_t Black() {
    return lv_color_black();
}

lv_color_t White() {
    return lv_color_white();
}

lv_obj_t* RectObj(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg,
                  lv_color_t border, int radius, int border_width) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, border_width, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

std::string TrimAscii(std::string value) {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= ' ') {
        value.erase(value.begin());
    }
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= ' ') {
        value.pop_back();
    }
    return value;
}

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool IsSystemSourceLine(const std::string& line) {
    return line == "来自微信" || line == "来自微信语音" ||
           StartsWith(line, "来自 ") || StartsWith(line, "from ");
}

bool IsGenericTitle(const std::string& line) {
    return line == "记事" || line == "备忘" || line == "提醒" ||
           line == "留言" || line == "微笺" || line == "今日提醒";
}

std::vector<std::string> NoteLines(const std::string& value) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : value) {
        if (c == '\n' || c == '\r') {
            std::string line = TrimAscii(current);
            if (!line.empty() && !IsSystemSourceLine(line)) {
                lines.push_back(line);
            }
            current.clear();
        } else if (c == '\t') {
            current.push_back(' ');
        } else {
            current.push_back(c);
        }
    }
    std::string line = TrimAscii(current);
    if (!line.empty() && !IsSystemSourceLine(line)) {
        lines.push_back(line);
    }
    if (!lines.empty() && IsGenericTitle(lines.front())) {
        lines.erase(lines.begin());
    }
    return lines;
}

std::string ClockString() {
    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) {
        return "--:--";
    }
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

const char* WeekdayName(int weekday) {
    static constexpr const char* kNames[] = {"周日", "周一", "周二", "周三",
                                             "周四", "周五", "周六"};
    if (weekday < 0 || weekday > 6) {
        return "";
    }
    return kNames[weekday];
}

std::string DateString() {
    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) {
        return "正在校时";
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d.%02d.%02d %s",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  WeekdayName(tm.tm_wday));
    return buf;
}

std::string QrFooterText(const char* status, int seconds_left) {
    const char* label = (status && status[0]) ? status : "微信扫码连接";
    if (std::strcmp(label, "等待扫码") == 0) {
        label = "微信扫码连接";
    }
    char footer[96];
    std::snprintf(footer, sizeof(footer), "%s  有效期 %d:%02d",
                  label, seconds_left / 60, seconds_left % 60);
    return footer;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

size_t Utf8CharBytes(const std::string& value, size_t offset) {
    if (offset >= value.size()) {
        return 0;
    }
    const auto c = static_cast<unsigned char>(value[offset]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int VisualUnits(const std::string& value, size_t offset) {
    if (offset >= value.size()) {
        return 0;
    }
    const auto c = static_cast<unsigned char>(value[offset]);
    if (c < 0x80) {
        if (c <= ' ') {
            return 6;
        }
        switch (c) {
            case '.':
            case ',':
            case ':':
            case ';':
            case '!':
            case '|':
            case '\'':
            case '"':
            case '`':
                return 8;
            case 'i':
            case 'l':
            case 'I':
            case '1':
                return 9;
            case 'm':
            case 'w':
            case 'M':
            case 'W':
            case '@':
            case '#':
                return 18;
            default:
                return 14;
        }
    }
    return 28;
}

int VisualLineUnits(const std::string& value) {
    int units = 0;
    for (size_t i = 0; i < value.size();) {
        const size_t n = Utf8CharBytes(value, i);
        if (n == 0) {
            break;
        }
        units += VisualUnits(value, i);
        i += n;
    }
    return units;
}

std::vector<std::string> WrappedVisualLines(const Note& note) {
    constexpr int kMaxUnitsPerLine = 600;
    std::vector<std::string> out;
    std::vector<std::string> source = NoteLines(note.text);
    if (source.empty()) {
        source.push_back("空白微笺");
    }

    for (const auto& raw : source) {
        std::string current;
        int units = 0;
        for (size_t i = 0; i < raw.size();) {
            const size_t n = Utf8CharBytes(raw, i);
            if (n == 0) {
                break;
            }
            const int char_units = VisualUnits(raw, i);
            if (!current.empty() && units + char_units > kMaxUnitsPerLine) {
                out.push_back(current);
                current.clear();
                units = 0;
            }
            current.append(raw, i, n);
            units += char_units;
            i += n;
        }
        if (!current.empty()) {
            out.push_back(current);
        }
    }
    return out;
}

std::vector<std::vector<std::string>> NotePages(const Note& note) {
    constexpr size_t kLinesPerPage = 10;
    std::vector<std::vector<std::string>> pages;
    std::vector<std::string> lines = WrappedVisualLines(note);
    for (size_t i = 0; i < lines.size(); i += kLinesPerPage) {
        std::vector<std::string> page;
        const size_t end = std::min(lines.size(), i + kLinesPerPage);
        for (size_t j = i; j < end; ++j) {
            page.push_back(lines[j]);
        }
        pages.push_back(std::move(page));
    }
    if (pages.empty()) {
        pages.push_back({"空白微笺"});
    }
    return pages;
}

size_t RenderedPageCount(const Note& note) {
    return note.screen_frames.empty() ? NotePages(note).size() : note.screen_frames.size();
}

bool AllLinesFit(const std::vector<std::string>& lines, int max_units) {
    for (const auto& line : lines) {
        if (VisualLineUnits(line) > max_units) {
            return false;
        }
    }
    return true;
}
}  // namespace

void Ui::ShowBoot(const char* line) {
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "启动中");

    auto* title = LabelLocked(lv_screen_active(), "自动进化的微笺屏",
                              &font_puhui_30_4, Black(), 34, 82, 332);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    auto* subtitle = LabelLocked(lv_screen_active(), line, &font_puhui_20_4, Black(), 38, 146, 324);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    FooterLocked("平常只显示当前一条微笺");
    display_.Unlock();
}

void Ui::ShowWifi(const char* ssid, const char* status) {
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "Wi-Fi");

    LabelLocked(lv_screen_active(), "连接无线网络", &font_puhui_30_4, Black(), 36, 74, 328);

    char line[160];
    std::snprintf(line, sizeof(line), "SSID: %s", ssid && ssid[0] ? ssid : "未配置");
    LabelLocked(lv_screen_active(), line, &font_puhui_20_4, Black(), 36, 134, 328);
    LabelLocked(lv_screen_active(), status, &font_puhui_20_4, Black(), 36, 174, 328);

    FooterLocked("忘记入口：weclawbot.link");
    display_.Unlock();
}

void Ui::ShowUsbConfig(const char* status) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "USB 配置");

    auto* title = LabelLocked(lv_screen_active(), "需要配置 Wi-Fi", &font_puhui_30_4, Black(), 34, 66, 332);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    auto* line1 = LabelLocked(lv_screen_active(), "USB-C 连接电脑", &font_puhui_20_4, Black(), 46, 132, 308);
    lv_obj_set_style_text_align(line1, LV_TEXT_ALIGN_CENTER, 0);

    auto* line2 = LabelLocked(lv_screen_active(), WEC_PRODUCT_HOST, &font_puhui_20_4, Black(), 34, 172, 332);
    lv_obj_set_style_text_align(line2, LV_TEXT_ALIGN_CENTER, 0);

    auto* line3 = LabelLocked(lv_screen_active(), status, &font_puhui_14_1, Black(), 28, 214, 344);
    lv_obj_set_style_text_align(line3, LV_TEXT_ALIGN_CENTER, 0);

    FooterLocked("打开 U 盘或官网进入安装/配置");
    display_.Unlock();
}

void Ui::ShowQr(const char* qr_url, int seconds_left) {
    uint8_t* qr = static_cast<uint8_t*>(heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT));
    uint8_t* tmp = static_cast<uint8_t*>(heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT));
    if (!qr || !tmp) {
        if (qr) heap_caps_free(qr);
        if (tmp) heap_caps_free(tmp);
        ShowError("二维码", "内存不足，无法生成二维码");
        return;
    }

    const bool ok = qrcodegen_encodeText(qr_url, tmp, qr, qrcodegen_Ecc_LOW,
                                         qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                         qrcodegen_Mask_AUTO, true);
    heap_caps_free(tmp);
    if (!ok) {
        heap_caps_free(qr);
        ShowError("二维码", "二维码内容过长");
        return;
    }

    const int qr_size = qrcodegen_getSize(qr);
    const int border = 2;
    const int total = qr_size + border * 2;
    int scale = 132 / total;
    if (scale < 1) scale = 1;
    const int image_wh = total * scale;
    const uint32_t stride = lv_draw_buf_width_to_stride(image_wh, LV_COLOR_FORMAT_RGB565);
    const size_t buf_size = stride * image_wh;

    auto* pixel_buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(4, buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixel_buf) {
        pixel_buf = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(4, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    auto* dsc = static_cast<lv_image_dsc_t*>(
        heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_8BIT));
    if (!pixel_buf || !dsc) {
        if (pixel_buf) heap_caps_free(pixel_buf);
        if (dsc) heap_caps_free(dsc);
        heap_caps_free(qr);
        ShowError("二维码", "内存不足，无法绘制二维码");
        return;
    }

    std::fill(pixel_buf, pixel_buf + buf_size, 0xFF);
    const int offset = border * scale;
    for (int row = 0; row < qr_size; ++row) {
        for (int col = 0; col < qr_size; ++col) {
            if (!qrcodegen_getModule(qr, col, row)) {
                continue;
            }
            const int y0 = offset + row * scale;
            const int x0 = offset + col * scale;
            for (int dy = 0; dy < scale; ++dy) {
                auto* line = reinterpret_cast<uint16_t*>(pixel_buf + (y0 + dy) * stride) + x0;
                for (int dx = 0; dx < scale; ++dx) {
                    line[dx] = 0x0000;
                }
            }
        }
    }
    heap_caps_free(qr);

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = image_wh;
    dsc->header.h = image_wh;
    dsc->header.stride = stride;
    dsc->data_size = buf_size;
    dsc->data = pixel_buf;

    display_.Lock();
    ClearLocked();
    ReleaseQrAssets();
    qr_buf_ = pixel_buf;
    qr_dsc_ = dsc;
    calendar_home_active_ = true;
    note_view_active_ = false;
    qr_calendar_active_ = true;
    qr_status_ = "等待扫码";
    calendar_footer_ = QrFooterText("微信扫码连接", seconds_left);

    HeaderLocked("微笺屏", "");

    home_time_shadow_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_30_4,
                                          Black(), 27, 42, 160);
    home_time_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_30_4,
                                   Black(), 26, 42, 160);
    lv_obj_set_style_text_align(home_time_shadow_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(home_time_label_, LV_TEXT_ALIGN_CENTER, 0);

    home_date_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_16_4,
                                   Black(), 16, 86, 176);
    lv_obj_add_flag(home_date_label_, LV_OBJ_FLAG_HIDDEN);

    home_sensor_shadow_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_20_4,
                                            Black(), 21, 83, 176);
    home_sensor_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_20_4,
                                     Black(), 20, 83, 176);
    lv_obj_set_style_text_align(home_sensor_shadow_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(home_sensor_label_, LV_TEXT_ALIGN_CENTER, 0);

    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    calendar_time_ready_ = tm.tm_year >= 120;

    DrawCalendarLocked(tm);
    DrawQrPanelLocked("微信扫码连接", seconds_left);
    FooterLocked(calendar_footer_.c_str());
    UpdateStatusLabelsLocked();
    display_.Unlock();
}

void Ui::ShowQrStatus(const char* status, int seconds_left) {
    display_.Lock();
    qr_status_ = status ? status : "";
    char caption[96];
    std::snprintf(caption, sizeof(caption), "%s  有效期 %d:%02d",
                  status ? status : "等待扫码",
                  seconds_left / 60, seconds_left % 60);
    if (qr_status_label_) {
        lv_label_set_text(qr_status_label_, caption);
    }
    if (qr_calendar_active_ && footer_label_) {
        calendar_footer_ = QrFooterText(status, seconds_left);
        lv_label_set_text(footer_label_, calendar_footer_.c_str());
    }
    display_.Unlock();
}

void Ui::ShowLoginSuccess() {
    ShowIdleHome("微信已连接", "发送微信文本，它会变成当前微笺");
}

void Ui::ShowIdleHome(const char* status, const char* footer) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "微笺");

    auto* title = LabelLocked(lv_screen_active(), status, &font_puhui_30_4, Black(), 38, 74, 324);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    auto* line = LabelLocked(lv_screen_active(), "等待微信文本上屏",
                             &font_puhui_20_4, Black(), 38, 142, 324);
    lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);

    FooterLocked(footer ? footer : "微信端确认、修改或清除");
    display_.Unlock();
}

void Ui::ShowThinking(const char* status, const char* footer) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    ShowCalendarHomeLocked("处理中",
                           footer ? footer : (status ? status : "正在整理微信内容"),
                           true);
    display_.Unlock();
}

void Ui::ShowNotes(const std::vector<Note>& notes, size_t index) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();

    if (notes.empty()) {
        display_.Unlock();
        ShowEmptyNotes();
        return;
    }
    if (index >= notes.size()) {
        index = 0;
    }

    note_page_ = 0;
    RenderNotePageLocked(notes[index], note_page_, nullptr);
    display_.Unlock();
}

bool Ui::ShowNextNotePage(const std::vector<Note>& notes, size_t index) {
    if (notes.empty()) {
        return false;
    }
    if (index >= notes.size()) {
        index = 0;
    }
    if (RenderedPageCount(notes[index]) <= 1) {
        return false;
    }

    display_.Lock();
    ClearLocked();
    const size_t requested_page = note_page_ + 1;
    size_t page_count = 1;
    RenderNotePageLocked(notes[index], requested_page, &page_count);
    display_.Unlock();
    return page_count > 1;
}

bool Ui::ShowPreviousNotePage(const std::vector<Note>& notes, size_t index) {
    if (notes.empty()) {
        return false;
    }
    if (index >= notes.size()) {
        index = 0;
    }
    if (RenderedPageCount(notes[index]) <= 1) {
        return false;
    }

    const size_t requested_page = note_page_ == 0 ? note_page_count_ - 1 : note_page_ - 1;
    display_.Lock();
    ClearLocked();
    size_t page_count = 1;
    RenderNotePageLocked(notes[index], requested_page, &page_count);
    display_.Unlock();
    return page_count > 1;
}

void Ui::ShowEmptyNotes() {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    ShowCalendarHomeLocked();
    display_.Unlock();
}

void Ui::ShowIdlePhoto(const Note& photo) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    if (!photo.screen_frames.empty() &&
        photo.screen_frame_width > 0 && photo.screen_frame_width <= WEC_RLCD_WIDTH &&
        photo.screen_frame_height > 0 && photo.screen_frame_height <= WEC_RLCD_HEIGHT &&
        photo.screen_frame_stride >= (photo.screen_frame_width + 7) / 8) {
        const int x = (WEC_RLCD_WIDTH - photo.screen_frame_width) / 2;
        const int y = (WEC_RLCD_HEIGHT - photo.screen_frame_height) / 2;
        if (RenderBitmapLocked(photo.screen_frames[0], photo.screen_frame_width,
                               photo.screen_frame_height, photo.screen_frame_stride,
                               x, y)) {
            display_.Unlock();
            return;
        }
    }
    red_block_pet_ = RedBlockPetCreate(lv_screen_active());
    RedBlockPetSetState(red_block_pet_, RedBlockPetState::kIdle);
    display_.Unlock();
}

void Ui::ShowPlatformPrompt(const char* kind, const char* file_name) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "平台服务");

    LabelLocked(lv_screen_active(), "已收到非文本消息", &font_puhui_30_4, Black(), 34, 62, 332);

    char detail[180];
    std::snprintf(detail, sizeof(detail), "%s%s%s",
                  kind ? kind : "文件",
                  file_name && file_name[0] ? ": " : "",
                  file_name && file_name[0] ? file_name : "");
    LabelLocked(lv_screen_active(), detail, &font_puhui_20_4, Black(), 34, 126, 332);

    LabelLocked(lv_screen_active(), CONFIG_WEC_PLATFORM_HINT, &font_puhui_20_4, Black(), 34, 170, 332);
    FooterLocked("当前开源固件免费支持文本微笺");
    display_.Unlock();
}

void Ui::ShowScreensaver(bool connected, size_t note_count) {
    (void)connected;
    (void)note_count;
    screensaver_active_ = true;
    screensaver_started_at_ = std::time(nullptr);
    display_.Lock();
    ClearLocked();
    red_block_pet_ = RedBlockPetCreate(lv_screen_active());
    display_.Unlock();
}

void Ui::ShowError(const char* title, const char* detail) {
    screensaver_active_ = false;
    display_.Lock();
    ClearLocked();
    HeaderLocked("WeClawBot", "错误");
    LabelLocked(lv_screen_active(), title, &font_puhui_30_4, Black(), 34, 72, 332);
    LabelLocked(lv_screen_active(), detail, &font_puhui_20_4, Black(), 34, 132, 332);
    FooterLocked("请查看串口日志");
    display_.Unlock();
}

void Ui::SetEnvironment(float temperature_c, float humidity_percent, bool valid) {
    display_.Lock();
    environment_valid_ = valid;
    if (valid) {
        temperature_c_ = temperature_c;
        humidity_percent_ = humidity_percent;
    }
    UpdateStatusLabelsLocked();
    display_.Unlock();
}

void Ui::SetNetworkStatus(bool connected) {
    display_.Lock();
    if (network_connected_ != connected) {
        network_connected_ = connected;
        UpdateNetworkIndicatorLocked();
    }
    display_.Unlock();
}

void Ui::SetBatteryStatus(bool present, int percent) {
    display_.Lock();
    battery_status_known_ = true;
    battery_present_ = present;
    battery_percent_ = std::max(0, std::min(100, percent));
    UpdateBatteryIndicatorLocked();
    display_.Unlock();
}

void Ui::Tick() {
    display_.Lock();
    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    const bool time_ready = tm.tm_year >= 120;
    if (calendar_home_active_ && !qr_calendar_active_ && time_ready != calendar_time_ready_) {
        const std::string header_detail = header_detail_;
        const std::string footer = calendar_footer_;
        const bool thinking = calendar_thinking_;
        ClearLocked();
        ShowCalendarHomeLocked(header_detail.c_str(), footer.c_str(), thinking);
    } else {
        calendar_time_ready_ = time_ready;
        UpdateStatusLabelsLocked();
    }
    display_.Unlock();
}

bool Ui::RenderNotePageLocked(const Note& note, size_t page, size_t* page_count) {
    if (!note.screen_frames.empty() && note.screen_frame_width > 0 &&
        note.screen_frame_height > 0 && note.screen_frame_stride > 0) {
        const size_t count = note.screen_frames.size();
        if (page_count) {
            *page_count = count;
        }
        page %= count;
        note_view_active_ = true;
        note_page_ = page;
        note_page_count_ = count;
        if (RenderContentBitmapLocked(note, page)) {
            return true;
        }
        ESP_LOGW(kTag, "content bitmap rejected, falling back to text layout");
    }

    std::vector<std::vector<std::string>> pages = NotePages(note);
    const size_t count = pages.empty() ? 1 : pages.size();
    if (page_count) {
        *page_count = count;
    }
    if (count == 0) {
        return false;
    }
    page %= count;
    note_view_active_ = true;
    note_page_ = page;
    note_page_count_ = count;

    char page_label[40];
    if (count > 1) {
        std::snprintf(page_label, sizeof(page_label), "%u/%u", static_cast<unsigned>(page + 1),
                      static_cast<unsigned>(count));
    } else {
        std::snprintf(page_label, sizeof(page_label), "%s", ClockString().c_str());
    }
    HeaderLocked("微笺屏", page_label);

    const auto& lines = pages[page];
    if (count == 1 && lines.size() == 1 && VisualLineUnits(lines.front()) <= 290) {
        auto* label = LabelLocked(lv_screen_active(), lines.front().c_str(), &font_puhui_30_4,
                                  Black(), 30, 110, 340);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    } else if (count == 1 && lines.size() <= 3 && AllLinesFit(lines, 290)) {
        const int y0 = lines.size() == 1 ? 112 : (lines.size() == 2 ? 92 : 76);
        for (size_t i = 0; i < lines.size(); ++i) {
            auto* label = LabelLocked(lv_screen_active(), lines[i].c_str(), &font_puhui_30_4,
                                      Black(), 28, y0 + static_cast<int>(i) * 46, 344);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        }
    } else if (lines.size() <= 6 && AllLinesFit(lines, 380)) {
        const int y0 = lines.size() <= 4 ? 62 : 54;
        constexpr int kLineH = 31;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto* label = LabelLocked(lv_screen_active(), lines[i].c_str(), &font_puhui_20_4,
                                      Black(), 28, y0 + static_cast<int>(i) * kLineH, 344);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        }
    } else {
        constexpr int kY0 = 48;
        constexpr int kLineH = 21;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto* label = LabelLocked(lv_screen_active(), lines[i].c_str(), &font_puhui_14_1,
                                      Black(), 20, kY0 + static_cast<int>(i) * kLineH, 360);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        }
    }

    char footer[96];
    if (!note.time_label.empty()) {
        std::snprintf(footer, sizeof(footer), "%s  微信端可修改或替换", note.time_label.c_str());
    } else {
        std::snprintf(footer, sizeof(footer), "微信端可修改、清除或替换");
    }
    FooterLocked(footer);
    return true;
}

bool Ui::RenderContentBitmapLocked(const Note& note, size_t page) {
    if (page >= note.screen_frames.size() ||
        note.screen_frame_width <= 0 || note.screen_frame_width > WEC_CONTENT_BITMAP_WIDTH ||
        note.screen_frame_height <= 0 || note.screen_frame_height > WEC_CONTENT_BITMAP_HEIGHT ||
        note.screen_frame_stride < (note.screen_frame_width + 7) / 8) {
        return false;
    }

    const auto& frame = note.screen_frames[page];
    const size_t source_needed = static_cast<size_t>(note.screen_frame_stride) *
                                 static_cast<size_t>(note.screen_frame_height);
    if (frame.size() < source_needed) {
        return false;
    }

    char page_label[40];
    if (note_page_count_ > 1) {
        std::snprintf(page_label, sizeof(page_label), "%u/%u", static_cast<unsigned>(page + 1),
                      static_cast<unsigned>(note_page_count_));
    } else {
        std::snprintf(page_label, sizeof(page_label), "%s", ClockString().c_str());
    }

    HeaderLocked("微笺屏", page_label);
    const int x = WEC_CONTENT_BITMAP_X +
                  (WEC_CONTENT_BITMAP_WIDTH - note.screen_frame_width) / 2;
    const int y = WEC_CONTENT_BITMAP_Y +
                  (WEC_CONTENT_BITMAP_HEIGHT - note.screen_frame_height) / 2;
    if (!RenderBitmapLocked(frame, note.screen_frame_width, note.screen_frame_height,
                            note.screen_frame_stride, x, y)) {
        return false;
    }

    char footer[96];
    if (!note.time_label.empty()) {
        std::snprintf(footer, sizeof(footer), "%s  微信端可修改或替换", note.time_label.c_str());
    } else {
        std::snprintf(footer, sizeof(footer), "微信端可修改、清除或替换");
    }
    FooterLocked(footer);
    return true;
}

bool Ui::RenderBitmapLocked(const std::vector<uint8_t>& frame,
                            int width,
                            int height,
                            int stride,
                            int x,
                            int y) {
    if (width <= 0 || width > WEC_RLCD_WIDTH || height <= 0 || height > WEC_RLCD_HEIGHT ||
        stride < (width + 7) / 8) {
        return false;
    }
    const size_t source_needed = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (frame.size() < source_needed) {
        return false;
    }

    const uint32_t lv_stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_I1);
    const size_t pixel_bytes = static_cast<size_t>(lv_stride) * static_cast<size_t>(height);
    const size_t data_size = 8 + pixel_bytes;

    uint8_t* pixel_buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(4, data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixel_buf) {
        pixel_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            4, data_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!pixel_buf) {
        ESP_LOGW(kTag, "bitmap alloc failed: %u bytes", static_cast<unsigned>(data_size));
        return false;
    }

    std::memset(pixel_buf, 0, data_size);
    pixel_buf[0] = 0xff;
    pixel_buf[1] = 0xff;
    pixel_buf[2] = 0xff;
    pixel_buf[3] = 0xff;
    pixel_buf[4] = 0x00;
    pixel_buf[5] = 0x00;
    pixel_buf[6] = 0x00;
    pixel_buf[7] = 0xff;

    uint8_t* dest = pixel_buf + 8;
    const size_t copy_stride = std::min(static_cast<size_t>(stride),
                                        static_cast<size_t>(lv_stride));
    for (int row = 0; row < height; ++row) {
        std::memcpy(dest + static_cast<size_t>(row) * lv_stride,
                    frame.data() + static_cast<size_t>(row) * stride,
                    copy_stride);
    }

    lv_image_dsc_t* dsc = static_cast<lv_image_dsc_t*>(
        heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dsc) {
        dsc = static_cast<lv_image_dsc_t*>(
            heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!dsc) {
        heap_caps_free(pixel_buf);
        ESP_LOGW(kTag, "bitmap image descriptor alloc failed");
        return false;
    }

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_I1;
    dsc->header.flags = 0;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.stride = lv_stride;
    dsc->data_size = data_size;
    dsc->data = pixel_buf;

    lv_obj_t* img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);

    content_buf_ = pixel_buf;
    content_dsc_ = dsc;
    return true;
}

void Ui::ShowCalendarHomeLocked(const char* header_detail, const char* footer, bool thinking) {
    calendar_home_active_ = true;
    note_view_active_ = false;
    calendar_thinking_ = thinking;
    calendar_footer_ = footer ? footer : "微信发送文字、清单或照片即可上屏";

    HeaderLocked("微笺屏", header_detail ? header_detail : "日历");

    home_time_shadow_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_30_4,
                                          Black(), 27, 42, 160);
    home_time_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_30_4,
                                   Black(), 26, 42, 160);
    lv_obj_set_style_text_align(home_time_shadow_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(home_time_label_, LV_TEXT_ALIGN_CENTER, 0);

    home_date_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_16_4,
                                   Black(), 16, 86, 176);
    lv_obj_add_flag(home_date_label_, LV_OBJ_FLAG_HIDDEN);

    home_sensor_shadow_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_20_4,
                                            Black(), 21, 86, 176);
    home_sensor_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_20_4,
                                     Black(), 20, 86, 176);
    lv_obj_set_style_text_align(home_sensor_shadow_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(home_sensor_label_, LV_TEXT_ALIGN_CENTER, 0);

    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    calendar_time_ready_ = tm.tm_year >= 120;
    DrawCalendarLocked(tm);

    red_block_pet_ = RedBlockPetCreate(lv_screen_active());
    RedBlockPetSetPosition(red_block_pet_, 96, 138);
    RedBlockPetSetState(red_block_pet_,
                        thinking ? RedBlockPetState::kThinking : RedBlockPetState::kIdle);

    FooterLocked(calendar_footer_.c_str());
    UpdateStatusLabelsLocked();
}

void Ui::UpdateStatusLabelsLocked() {
    char sensor[40] = "";
    if (environment_valid_) {
        std::snprintf(sensor, sizeof(sensor), "%.0f℃ %.0f%%", temperature_c_, humidity_percent_);
    }

    const std::string clock = ClockString();
    std::string header = header_detail_;
    if (header_right_label_ && header != last_header_text_) {
        lv_label_set_text(header_right_label_, header.c_str());
        last_header_text_ = header;
    }
    UpdateNetworkIndicatorLocked();

    if (!calendar_home_active_) {
        return;
    }

    if (home_time_label_ && clock != last_home_time_) {
        lv_label_set_text(home_time_label_, clock.c_str());
        if (home_time_shadow_label_) {
            lv_label_set_text(home_time_shadow_label_, clock.c_str());
        }
        last_home_time_ = clock;
    }

    std::string home_sensor;
    if (environment_valid_) {
        char text[80];
        std::snprintf(text, sizeof(text), "室内温度 %.0f℃\n室内湿度 %.0f%%",
                      temperature_c_, humidity_percent_);
        home_sensor = text;
    } else {
        home_sensor = "室内温度 --℃\n室内湿度 --%";
    }
    if (home_sensor_label_ && home_sensor != last_home_sensor_) {
        lv_label_set_text(home_sensor_label_, home_sensor.c_str());
        if (home_sensor_shadow_label_) {
            lv_label_set_text(home_sensor_shadow_label_, home_sensor.c_str());
        }
        last_home_sensor_ = home_sensor;
    }
}

void Ui::UpdateNetworkIndicatorLocked() {
    for (auto* bar : network_signal_) {
        if (!bar) {
            continue;
        }
        lv_obj_set_style_bg_color(bar, network_connected_ ? Black() : White(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
}

void Ui::UpdateBatteryIndicatorLocked() {
    if (!battery_outline_ || !battery_nub_) {
        return;
    }

    if (battery_fill_) {
        lv_obj_delete(battery_fill_);
        battery_fill_ = nullptr;
    }
    if (battery_mark_label_) {
        lv_obj_delete(battery_mark_label_);
        battery_mark_label_ = nullptr;
    }
    ClearPlugIndicatorLocked();

    if (!battery_status_known_ || !battery_present_) {
        lv_obj_add_flag(battery_outline_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_nub_, LV_OBJ_FLAG_HIDDEN);
        plug_parts_[0] = RectObj(lv_screen_active(), 362, 18, 10, 8, White(), Black(), 0, 2);
        plug_parts_[1] = RectObj(lv_screen_active(), 364, 14, 2, 5, Black(), Black(), 0, 0);
        plug_parts_[2] = RectObj(lv_screen_active(), 369, 14, 2, 5, Black(), Black(), 0, 0);
        plug_parts_[3] = RectObj(lv_screen_active(), 372, 21, 9, 2, Black(), Black(), 0, 0);
        plug_parts_[4] = RectObj(lv_screen_active(), 380, 17, 2, 10, Black(), Black(), 0, 0);
        return;
    }

    lv_obj_clear_flag(battery_outline_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_nub_, LV_OBJ_FLAG_HIDDEN);
    const int fill_w = std::max(2, (battery_percent_ * 14) / 100);
    battery_fill_ = RectObj(lv_screen_active(), 364, 18, fill_w, 6,
                            Black(), Black(), 0, 0);
}

void Ui::ClearPlugIndicatorLocked() {
    for (auto& part : plug_parts_) {
        if (part) {
            lv_obj_delete(part);
            part = nullptr;
        }
    }
}

void Ui::DrawCalendarLocked(const std::tm& now_tm) {
    const int year = now_tm.tm_year + 1900;
    const int month = now_tm.tm_mon + 1;
    const int today = now_tm.tm_mday;
    const bool time_ready = now_tm.tm_year >= 120;

    const int panel_x = 196;
    const int panel_y = 46;
    const int panel_w = 190;
    const int panel_h = 196;
    RectObj(lv_screen_active(), panel_x, panel_y, panel_w, panel_h,
            White(), Black(), 0, 2);

    char year_label[24];
    if (time_ready) {
        std::snprintf(year_label, sizeof(year_label), "%d 年", year);
    } else {
        std::snprintf(year_label, sizeof(year_label), "---- 年");
    }
    LabelLocked(lv_screen_active(), year_label, &font_puhui_20_4, Black(),
                panel_x + 10, panel_y + 4, 96);

    char month_label[24];
    if (time_ready) {
        std::snprintf(month_label, sizeof(month_label), "%d月", month);
    } else {
        std::snprintf(month_label, sizeof(month_label), "--月");
    }
    auto* month_obj = LabelLocked(lv_screen_active(), month_label, &font_puhui_16_4,
                                  Black(), panel_x + 126, panel_y + 7, 48);
    lv_obj_set_style_text_align(month_obj, LV_TEXT_ALIGN_RIGHT, 0);

    if (!time_ready) {
        const char* state = network_connected_ ? "正在校时" : "等待网络";
        const char* detail = network_connected_
                                 ? "联网后会自动显示日期"
                                 : "连接 Wi-Fi 后自动校时";
        auto* state_label = LabelLocked(lv_screen_active(), state, &font_puhui_20_4,
                                        Black(), panel_x + 12, panel_y + 76, panel_w - 24);
        lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
        auto* detail_label = LabelLocked(lv_screen_active(), detail, &font_puhui_14_1,
                                         Black(), panel_x + 12, panel_y + 116, panel_w - 24);
        lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    std::tm first = now_tm;
    first.tm_mday = 1;
    first.tm_hour = 12;
    first.tm_min = 0;
    first.tm_sec = 0;
    std::mktime(&first);
    const int first_col = (first.tm_wday + 6) % 7;
    const int days = DaysInMonth(year, month);
    const int week_rows = std::max(5, (first_col + days + 6) / 7);

    const int grid_x = panel_x + 4;
    const int grid_y = panel_y + 36;
    const int cell_w = 26;
    const int head_h = 18;
    const int cell_h = week_rows <= 5 ? 24 : 20;
    const int grid_w = cell_w * 7;
    const int grid_h = head_h + cell_h * week_rows;

    RectObj(lv_screen_active(), grid_x, grid_y, grid_w, head_h, Black(), Black(), 0, 0);

    for (int col = 0; col <= 7; ++col) {
        lv_obj_t* line = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, grid_x + col * cell_w, grid_y);
        lv_obj_set_size(line, 1, grid_h);
        lv_obj_set_style_bg_color(line, Black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    }
    for (int row = 0; row <= week_rows + 1; ++row) {
        lv_obj_t* line = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(line);
        const int y = row == 0 ? grid_y : grid_y + head_h + (row - 1) * cell_h;
        lv_obj_set_pos(line, grid_x, y);
        lv_obj_set_size(line, grid_w, 1);
        lv_obj_set_style_bg_color(line, Black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    }

    static constexpr const char* kWeek[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int col = 0; col < 7; ++col) {
        auto* label = LabelLocked(lv_screen_active(), kWeek[col], &font_puhui_14_1,
                                  White(), grid_x + col * cell_w, grid_y + 1, cell_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

    for (int day = 1; day <= days; ++day) {
        const int cell = first_col + day - 1;
        const int row = cell / 7;
        const int col = cell % 7;
        const int x = grid_x + col * cell_w;
        const int y = grid_y + head_h + row * cell_h;
        const bool is_today = day == today;
        if (is_today) {
            RectObj(lv_screen_active(), x + 1, y + 1, cell_w - 2, cell_h - 2,
                    Black(), Black(), 0, 0);
        }
        char day_text[4];
        std::snprintf(day_text, sizeof(day_text), "%d", day);
        auto* label = LabelLocked(lv_screen_active(), day_text, &font_puhui_16_4,
                                  is_today ? White() : Black(), x, y + 1, cell_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void Ui::DrawQrPanelLocked(const char* status, int seconds_left) {
    constexpr int panel_x = 16;
    constexpr int panel_y = 136;
    constexpr int panel_w = 176;
    constexpr int panel_h = 112;

    if (qr_dsc_) {
        auto* img = lv_image_create(lv_screen_active());
        lv_image_set_src(img, qr_dsc_);
        const int qr_w = static_cast<int>(qr_dsc_->header.w);
        const int qr_h = static_cast<int>(qr_dsc_->header.h);
        const int x = panel_x + (panel_w - qr_w) / 2;
        const int y = panel_y + (panel_h - qr_h) / 2;
        lv_obj_set_pos(img, x, y);
    }

    (void)status;
    (void)seconds_left;
}

void Ui::ClearLocked() {
    note_view_active_ = false;
    calendar_home_active_ = false;
    if (red_block_pet_) {
        RedBlockPetDestroy(red_block_pet_);
        red_block_pet_ = nullptr;
    }
    lv_obj_clean(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), White(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    header_right_label_ = nullptr;
    footer_label_ = nullptr;
    home_time_label_ = nullptr;
    home_time_shadow_label_ = nullptr;
    home_date_label_ = nullptr;
    home_sensor_label_ = nullptr;
    home_sensor_shadow_label_ = nullptr;
    for (auto& bar : network_signal_) {
        bar = nullptr;
    }
    battery_outline_ = nullptr;
    battery_nub_ = nullptr;
    battery_fill_ = nullptr;
    battery_mark_label_ = nullptr;
    for (auto& part : plug_parts_) {
        part = nullptr;
    }
    qr_status_label_ = nullptr;
    header_detail_.clear();
    calendar_footer_.clear();
    calendar_time_ready_ = false;
    calendar_thinking_ = false;
    qr_calendar_active_ = false;
    qr_status_.clear();
    last_header_text_.clear();
    last_home_time_.clear();
    last_home_date_.clear();
    last_home_sensor_.clear();
    ReleaseQrAssets();
    ReleaseContentAssets();
}

void Ui::ReleaseQrAssets() {
    if (qr_dsc_) {
        heap_caps_free(qr_dsc_);
        qr_dsc_ = nullptr;
    }
    if (qr_buf_) {
        heap_caps_free(qr_buf_);
        qr_buf_ = nullptr;
    }
}

void Ui::ReleaseContentAssets() {
    if (content_dsc_) {
        heap_caps_free(content_dsc_);
        content_dsc_ = nullptr;
    }
    if (content_buf_) {
        heap_caps_free(content_buf_);
        content_buf_ = nullptr;
    }
}

void Ui::HeaderLocked(const char* left, const char* right) {
    header_detail_ = right ? right : "";
    LabelLocked(lv_screen_active(), left, &font_puhui_14_1, Black(), 16, 10, 92);
    header_right_label_ = LabelLocked(lv_screen_active(), "", &font_puhui_14_1,
                                      Black(), 106, 10, 210);
    lv_obj_set_style_text_align(header_right_label_, LV_TEXT_ALIGN_RIGHT, 0);
    constexpr int kSignalX = 334;
    constexpr int kSignalBaseline = 27;
    for (int i = 0; i < 3; ++i) {
        const int height = 4 + i * 4;
        network_signal_[i] = RectObj(lv_screen_active(), kSignalX + i * 7,
                                     kSignalBaseline - height, 4, height,
                                     network_connected_ ? Black() : White(),
                                     Black(), 0, 1);
    }
    battery_outline_ = RectObj(lv_screen_active(), 362, 16, 18, 10,
                               White(), Black(), 0, 1);
    battery_nub_ = RectObj(lv_screen_active(), 381, 19, 3, 4,
                           Black(), Black(), 0, 0);
    UpdateStatusLabelsLocked();
    UpdateBatteryIndicatorLocked();

    lv_obj_t* line = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 16, 32);
    lv_obj_set_size(line, 368, 2);
    lv_obj_set_style_bg_color(line, Black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

void Ui::FooterLocked(const char* text) {
    lv_obj_t* line = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 16, 258);
    lv_obj_set_size(line, 368, 2);
    lv_obj_set_style_bg_color(line, Black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    footer_label_ = LabelLocked(lv_screen_active(), text, &font_puhui_14_1, Black(), 18, 270, 364);
    lv_obj_set_style_text_align(footer_label_, LV_TEXT_ALIGN_CENTER, 0);
}

lv_obj_t* Ui::LabelLocked(lv_obj_t* parent, const char* text, const lv_font_t* font,
                          lv_color_t color, int x, int y, int w) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    return label;
}

std::string Ui::NowString(const char* fallback) const {
    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) {
        return fallback;
    }
    char buf[32];
    std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm);
    return buf;
}

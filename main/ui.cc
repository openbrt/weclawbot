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

    FooterLocked("首次使用请通过串口配置 Wi-Fi");
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

    auto* line2 = LabelLocked(lv_screen_active(), "通过本地控制台保存无线网络", &font_puhui_14_1, Black(), 34, 176, 332);
    lv_obj_set_style_text_align(line2, LV_TEXT_ALIGN_CENTER, 0);

    auto* line3 = LabelLocked(lv_screen_active(), status, &font_puhui_14_1, Black(), 28, 210, 344);
    lv_obj_set_style_text_align(line3, LV_TEXT_ALIGN_CENTER, 0);

    FooterLocked("保存 Wi-Fi 后重启设备");
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
    int scale = 210 / total;
    if (scale < 2) scale = 2;
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

    HeaderLocked("WeClawBot", "微信登录");
    auto* img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, qr_dsc_);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -8);

    char caption[96];
    std::snprintf(caption, sizeof(caption), "微信扫码连接  有效期 %d:%02d",
                  seconds_left / 60, seconds_left % 60);
    qr_status_label_ = LabelLocked(lv_screen_active(), caption, &font_puhui_14_1, Black(), 24, 265, 352);
    lv_obj_set_style_text_align(qr_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    display_.Unlock();
}

void Ui::ShowQrStatus(const char* status, int seconds_left) {
    display_.Lock();
    char caption[96];
    std::snprintf(caption, sizeof(caption), "%s  有效期 %d:%02d", status,
                  seconds_left / 60, seconds_left % 60);
    if (qr_status_label_) {
        lv_label_set_text(qr_status_label_, caption);
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

    red_block_pet_ = RedBlockPetCreate(lv_screen_active());
    RedBlockPetSetState(red_block_pet_, RedBlockPetState::kThinking);

    HeaderLocked("WeClawBot", "思考中");

    auto* label = LabelLocked(lv_screen_active(), status ? status : "正在整理微信内容",
                              &font_puhui_20_4, Black(), 34, 218, 332);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    FooterLocked(footer ? footer : "红方块思考中...");
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
    red_block_pet_ = RedBlockPetCreate(lv_screen_active());
    RedBlockPetSetState(red_block_pet_, RedBlockPetState::kIdle);
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

void Ui::ClearLocked() {
    note_view_active_ = false;
    if (red_block_pet_) {
        RedBlockPetDestroy(red_block_pet_);
        red_block_pet_ = nullptr;
    }
    lv_obj_clean(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), White(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    qr_status_label_ = nullptr;
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
    LabelLocked(lv_screen_active(), left, &font_puhui_14_1, Black(), 16, 10, 220);
    auto* r = LabelLocked(lv_screen_active(), right, &font_puhui_14_1, Black(), 240, 10, 144);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);

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

    auto* label = LabelLocked(lv_screen_active(), text, &font_puhui_14_1, Black(), 18, 270, 364);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
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

#include "serial_config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <string>

#include <cJSON.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>

#if CONFIG_TINYUSB_CDC_ENABLED
#define WEC_HAS_TINYUSB_CDC 1
#include <tinyusb_cdc_acm.h>
#endif

#if !CONFIG_TINYUSB_CDC_ENABLED && \
    (CONFIG_USJ_ENABLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || \
     CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#define WEC_HAS_USB_SERIAL_JTAG 1
#include <driver/usb_serial_jtag.h>
#endif

#if CONFIG_ESP_CONSOLE_UART
#define WEC_HAS_CONSOLE_UART 1
#include <driver/uart.h>
#endif

#include "app_config.h"

namespace {
constexpr char kTag[] = "SerialConfig";
constexpr size_t kLineBufferSize = 1536;
constexpr char kCuratorUrlKey[] = "curator_url";
constexpr char kAiProviderKey[] = "ai_provider";
constexpr char kAiTokenKey[] = "ai_token";
constexpr char kAiEndpointKey[] = "ai_endpoint";
constexpr char kAiModelKey[] = "ai_model";
constexpr int kGpioScanDefaultMs = 60000;
constexpr int kGpioScanMaxMs = 180000;

constexpr gpio_num_t kGpioScanPins[] = {
    WEC_BOOT_BUTTON_GPIO,
    WEC_KEY_BUTTON_GPIO,
};

constexpr gpio_num_t kGpioProbePins[] = {
    GPIO_NUM_0,  GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_3,  GPIO_NUM_4,
    GPIO_NUM_5,  GPIO_NUM_6,  GPIO_NUM_7,  GPIO_NUM_8,  GPIO_NUM_9,
    GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14,
    GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19,
    GPIO_NUM_20, GPIO_NUM_21, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35,
    GPIO_NUM_36, GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40,
    GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_45,
    GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48,
};

std::atomic_bool g_gpio_scan_running{false};

struct GpioScanRequest {
    int duration_ms;
};

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string JsonString(const cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(root), key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

bool LooksLikeHttpUrl(const std::string& value) {
    return value.empty() || value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0;
}

std::string ReadConfigString(const char* key, const char* fallback = "") {
    nvs_handle_t nvs = 0;
    if (nvs_open(WEC_CONFIG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return fallback ? fallback : "";
    }

    std::string out;
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) == ESP_OK && len > 1) {
        std::string value(len, '\0');
        if (nvs_get_str(nvs, key, value.data(), &len) == ESP_OK) {
            if (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            out = value;
        }
    }
    nvs_close(nvs);
    if (out.empty()) {
        out = fallback ? fallback : "";
    }
    return out;
}

bool SaveConfigString(const char* key, const std::string& value) {
    nvs_handle_t nvs = 0;
    if (nvs_open(WEC_CONFIG_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = value.empty() ? nvs_erase_key(nvs, key)
                                  : nvs_set_str(nvs, key, value.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

#if WEC_HAS_TINYUSB_CDC
void WriteCdcBytes(const uint8_t* data, size_t size) {
    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) || !data || size == 0) {
        return;
    }

    constexpr size_t kChunkSize = 128;
    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const size_t requested = remaining < kChunkSize ? remaining : kChunkSize;
        const size_t queued =
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + offset, requested);
        if (queued == 0) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        offset += queued;
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
    }
}
#endif

void WriteResponseLine(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
#if WEC_HAS_TINYUSB_CDC
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        WriteCdcBytes(reinterpret_cast<const uint8_t*>(line.data()), line.size());
        static constexpr uint8_t newline[] = "\n";
        WriteCdcBytes(newline, 1);
    }
#endif
}

void PrintJson(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    if (raw) {
        std::string line = "WEC:";
        line += raw;
        WriteResponseLine(line);
        cJSON_free(raw);
    }
}

void AddLocalTime(cJSON* root) {
    std::time_t now = 0;
    std::time(&now);
    cJSON_AddNumberToObject(root, "device_epoch", static_cast<double>(now));

    std::tm tm = {};
    localtime_r(&now, &tm);
    if (tm.tm_year >= 120) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(root, "device_time", buf);
    } else {
        cJSON_AddStringToObject(root, "device_time", "");
    }
}

void PrintGpioScanEvent(const char* stage, int gpio = -1, int level = -1, int elapsed_ms = -1,
                        const char* message = nullptr) {
    char buffer[512];
    int used = std::snprintf(buffer, sizeof(buffer),
                             "WEC:{\"ok\":true,\"type\":\"gpio_scan\",\"stage\":\"%s\"",
                             stage ? stage : "event");
    if (gpio >= 0) {
        used += std::snprintf(buffer + used, sizeof(buffer) - used, ",\"gpio\":%d", gpio);
    }
    if (level >= 0) {
        used += std::snprintf(buffer + used, sizeof(buffer) - used, ",\"level\":%d", level);
    }
    if (elapsed_ms >= 0) {
        used += std::snprintf(buffer + used, sizeof(buffer) - used, ",\"elapsed_ms\":%d",
                              elapsed_ms);
    }
    if (message) {
        used += std::snprintf(buffer + used, sizeof(buffer) - used, ",\"message\":\"%s\"",
                              message);
    }
    std::snprintf(buffer + used, sizeof(buffer) - used, "}");
    WriteResponseLine(buffer);
}

void PrintGpioLevels() {
    std::string line = "WEC:{\"ok\":true,\"type\":\"gpio_levels\",\"levels\":{";
    for (size_t i = 0; i < sizeof(kGpioProbePins) / sizeof(kGpioProbePins[0]); ++i) {
        const int gpio = static_cast<int>(kGpioProbePins[i]);
        char item[24];
        std::snprintf(item, sizeof(item), "%s\"%d\":%d", i == 0 ? "" : ",", gpio,
                      gpio_get_level(kGpioProbePins[i]));
        line += item;
    }
    line += "}}";
    WriteResponseLine(line);
}

void GpioScanTask(void* arg) {
    auto* request = static_cast<GpioScanRequest*>(arg);
    const int duration_ms = request ? request->duration_ms : kGpioScanDefaultMs;
    delete request;

    for (gpio_num_t gpio : kGpioScanPins) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << static_cast<int>(gpio);
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "gpio scan config GPIO%d failed: %s", static_cast<int>(gpio),
                     esp_err_to_name(err));
        }
    }

    constexpr size_t kPinCount = sizeof(kGpioScanPins) / sizeof(kGpioScanPins[0]);
    int stable[kPinCount] = {};
    int raw[kPinCount] = {};
    int same_count[kPinCount] = {};
    for (size_t i = 0; i < kPinCount; ++i) {
        stable[i] = gpio_get_level(kGpioScanPins[i]);
        raw[i] = stable[i];
        same_count[i] = 3;
    }

    PrintGpioScanEvent("start", -1, -1, 0, "按键 GPIO 扫描已开始");
    for (size_t i = 0; i < kPinCount; ++i) {
        PrintGpioScanEvent("baseline", static_cast<int>(kGpioScanPins[i]), stable[i], 0);
    }
    const int64_t started_us = esp_timer_get_time();
    while ((esp_timer_get_time() - started_us) / 1000 < duration_ms) {
        for (size_t i = 0; i < kPinCount; ++i) {
            const int level = gpio_get_level(kGpioScanPins[i]);
            if (level != raw[i]) {
                raw[i] = level;
                same_count[i] = 1;
                continue;
            }
            if (same_count[i] < 3) {
                ++same_count[i];
            }
            if (same_count[i] == 3 && raw[i] != stable[i]) {
                stable[i] = raw[i];
                const int elapsed_ms = static_cast<int>((esp_timer_get_time() - started_us) / 1000);
                PrintGpioScanEvent("change", static_cast<int>(kGpioScanPins[i]), stable[i], elapsed_ms);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    PrintGpioScanEvent("done", -1, -1, duration_ms, "按键 GPIO 扫描已结束");
    g_gpio_scan_running.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

bool SerialConfig::Start() {
    InstallChannels();
    BaseType_t ok = xTaskCreate(TaskThunk, "serial_config", 12288, this, 4, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate failed");
        return false;
    }
    return true;
}

void SerialConfig::TaskThunk(void* arg) {
    static_cast<SerialConfig*>(arg)->Run();
}

void SerialConfig::Run() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ESP_LOGI(kTag, "USB serial config task ready");

    char line[kLineBufferSize];
    size_t n = 0;
    while (true) {
        uint8_t b = 0;
        int got = ReadByte(&b, 200);
        if (got <= 0) {
            continue;
        }
        if (b == '\n' || b == '\r') {
            if (n > 0) {
                line[n] = '\0';
                HandleLine(line);
                n = 0;
            }
            continue;
        }
        if (n < sizeof(line) - 1) {
            line[n++] = static_cast<char>(b);
        } else {
            n = 0;
        }
    }
}

bool SerialConfig::InstallChannels() {
    bool installed = false;
#if WEC_HAS_TINYUSB_CDC
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        installed = true;
    }
#endif
#if WEC_HAS_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 256;
    cfg.rx_buffer_size = 256;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        installed = true;
    } else {
        ESP_LOGW(kTag, "USB-Serial-JTAG driver install failed: %s", esp_err_to_name(err));
    }
#endif
#if WEC_HAS_CONSOLE_UART
    constexpr uart_port_t kConsoleUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    if (!uart_is_driver_installed(kConsoleUart)) {
        esp_err_t err = uart_driver_install(kConsoleUart, 1024, 0, 0, nullptr, 0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "UART driver install failed: %s", esp_err_to_name(err));
        }
    }
    installed = true;
#endif
    return installed;
}

int SerialConfig::ReadByte(uint8_t* out, int timeout_ms) {
    if (!out) {
        return 0;
    }
    int per_channel = timeout_ms / 2;
    if (per_channel < 10) {
        per_channel = 10;
    }

#if WEC_HAS_TINYUSB_CDC
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(per_channel) * 1000;
    while (esp_timer_get_time() < deadline_us) {
        if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
            size_t rx_size = 0;
            esp_err_t err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, out, 1, &rx_size);
            if (err == ESP_OK && rx_size > 0) {
                return static_cast<int>(rx_size);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
#if WEC_HAS_USB_SERIAL_JTAG
    int n = usb_serial_jtag_read_bytes(out, 1, pdMS_TO_TICKS(per_channel));
    if (n > 0) {
        return n;
    }
#endif
#if WEC_HAS_CONSOLE_UART
    constexpr uart_port_t kConsoleUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    int m = uart_read_bytes(kConsoleUart, out, 1, pdMS_TO_TICKS(per_channel));
    if (m > 0) {
        return m;
    }
#endif
    return 0;
}

void SerialConfig::HandleLine(const char* line) {
    if (!line) {
        return;
    }

    std::string input = Trim(line);
    if (input.rfind("WEC:", 0) != 0) {
        return;
    }
    input = Trim(input.substr(4));

    if (input == "HELLO" || input == "PING") {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "type", "hello");
        cJSON_AddStringToObject(root, "name", "WeClawBot");
        cJSON_AddStringToObject(root, "version", WEC_FIRMWARE_VERSION);
        cJSON_AddStringToObject(root, "chip", "ESP32-S3");
        AddLocalTime(root);
        PrintJson(root);
        cJSON_Delete(root);
        return;
    }

    if (input == "GET") {
        SendStatus();
        return;
    }

    if (input == "GPIO_SCAN" || input.rfind("GPIO_SCAN ", 0) == 0) {
        HandleGpioScan(input);
        return;
    }

    if (input == "GPIO_LEVELS") {
        PrintGpioLevels();
        return;
    }

    if (input.rfind("SET ", 0) == 0) {
        if (HandleSet(input.c_str() + 4)) {
            SendOk("set", "配置已保存，请重启设备");
        }
        return;
    }

    if (input == "CLEAR_WIFI") {
        if (WifiManager::ClearCredentials()) {
            ui_.ShowUsbConfig("Wi-Fi 配置已清除");
            SendOk("clear_wifi");
        } else {
            SendError("nvs_write_failed", "清除 Wi-Fi 配置失败");
        }
        return;
    }

    if (input == "CLEAR_WECHAT") {
        bot_.ClearSavedCredentials();
        ui_.ShowUsbConfig("微信登录状态已清除");
        SendOk("clear_wechat");
        return;
    }

    if (input == "CLEAR_NOTES") {
        notes_.ClearAll();
        if (const Note* photo = notes_.IdlePhoto()) {
            ui_.ShowIdlePhoto(*photo);
        } else {
            ui_.ShowEmptyNotes();
        }
        SendOk("clear_notes");
        return;
    }

    if (input == "SHOW_PET") {
        if (notes_.Empty() && notes_.HasIdlePhoto()) {
            ui_.ShowIdlePhoto(*notes_.IdlePhoto());
        } else {
            ui_.ShowScreensaver(bot_.Connected(), notes_.Count());
        }
        SendOk("show_pet");
        return;
    }

    if (input.rfind("CURATE_TEST ", 0) == 0) {
        std::string text = Trim(input.substr(12));
        if (text.empty()) {
            SendError("empty_text", "测试文本不能为空");
            return;
        }
        SendOk("curate_test", "正在从设备请求云端整理");
        const bool ok = bot_.CurateLoopbackText(text.c_str());
        if (ok) {
            SendOk("curate_test_done", "云端整理完成并已尝试上屏");
        } else {
            SendError("curate_test_failed", "云端整理或上屏失败");
        }
        return;
    }

    if (input.rfind("PHOTO_TEST ", 0) == 0) {
        std::string url = Trim(input.substr(11));
        if (url.empty()) {
            SendError("empty_url", "测试图片 URL 不能为空");
            return;
        }
        SendOk("photo_test", "正在从设备请求云端照片相框");
        const bool ok = bot_.CurateLoopbackImage(url.c_str());
        if (ok) {
            SendOk("photo_test_done", "云端照片处理完成并已尝试保存相框");
        } else {
            SendError("photo_test_failed", "云端照片处理失败");
        }
        return;
    }

    if (input == "REBOOT") {
        SendOk("reboot", "设备正在重启");
        std::fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return;
    }

    SendError("unknown_command", "未知命令");
}

void SerialConfig::HandleGpioScan(const std::string& input) {
    int duration_ms = kGpioScanDefaultMs;
    if (input.rfind("GPIO_SCAN ", 0) == 0) {
        std::string raw = Trim(input.substr(10));
        if (!raw.empty()) {
            duration_ms = std::atoi(raw.c_str());
        }
    }
    if (duration_ms < 5000) {
        duration_ms = 5000;
    }
    if (duration_ms > kGpioScanMaxMs) {
        duration_ms = kGpioScanMaxMs;
    }

    bool expected = false;
    if (!g_gpio_scan_running.compare_exchange_strong(expected, true)) {
        SendError("gpio_scan_busy", "GPIO 扫描已经在运行");
        return;
    }

    auto* request = new GpioScanRequest{duration_ms};
    BaseType_t ok = xTaskCreate(GpioScanTask, "gpio_scan", 4096, request, 3, nullptr);
    if (ok != pdPASS) {
        delete request;
        g_gpio_scan_running.store(false);
        SendError("gpio_scan_start_failed", "GPIO 扫描任务启动失败");
        return;
    }
    SendOk("gpio_scan", "GPIO 扫描已在后台启动");
}

void SerialConfig::SendStatus() {
    wifi_.LoadCredentials();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "config");
    cJSON_AddStringToObject(root, "name", "WeClawBot");
    cJSON_AddStringToObject(root, "version", WEC_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "board", "Waveshare ESP32-S3-RLCD-4.2");
    AddLocalTime(root);
    cJSON_AddBoolToObject(root, "wifi_configured", wifi_.HasConfiguredSsid());
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_.ConfiguredSsid().c_str());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_.Connected());
    cJSON_AddStringToObject(root, "ip", wifi_.IpAddress().c_str());
    cJSON_AddNumberToObject(root, "wifi_error_reason", wifi_.LastDisconnectReason());
    cJSON_AddStringToObject(root, "wifi_error", wifi_.LastDisconnectReasonText());
    std::string curator_url = ReadConfigString(kCuratorUrlKey, CONFIG_WEC_CURATOR_URL);
    std::string ai_provider = ReadConfigString(kAiProviderKey, "weclawbot");
    std::string ai_endpoint = ReadConfigString(kAiEndpointKey, "");
    std::string ai_model = ReadConfigString(kAiModelKey, "");
    std::string ai_token = ReadConfigString(kAiTokenKey, "");
    cJSON_AddBoolToObject(root, "curator_enabled", !curator_url.empty());
    cJSON_AddStringToObject(root, "curator_url", curator_url.c_str());
    cJSON_AddStringToObject(root, "ai_provider", ai_provider.c_str());
    cJSON_AddBoolToObject(root, "ai_token_configured", !ai_token.empty());
    cJSON_AddStringToObject(root, "ai_endpoint", ai_endpoint.c_str());
    cJSON_AddStringToObject(root, "ai_model", ai_model.c_str());
    cJSON_AddBoolToObject(root, "wechat_connected", bot_.Connected());
    cJSON_AddStringToObject(root, "wechat_state", bot_.LoginStateText());
    cJSON_AddNumberToObject(root, "wechat_qr_seconds_left", bot_.QrSecondsLeft());
    cJSON_AddBoolToObject(root, "screensaver_active", ui_.ScreensaverActive());
    cJSON_AddNumberToObject(root, "screensaver_started_at", static_cast<double>(ui_.ScreensaverStartedAt()));
    cJSON_AddNumberToObject(root, "screensaver_return_sec", CONFIG_WEC_SCREENSAVER_RETURN_SEC);
    cJSON_AddNumberToObject(root, "note_count", static_cast<double>(notes_.Count()));
    cJSON_AddNumberToObject(root, "note_index", static_cast<double>(notes_.CurrentIndex()));
    cJSON_AddBoolToObject(root, "idle_photo_configured", notes_.HasIdlePhoto());
    PrintJson(root);
    cJSON_Delete(root);
}

void SerialConfig::SendOk(const char* type, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", type ? type : "ok");
    if (message) {
        cJSON_AddStringToObject(root, "message", message);
    }
    PrintJson(root);
    cJSON_Delete(root);
}

void SerialConfig::SendError(const char* code, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "code", code ? code : "error");
    cJSON_AddStringToObject(root, "message", message ? message : "配置命令失败");
    PrintJson(root);
    cJSON_Delete(root);
}

bool SerialConfig::HandleSet(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        SendError("bad_json", "配置 JSON 解析失败");
        return false;
    }

    cJSON* ssid_node = cJSON_GetObjectItem(root, "ssid");
    cJSON* password_node = cJSON_GetObjectItem(root, "password");
    cJSON* curator_node = cJSON_GetObjectItem(root, "curator_url");
    cJSON* ai_provider_node = cJSON_GetObjectItem(root, "ai_provider");
    cJSON* ai_token_node = cJSON_GetObjectItem(root, "ai_token");
    cJSON* ai_endpoint_node = cJSON_GetObjectItem(root, "ai_endpoint");
    cJSON* ai_model_node = cJSON_GetObjectItem(root, "ai_model");

    const bool has_ssid = cJSON_IsString(ssid_node);
    const bool has_password = cJSON_IsString(password_node);
    const bool has_curator_url = cJSON_IsString(curator_node);
    const bool has_bad_curator_url = curator_node && !has_curator_url;
    const bool has_ai_provider = cJSON_IsString(ai_provider_node);
    const bool has_ai_token = cJSON_IsString(ai_token_node);
    const bool has_ai_endpoint = cJSON_IsString(ai_endpoint_node);
    const bool has_ai_model = cJSON_IsString(ai_model_node);
    const bool has_bad_ai_provider = ai_provider_node && !has_ai_provider;
    const bool has_bad_ai_token = ai_token_node && !has_ai_token;
    const bool has_bad_ai_endpoint = ai_endpoint_node && !has_ai_endpoint;
    const bool has_bad_ai_model = ai_model_node && !has_ai_model;

    std::string ssid = has_ssid ? ssid_node->valuestring : "";
    std::string password = has_password ? password_node->valuestring : "";
    std::string curator_url = has_curator_url ? curator_node->valuestring : "";
    std::string ai_provider = has_ai_provider ? ai_provider_node->valuestring : "";
    std::string ai_token = has_ai_token ? ai_token_node->valuestring : "";
    std::string ai_endpoint = has_ai_endpoint ? ai_endpoint_node->valuestring : "";
    std::string ai_model = has_ai_model ? ai_model_node->valuestring : "";
    cJSON_Delete(root);

    if (!has_ssid && !has_curator_url && !has_ai_provider && !has_ai_token &&
        !has_ai_endpoint && !has_ai_model && !has_bad_curator_url &&
        !has_bad_ai_provider && !has_bad_ai_token && !has_bad_ai_endpoint &&
        !has_bad_ai_model) {
        SendError("empty_config", "没有可保存的配置项");
        return false;
    }
    if (has_bad_curator_url) {
        SendError("curator_url_invalid", "云端整理 URL 必须是字符串");
        return false;
    }
    if (has_bad_ai_provider || has_bad_ai_token || has_bad_ai_endpoint || has_bad_ai_model) {
        SendError("ai_config_invalid", "AI provider 配置必须是字符串");
        return false;
    }

    if (has_ssid) {
        if (ssid.empty()) {
            SendError("ssid_required", "SSID 不能为空");
            return false;
        }
        if (ssid.size() > 32) {
            SendError("ssid_too_long", "SSID 不能超过 32 字节");
            return false;
        }
        if (password.size() > 63) {
            SendError("password_too_long", "Wi-Fi 密码不能超过 63 字节");
            return false;
        }

        if (!WifiManager::SaveCredentials(ssid, password)) {
            SendError("nvs_write_failed", "保存 Wi-Fi 配置失败");
            return false;
        }
        wifi_.LoadCredentials();
    }

    if (has_curator_url) {
        if (curator_url.size() > 240) {
            SendError("curator_url_too_long", "云端整理 URL 不能超过 240 字节");
            return false;
        }
        if (!LooksLikeHttpUrl(curator_url)) {
            SendError("curator_url_invalid", "云端整理 URL 需要以 http:// 或 https:// 开头");
            return false;
        }
        if (!SaveConfigString(kCuratorUrlKey, curator_url)) {
            SendError("nvs_write_failed", "保存云端整理 URL 失败");
            return false;
        }
    }

    if (has_ai_provider) {
        if (ai_provider.size() > 32) {
            SendError("ai_provider_too_long", "Provider 名称不能超过 32 字节");
            return false;
        }
        if (!SaveConfigString(kAiProviderKey, ai_provider)) {
            SendError("nvs_write_failed", "保存 provider 失败");
            return false;
        }
    }

    if (has_ai_endpoint) {
        if (ai_endpoint.size() > 200) {
            SendError("ai_endpoint_too_long", "Provider Endpoint 不能超过 200 字节");
            return false;
        }
        if (!LooksLikeHttpUrl(ai_endpoint)) {
            SendError("ai_endpoint_invalid", "Provider Endpoint 需要以 http:// 或 https:// 开头");
            return false;
        }
        if (!SaveConfigString(kAiEndpointKey, ai_endpoint)) {
            SendError("nvs_write_failed", "保存 Provider Endpoint 失败");
            return false;
        }
    }

    if (has_ai_model) {
        if (ai_model.size() > 80) {
            SendError("ai_model_too_long", "模型名称不能超过 80 字节");
            return false;
        }
        if (!SaveConfigString(kAiModelKey, ai_model)) {
            SendError("nvs_write_failed", "保存模型名称失败");
            return false;
        }
    }

    if (has_ai_token) {
        if (ai_token == "__CLEAR__") {
            ai_token.clear();
        }
        if (ai_token.size() > 240) {
            SendError("ai_token_too_long", "API key/access token 不能超过 240 字节");
            return false;
        }
        if (!SaveConfigString(kAiTokenKey, ai_token)) {
            SendError("nvs_write_failed", "保存 API key/access token 失败");
            return false;
        }
    }

    ui_.ShowUsbConfig("配置已保存，请重启设备");
    return true;
}

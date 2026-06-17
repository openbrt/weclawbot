#include "wechat_bot.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <strings.h>
#include <utility>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <nvs.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "WechatBot";
constexpr size_t kMaxHttpBody = 128 * 1024;
constexpr int kHttpBufferSize = 4096;
constexpr int kPreviewImageRetryAttempts = 3;
constexpr int kPreviewImageRetryBaseDelayMs = 700;
constexpr char kIlinkChannelVersion[] = "2.0.1";
constexpr char kWechatCdnBaseUrl[] = "https://novac2c.cdn.weixin.qq.com/c2c";
constexpr char kDefaultCdnUploadProxy[] = "https://weclawbot.link/api/cdn/upload";

constexpr char kBotTokenKey[] = "bot_token";
constexpr char kBotIdKey[] = "bot_id";
constexpr char kBaseUrlKey[] = "baseurl";
constexpr char kCursorKey[] = "cursor";
constexpr char kCuratorUrlKey[] = "curator_url";
constexpr char kAiProviderKey[] = "ai_provider";
constexpr char kAiTokenKey[] = "ai_token";
constexpr char kAiEndpointKey[] = "ai_endpoint";
constexpr char kAiModelKey[] = "ai_model";

std::string JsonString(const cJSON* object, const char* key, const char* fallback = "") {
    if (!object || !key) {
        return fallback ? fallback : "";
    }
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(object), key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

int JsonInt(const cJSON* object, const char* key, int fallback = 0) {
    if (!object || !key) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(object), key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

void AddBaseInfo(cJSON* root) {
    cJSON* base_info = cJSON_CreateObject();
    cJSON_AddStringToObject(base_info, "channel_version", kIlinkChannelVersion);
    cJSON_AddItemToObject(root, "base_info", base_info);
}

std::string HexEncode(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    esp_fill_random(data.data(), data.size());
    return HexEncode(data.data(), data.size());
}

std::string Base64EncodeText(const std::string& text) {
    const size_t output_size = ((text.size() + 2) / 3) * 4 + 4;
    std::vector<unsigned char> encoded(output_size);
    size_t out_len = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &out_len,
                              reinterpret_cast<const unsigned char*>(text.data()),
                              text.size()) != 0) {
        return "";
    }
    return std::string(reinterpret_cast<char*>(encoded.data()), out_len);
}

std::vector<uint8_t> Base64DecodeBytes(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(nullptr, 0, &out_len,
                                    reinterpret_cast<const unsigned char*>(text.data()),
                                    text.size());
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || out_len == 0) {
        return {};
    }
    std::vector<uint8_t> out(out_len);
    ret = mbedtls_base64_decode(out.data(), out.size(), &out_len,
                                reinterpret_cast<const unsigned char*>(text.data()),
                                text.size());
    if (ret != 0) {
        return {};
    }
    out.resize(out_len);
    return out;
}

std::string Md5Hex(const std::string& data) {
    unsigned char digest[16] = {};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info || mbedtls_md(info,
                            reinterpret_cast<const unsigned char*>(data.data()),
                            data.size(),
                            digest) != 0) {
        return "";
    }
    return HexEncode(digest, sizeof(digest));
}

size_t AesEcbPaddedSize(size_t size) {
    return ((size / 16) + 1) * 16;
}

std::string EncryptAesEcbPkcs7(const std::string& plaintext, const unsigned char key[16]) {
    const size_t padded_size = AesEcbPaddedSize(plaintext.size());
    std::string ciphertext(padded_size, '\0');
    if (!plaintext.empty()) {
        std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
    }
    const size_t pad_size = padded_size - plaintext.size();
    const unsigned char pad = static_cast<unsigned char>(pad_size);
    std::memset(ciphertext.data() + plaintext.size(), pad, pad_size);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return "";
    }
    for (size_t offset = 0; offset < padded_size; offset += 16) {
        unsigned char block[16];
        std::memcpy(block, ciphertext.data() + offset, sizeof(block));
        if (mbedtls_aes_crypt_ecb(&ctx,
                                  MBEDTLS_AES_ENCRYPT,
                                  block,
                                  reinterpret_cast<unsigned char*>(ciphertext.data() + offset)) != 0) {
            mbedtls_aes_free(&ctx);
            return "";
        }
    }
    mbedtls_aes_free(&ctx);
    return ciphertext;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0x0f]);
            out.push_back(kHex[ch & 0x0f]);
        }
    }
    return out;
}

std::string StripPreviewUrlFromReply(std::string reply) {
    const char* markers[] = {"屏幕预览：", "预览：", "Preview:"};
    size_t pos = std::string::npos;
    for (const char* marker : markers) {
        pos = reply.find(marker);
        if (pos != std::string::npos) {
            break;
        }
    }
    if (pos == std::string::npos) {
        pos = reply.find("/api/preview/");
        if (pos != std::string::npos) {
            while (pos > 0 && reply[pos - 1] != '\n' && reply[pos - 1] != '\r') {
                --pos;
            }
        }
    }
    if (pos == std::string::npos) return reply;
    while (pos > 0 && (reply[pos - 1] == '\n' || reply[pos - 1] == '\r')) {
        --pos;
    }
    reply.erase(pos);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r' ||
                              reply.back() == ' ' || reply.back() == '\t')) {
        reply.pop_back();
    }
    return reply;
}

const char* ItemString(const cJSON* object, const char* key) {
    if (!object || !key) {
        return "";
    }
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(object), key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

const cJSON* JsonObject(const cJSON* object, const char* key) {
    if (!object || !key) {
        return nullptr;
    }
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(object), key);
    return cJSON_IsObject(item) ? item : nullptr;
}

std::string FirstJsonString(const cJSON* object, std::initializer_list<const char*> keys) {
    if (!object) {
        return "";
    }
    for (const char* key : keys) {
        std::string value = JsonString(object, key);
        if (!value.empty()) {
            return value;
        }
    }
    return "";
}

std::string JsonObjectKeys(const cJSON* object, int max_keys = 10) {
    if (!object || !cJSON_IsObject(object)) {
        return "";
    }
    std::string keys;
    int count = 0;
    for (const cJSON* child = object->child; child && count < max_keys; child = child->next) {
        if (!child->string) {
            continue;
        }
        if (!keys.empty()) {
            keys += ",";
        }
        keys += child->string;
        ++count;
    }
    if (object->child && count >= max_keys) {
        keys += ",...";
    }
    return keys;
}

std::string MediaUrlFromItem(const cJSON* item) {
    const cJSON* media = JsonObject(item, "media");
    std::string url = FirstJsonString(media, {"full_url", "fullUrl", "cdn_url", "cdnUrl", "download_url", "url"});
    if (!url.empty()) {
        return url;
    }
    return FirstJsonString(item, {"full_url", "fullUrl", "cdn_url", "cdnUrl", "download_url", "url"});
}

std::string AesKeyFromItem(const cJSON* item) {
    std::string key = FirstJsonString(item, {"aeskey", "aes_key", "aesKey"});
    if (!key.empty()) {
        return key;
    }
    return FirstJsonString(JsonObject(item, "media"), {"aes_key", "aeskey", "aesKey"});
}

size_t SizeFromItem(const cJSON* item) {
    if (!item) {
        return 0;
    }
    const char* keys[] = {"len", "size", "file_size", "fileSize"};
    for (const char* key : keys) {
        cJSON* node = cJSON_GetObjectItem(const_cast<cJSON*>(item), key);
        if (cJSON_IsNumber(node) && node->valuedouble > 0) {
            return static_cast<size_t>(node->valuedouble);
        }
        if (cJSON_IsString(node) && node->valuestring && node->valuestring[0] != '\0') {
            char* end = nullptr;
            const unsigned long long value = std::strtoull(node->valuestring, &end, 10);
            if (end && *end == '\0') {
                return static_cast<size_t>(value);
            }
        }
    }
    return 0;
}

std::string PreviewText(const char* text, size_t max_bytes = 80) {
    if (!text) {
        return "";
    }
    std::string value(text);
    if (value.size() <= max_bytes) {
        return value;
    }
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return value.substr(0, cut) + "...";
}

void PrintJsonLine(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    if (raw) {
        std::printf("WEC:%s\n", raw);
        std::fflush(stdout);
        cJSON_free(raw);
    }
}

void EmitEventLog(cJSON* root) {
    if (!root) {
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "event");
    PrintJsonLine(root);
    cJSON_Delete(root);
}

cJSON* NewEventLog(const char* scope, const char* stage) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scope", scope ? scope : "device");
    cJSON_AddStringToObject(root, "stage", stage ? stage : "event");
    return root;
}

void EmitHeapLog(const char* scope, const char* stage) {
    cJSON* log = NewEventLog(scope, stage);
    cJSON_AddNumberToObject(log, "free_heap",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(log, "largest_free_block",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(log, "free_internal",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(log, "largest_internal",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    EmitEventLog(log);
}

std::string TrimCommandArgument(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == ':' || value.front() == ',')) {
        value.erase(value.begin());
    }
    while (value.rfind("：", 0) == 0 || value.rfind("，", 0) == 0) {
        value.erase(0, 3);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
    }
    return value;
}

std::string ExtractReplacementText(const char* text) {
    if (!text || text[0] == '\0') {
        return "";
    }
    const std::string input(text);
    const char* prefixes[] = {"修改为", "修改成", "改为", "改成", "请修改为", "请改为", "请改成"};
    for (const char* prefix : prefixes) {
        const size_t len = std::strlen(prefix);
        if (input.rfind(prefix, 0) == 0) {
            return TrimCommandArgument(input.substr(len));
        }
    }
    return "";
}

bool ExtractScreenFrames(const cJSON* decision,
                         std::vector<std::vector<uint8_t>>& frames,
                         int& width,
                         int& height,
                         int& stride,
                         int max_width,
                         int max_height) {
    frames.clear();
    width = 0;
    height = 0;
    stride = 0;

    cJSON* pages = cJSON_GetObjectItem(const_cast<cJSON*>(decision), "screen_pages");
    if (!cJSON_IsArray(pages)) {
        return false;
    }
    const int count = cJSON_GetArraySize(pages);
    if (count <= 0 || count > 3) {
        return false;
    }

    for (int i = 0; i < count; ++i) {
        cJSON* page = cJSON_GetArrayItem(pages, i);
        const std::string format = JsonString(page, "format");
        const int page_width = JsonInt(page, "width");
        const int page_height = JsonInt(page, "height");
        const int page_stride = JsonInt(page, "stride");
        const std::string data_b64 = JsonString(page, "data_b64");
        if (format != "mono1" || page_width <= 0 || page_height <= 0 ||
            page_width > max_width || page_height > max_height ||
            page_stride < (page_width + 7) / 8 || data_b64.empty()) {
            frames.clear();
            return false;
        }
        if (width == 0) {
            width = page_width;
            height = page_height;
            stride = page_stride;
        } else if (width != page_width || height != page_height || stride != page_stride) {
            frames.clear();
            return false;
        }

        std::vector<uint8_t> decoded = Base64DecodeBytes(data_b64);
        const size_t needed = static_cast<size_t>(page_stride) * static_cast<size_t>(page_height);
        if (decoded.size() < needed) {
            frames.clear();
            return false;
        }
        decoded.resize(needed);
        frames.push_back(std::move(decoded));
    }
    return !frames.empty();
}

std::string CdnUploadProxyEndpoint(const std::string& curator_url) {
    std::string proxy = kDefaultCdnUploadProxy;
    const size_t scheme = curator_url.find("://");
    if (scheme != std::string::npos) {
        const size_t path_start = curator_url.find('/', scheme + 3);
        if (path_start != std::string::npos) {
            proxy = curator_url.substr(0, path_start) + "/api/cdn/upload";
        }
    }
    return proxy;
}
}  // namespace

const char* WechatBot::LoginStateText() const {
    switch (login_state_.load()) {
        case LoginState::kStarting:
            return "正在启动";
        case LoginState::kFetchingQr:
            return "正在获取二维码";
        case LoginState::kWaitingForScan:
            return "等待微信扫码";
        case LoginState::kScanned:
            return "已扫码，请在微信确认";
        case LoginState::kSwitchingLine:
            return "已扫码，正在切换登录线路";
        case LoginState::kConnected:
            return "已连接";
        case LoginState::kQrError:
            return "二维码请求失败，正在重试";
    }
    return "未知";
}

esp_err_t WechatBot::HttpEventHandler(esp_http_client_event_t* event) {
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (!response) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "x-encrypted-param") == 0) {
            response->encrypted_param = event->header_value;
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        if (response->body.size() + event->data_len <= kMaxHttpBody) {
            response->body.append(static_cast<const char*>(event->data), event->data_len);
        } else {
            response->overflow = true;
        }
    } else if (event->event_id == HTTP_EVENT_ERROR) {
        response->overflow = true;
    }
    return ESP_OK;
}

void WechatBot::Start() {
    esp_log_level_set(kTag, ESP_LOG_INFO);
    // A quiet getupdates long poll ends with ESP_ERR_HTTP_EAGAIN. It is a
    // normal idle heartbeat, so keep the ESP-IDF transport warning off-screen.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_ERROR);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    EnsureHttpMutex();
    TouchActivity();
    SyncTime();
    LoadCredentials();

    while (true) {
        if (relogin_requested_.exchange(false)) {
            ui_.ShowBoot("正在清除登录状态");
            ClearCredentials();
            bot_token_.clear();
            cursor_.clear();
            connected_ = false;
        }

        if (bot_token_.empty()) {
            if (!DoQrLogin()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        connected_ = true;
        login_state_ = LoginState::kConnected;
        qr_seconds_left_ = 0;
        ui_.ShowLoginSuccess();
        vTaskDelay(pdMS_TO_TICKS(900));
        RenderCurrentOrEmpty();
        RunGetUpdatesLoop();
    }
}

void WechatBot::RequestRelogin() {
    relogin_requested_ = true;
}

void WechatBot::ClearSavedCredentials() {
    ClearCredentials();
    bot_token_.clear();
    bot_id_.clear();
    qrcode_token_.clear();
    cursor_.clear();
    connected_ = false;
    login_state_ = LoginState::kStarting;
    qr_seconds_left_ = 0;
    relogin_requested_ = true;
}

bool WechatBot::CurateLoopbackText(const char* text) {
    if (!text || text[0] == '\0') {
        return false;
    }
    LoadCuratorConfig();
    cJSON* log = NewEventLog("curator", "loopback_start");
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    EmitEventLog(log);
    local_curator_active_ = true;
    const bool ok = CurateText(nullptr, text, false);
    local_curator_active_ = false;
    return ok;
}

bool WechatBot::CurateLoopbackImage(const char* url) {
    if (!url || url[0] == '\0') {
        return false;
    }
    LoadCuratorConfig();
    cJSON* log = NewEventLog("curator", "loopback_image_start");
    cJSON_AddStringToObject(log, "url", url);
    EmitEventLog(log);
    local_curator_active_ = true;
    const bool ok = CurateAttachment(nullptr, "wechat_image", "loopback-photo.jpg",
                                     url, "00000000000000000000000000000000",
                                     "image", 0);
    local_curator_active_ = false;
    return ok;
}

void WechatBot::SyncTime() {
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.smooth_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(err));
    }

    std::time_t now = 0;
    std::tm tm = {};
    for (int i = 0; i < 24; ++i) {
        std::time(&now);
        localtime_r(&now, &tm);
        if (tm.tm_year >= 120) {
            ESP_LOGI(kTag, "Time synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(kTag, "Time sync timed out, continuing");
}

void WechatBot::LoadCredentials() {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_WECHAT_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        LoadCuratorConfig();
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open read failed: %s", esp_err_to_name(err));
        LoadCuratorConfig();
        return;
    }

    auto read_str = [nvs](const char* key, std::string& out) {
        size_t len = 0;
        if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len <= 1) {
            return;
        }
        std::string value(len, '\0');
        if (nvs_get_str(nvs, key, value.data(), &len) == ESP_OK) {
            if (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            out = value;
        }
    };

    read_str(kBotTokenKey, bot_token_);
    read_str(kBotIdKey, bot_id_);
    read_str(kBaseUrlKey, base_url_);
    read_str(kCursorKey, cursor_);
    if (base_url_.empty()) {
        base_url_ = CONFIG_WEC_WECHAT_BASE_URL;
    }
    nvs_close(nvs);
    LoadCuratorConfig();
}

void WechatBot::LoadCuratorConfig() {
    curator_url_.clear();
    ai_provider_.clear();
    ai_token_.clear();
    ai_endpoint_.clear();
    ai_model_.clear();

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        auto read_str = [nvs](const char* key, std::string& out) {
            size_t len = 0;
            if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len <= 1) {
                return;
            }
            std::string value(len, '\0');
            if (nvs_get_str(nvs, key, value.data(), &len) == ESP_OK) {
                if (!value.empty() && value.back() == '\0') {
                    value.pop_back();
                }
                out = value;
            }
        };
        read_str(kCuratorUrlKey, curator_url_);
        read_str(kAiProviderKey, ai_provider_);
        read_str(kAiTokenKey, ai_token_);
        read_str(kAiEndpointKey, ai_endpoint_);
        read_str(kAiModelKey, ai_model_);
        nvs_close(nvs);
    }

    if (curator_url_.empty()) {
        curator_url_ = CONFIG_WEC_CURATOR_URL;
    }
    if (ai_provider_.empty()) {
        ai_provider_ = "weclawbot";
    }

    cJSON* log = NewEventLog("curator", "config_loaded");
    cJSON_AddBoolToObject(log, "enabled", !curator_url_.empty());
    cJSON_AddStringToObject(log, "url", PreviewText(curator_url_.c_str(), 120).c_str());
    cJSON_AddStringToObject(log, "ai_provider", ai_provider_.c_str());
    cJSON_AddBoolToObject(log, "ai_token_configured", !ai_token_.empty());
    EmitEventLog(log);
}

void WechatBot::SaveCredentials() {
    nvs_handle_t nvs = 0;
    if (nvs_open(WEC_WECHAT_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, kBotTokenKey, bot_token_.c_str());
    nvs_set_str(nvs, kBotIdKey, bot_id_.c_str());
    nvs_set_str(nvs, kBaseUrlKey, base_url_.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
}

void WechatBot::ClearCredentials() {
    nvs_handle_t nvs = 0;
    if (nvs_open(WEC_WECHAT_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool WechatBot::DoQrLogin() {
    connected_ = false;
    while (!relogin_requested_) {
        if (FetchQrCode() && PollQrStatus()) {
            SaveCredentials();
            connected_ = true;
            return true;
        }
        login_state_ = LoginState::kQrError;
        qr_seconds_left_ = 0;
        ui_.ShowError("二维码已过期", "正在请求新的微信登录二维码");
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    return false;
}

bool WechatBot::FetchQrCode() {
    login_state_ = LoginState::kFetchingQr;
    qr_seconds_left_ = 0;
    std::string url = base_url_ + "/ilink/bot/get_bot_qrcode?bot_type=3";
    ESP_LOGI(kTag, "Fetching QR");

    std::string response = HttpGet(url);
    if (response.empty()) {
        login_state_ = LoginState::kQrError;
        ui_.ShowError("二维码请求失败", "请检查网络或串口日志");
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        login_state_ = LoginState::kQrError;
        ui_.ShowError("二维码解析失败", "微信返回内容不是 JSON");
        return false;
    }

    int ret = 0;
    cJSON* ret_node = cJSON_GetObjectItem(root, "ret");
    if (cJSON_IsNumber(ret_node)) {
        ret = ret_node->valueint;
    }

    qrcode_token_ = JsonString(root, "qrcode");
    std::string qr_url = JsonString(root, "qrcode_img_content");
    cJSON_Delete(root);

    if (ret != 0 || qrcode_token_.empty() || qr_url.empty()) {
        login_state_ = LoginState::kQrError;
        ESP_LOGW(kTag, "Unexpected QR response: %.200s", response.c_str());
        ui_.ShowError("二维码不可用", "微信登录接口返回异常");
        return false;
    }

    qr_fetch_time_ = esp_timer_get_time() / 1000000;
    qr_seconds_left_ = WEC_QR_EXPIRY_MS / 1000;
    login_state_ = LoginState::kWaitingForScan;
    ui_.ShowQr(qr_url.c_str(), WEC_QR_EXPIRY_MS / 1000);
    return true;
}

bool WechatBot::PollQrStatus() {
    while (!relogin_requested_) {
        const int64_t now = esp_timer_get_time() / 1000000;
        const int elapsed = static_cast<int>(now - qr_fetch_time_);
        const int seconds_left = std::max(0, (WEC_QR_EXPIRY_MS / 1000) - elapsed);
        qr_seconds_left_ = seconds_left;
        if (seconds_left <= 0) {
            ESP_LOGI(kTag, "QR expired locally, requesting a new code");
            return false;
        }

        std::string url = base_url_ + "/ilink/bot/get_qrcode_status?qrcode=" + qrcode_token_;
        std::string response = HttpGet(url);
        if (response.empty()) {
            login_state_ = LoginState::kWaitingForScan;
            ui_.ShowQrStatus("等待扫码", seconds_left);
            vTaskDelay(pdMS_TO_TICKS(WEC_QR_POLL_INTERVAL_MS));
            continue;
        }

        cJSON* root = cJSON_Parse(response.c_str());
        if (!root) {
            vTaskDelay(pdMS_TO_TICKS(WEC_QR_POLL_INTERVAL_MS));
            continue;
        }

        std::string status = JsonString(root, "status");
        if (status == "wait") {
            login_state_ = LoginState::kWaitingForScan;
            ui_.ShowQrStatus("等待扫码", seconds_left);
        } else if (status == "scanned") {
            login_state_ = LoginState::kScanned;
            ui_.ShowQrStatus("已扫码，请在微信确认", seconds_left);
        } else if (status == "scaned_but_redirect") {
            std::string redirect = JsonString(root, "redirect_host");
            if (!redirect.empty()) {
                base_url_ = redirect;
            }
            login_state_ = LoginState::kSwitchingLine;
            ui_.ShowQrStatus("已扫码，切换登录线路", seconds_left);
        } else if (status == "confirmed") {
            bot_token_ = JsonString(root, "bot_token");
            bot_id_ = JsonString(root, "ilink_bot_id");
            std::string confirmed_base = JsonString(root, "baseurl");
            if (!confirmed_base.empty()) {
                base_url_ = confirmed_base;
            }
            cJSON_Delete(root);
            if (!bot_token_.empty() && !bot_id_.empty()) {
                connected_ = true;
                login_state_ = LoginState::kConnected;
                qr_seconds_left_ = 0;
                ui_.ShowLoginSuccess();
                return true;
            }
            ui_.ShowError("登录异常", "确认成功但缺少 bot token");
            return false;
        } else if (status == "expired") {
            cJSON_Delete(root);
            return false;
        }

        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(WEC_QR_POLL_INTERVAL_MS));
    }
    return false;
}

void WechatBot::RunGetUpdatesLoop() {
    int fail_count = 0;
    while (!relogin_requested_) {
        const bool ok = DoGetUpdates();
        if (ok) {
            if (fail_count >= 10) {
                RenderCurrentOrEmpty();
            }
            fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_WEC_POLL_INTERVAL_MS));
        } else {
            fail_count++;
            ESP_LOGW(kTag, "getupdates failed %d times", fail_count);
            if (fail_count >= 10) {
                ui_.ShowError("微信消息通道异常", "正在自动重连，无需重新扫码");
            }
            vTaskDelay(pdMS_TO_TICKS(std::min(30000, 2000 + fail_count * 2000)));
        }
    }
}

bool WechatBot::DoGetUpdates() {
    if (local_curator_active_.load()) {
        cJSON* log = NewEventLog("wechat", "getupdates_skipped");
        cJSON_AddStringToObject(log, "reason", "local_curator_active");
        EmitEventLog(log);
        return true;
    }

    std::string body = "{\"base_info\":{\"channel_version\":\"2.0.1\"},\"get_updates_buf\":\"" +
                       cursor_ + "\"}";
    std::string url = base_url_ + "/ilink/bot/getupdates";
    HttpResponse response = HttpPost(url, body, true);
    if (response.IdleTimeout()) {
        // getupdates is a long-poll endpoint. No data before the client timeout
        // means there are simply no new messages yet.
        cJSON* log = NewEventLog("wechat", "getupdates_poll");
        cJSON_AddBoolToObject(log, "idle_timeout", true);
        cJSON_AddNumberToObject(log, "message_count", 0);
        cJSON_AddNumberToObject(log, "status", response.status);
        cJSON_AddNumberToObject(log, "esp_error", response.error);
        EmitEventLog(log);
        return true;
    }
    if (!response.Ok() || response.body.empty()) {
        ESP_LOGW(kTag, "getupdates request failed status=%d err=%s",
                 response.status, esp_err_to_name(response.error));
        cJSON* log = NewEventLog("wechat", "getupdates_poll");
        cJSON_AddBoolToObject(log, "ok_response", false);
        cJSON_AddNumberToObject(log, "status", response.status);
        cJSON_AddNumberToObject(log, "esp_error", response.error);
        cJSON_AddNumberToObject(log, "response_bytes", static_cast<double>(response.body.size()));
        EmitEventLog(log);
        return false;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) {
        ESP_LOGW(kTag, "getupdates JSON parse failed");
        cJSON* log = NewEventLog("wechat", "getupdates_poll");
        cJSON_AddBoolToObject(log, "json_ok", false);
        cJSON_AddNumberToObject(log, "response_bytes", static_cast<double>(response.body.size()));
        EmitEventLog(log);
        return true;
    }

    cJSON* ret_node = cJSON_GetObjectItem(root, "ret");
    cJSON* err_node = cJSON_GetObjectItem(root, "errcode");
    const int ret = cJSON_IsNumber(ret_node) ? ret_node->valueint : 0;
    const int errcode = cJSON_IsNumber(err_node) ? err_node->valueint : 0;
    cJSON* msgs = cJSON_GetObjectItem(root, "msgs");
    const int msg_count = cJSON_IsArray(msgs) ? cJSON_GetArraySize(msgs) : 0;
    cJSON* poll_log = NewEventLog("wechat", "getupdates_poll");
    cJSON_AddNumberToObject(poll_log, "ret", ret);
    cJSON_AddNumberToObject(poll_log, "errcode", errcode);
    cJSON_AddNumberToObject(poll_log, "message_count", msg_count);
    cJSON_AddNumberToObject(poll_log, "response_bytes", static_cast<double>(response.body.size()));
    EmitEventLog(poll_log);
    if (ret != 0 || errcode != 0) {
        ESP_LOGW(kTag, "getupdates ret=%d errcode=%d", ret, errcode);
        cJSON_Delete(root);
        return false;
    }

    cJSON* cursor_node = cJSON_GetObjectItem(root, "get_updates_buf");
    if (cJSON_IsString(cursor_node) && cursor_node->valuestring[0] != '\0') {
        cursor_ = cursor_node->valuestring;
        nvs_handle_t nvs = 0;
        if (nvs_open(WEC_WECHAT_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, kCursorKey, cursor_.c_str());
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    if (cJSON_IsArray(msgs)) {
        if (msg_count > 0) {
            cJSON* log = NewEventLog("wechat", "getupdates_messages");
            cJSON_AddNumberToObject(log, "count", msg_count);
            EmitEventLog(log);
        }
        for (int i = 0; i < msg_count; ++i) {
            cJSON* msg = cJSON_GetArrayItem(msgs, i);
            cJSON* message_type = cJSON_GetObjectItem(msg, "message_type");
            if (!cJSON_IsNumber(message_type) || message_type->valueint != 1) {
                continue;
            }

            const char* from_user = ItemString(msg, "from_user_id");
            const char* context_token = ItemString(msg, "context_token");
            if (context_token[0] != '\0') {
                last_context_token_ = context_token;
            }

            cJSON* item_list = cJSON_GetObjectItem(msg, "item_list");
            if (!cJSON_IsArray(item_list)) {
                continue;
            }
            const int item_count = cJSON_GetArraySize(item_list);
            for (int j = 0; j < item_count; ++j) {
                DispatchItem(cJSON_GetArrayItem(item_list, j), from_user);
            }
        }
    }

    cJSON_Delete(root);
    return true;
}

void WechatBot::DispatchItem(const cJSON* item, const char* from_user) {
    cJSON* type = cJSON_GetObjectItem(const_cast<cJSON*>(item), "type");
    if (!cJSON_IsNumber(type)) {
        return;
    }

    cJSON* log = NewEventLog("wechat", "item_received");
    cJSON_AddNumberToObject(log, "item_type", type->valueint);
    cJSON_AddBoolToObject(log, "has_sender", from_user && from_user[0] != '\0');
    EmitEventLog(log);

    switch (type->valueint) {
        case 1: {
            cJSON* text_item = cJSON_GetObjectItem(const_cast<cJSON*>(item), "text_item");
            const char* text = ItemString(text_item, "text");
            if (text[0] != '\0') {
                HandleText(from_user, text);
            }
            break;
        }
        case 2:
            HandleImage(cJSON_GetObjectItem(const_cast<cJSON*>(item), "image_item"), from_user);
            break;
        case 3: {
            cJSON* voice_item = cJSON_GetObjectItem(const_cast<cJSON*>(item), "voice_item");
            const char* transcript = ItemString(voice_item, "text");
            if (transcript[0] != '\0') {
                // WeChat already provides ASR text for many voice messages.
                // Treat it as content, but never as a device command.
                HandleText(from_user, transcript, false);
            } else {
                HandleUnsupported(from_user, "语音", "");
            }
            break;
        }
        case 4: {
            cJSON* file_item = cJSON_GetObjectItem(const_cast<cJSON*>(item), "file_item");
            HandleFile(file_item, from_user);
            break;
        }
        default:
            HandleUnsupported(from_user, "消息", "");
            break;
    }
}

void WechatBot::HandleText(const char* from_user, const char* text, bool allow_commands) {
    TouchActivity();

    cJSON* log = NewEventLog("wechat", allow_commands ? "text_received" : "voice_transcript_received");
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    cJSON_AddNumberToObject(log, "bytes", text ? static_cast<double>(std::strlen(text)) : 0);
    cJSON_AddBoolToObject(log, "commands_enabled", allow_commands);
    EmitEventLog(log);

    if (allow_commands && std::strcmp(text, "/next") == 0) {
        EmitEventLog(NewEventLog("command", "next"));
        if (!ui_.ShowNextNotePage(notes_.All(), notes_.CurrentIndex())) {
            RenderCurrentOrEmpty();
        }
        return;
    }
    if (allow_commands && std::strcmp(text, "/prev") == 0) {
        EmitEventLog(NewEventLog("command", "prev"));
        if (!ui_.ShowPreviousNotePage(notes_.All(), notes_.CurrentIndex())) {
            RenderCurrentOrEmpty();
        }
        return;
    }
    if (allow_commands && std::strcmp(text, "/clear") == 0) {
        EmitEventLog(NewEventLog("command", "clear_current"));
        notes_.ClearCurrent();
        RenderCurrentOrEmpty();
        return;
    }
    if (allow_commands && std::strcmp(text, "/clear all") == 0) {
        EmitEventLog(NewEventLog("command", "clear_all"));
        notes_.ClearAll();
        RenderCurrentOrEmpty();
        return;
    }
    std::string replacement = allow_commands ? ExtractReplacementText(text) : "";
    if (!replacement.empty()) {
        if (notes_.ReplaceCurrent(replacement, from_user ? from_user : "")) {
            cJSON* replace_log = NewEventLog("feedback", "replace_current_note");
            cJSON_AddStringToObject(replace_log, "preview", PreviewText(replacement.c_str()).c_str());
            EmitEventLog(replace_log);
            RenderCurrentOrEmpty();
            if (from_user && from_user[0] != '\0') {
                SendTextMessage(from_user, "已按你的反馈修改当前微笺。后续这类修改会用于优化整理规则。");
            }
        } else if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "当前没有可修改的微笺。请先发送一条想显示在屏上的内容。");
        }
        return;
    }

    if (!curator_url_.empty()) {
        cJSON* start_log = NewEventLog("curator", "start");
        cJSON_AddStringToObject(start_log, "preview", PreviewText(text).c_str());
        EmitEventLog(start_log);
        ui_.ShowThinking("正在整理微信内容", "红方块思考中...");
        if (!CurateText(from_user, text, !allow_commands)) {
            EmitEventLog(NewEventLog("curator", "failed"));
            ui_.ShowError("云端整理失败", "没有贴上屏幕，请稍后重试");
            if (from_user && from_user[0] != '\0') {
                SendTextMessage(from_user, "WeClawBot 云端整理暂不可用，这条消息没有贴到屏幕。");
            }
        }
        return;
    }

    EmitEventLog(NewEventLog("curator", "disabled_local_note"));
    notes_.Add(text, from_user ? from_user : "");
    RenderCurrentOrEmpty();

#if CONFIG_WEC_REPLY_ON_TEXT
    if (from_user && from_user[0] != '\0') {
        SendTextMessage(from_user, "已贴到 WeClawBot 屏幕。");
    }
#endif
}

void WechatBot::HandleImage(const cJSON* image_item, const char* from_user) {
    TouchActivity();

    const std::string cdn_url = MediaUrlFromItem(image_item);
    const std::string aes_key = AesKeyFromItem(image_item);
    const size_t byte_size = SizeFromItem(image_item);
    const cJSON* media = JsonObject(image_item, "media");

    cJSON* log = NewEventLog("wechat", "image_received");
    cJSON_AddBoolToObject(log, "has_image_item", image_item != nullptr);
    cJSON_AddStringToObject(log, "image_keys", JsonObjectKeys(image_item).c_str());
    cJSON_AddStringToObject(log, "media_keys", JsonObjectKeys(media).c_str());
    cJSON_AddBoolToObject(log, "has_media_url", !cdn_url.empty());
    cJSON_AddBoolToObject(log, "has_aes_key", !aes_key.empty());
    cJSON_AddNumberToObject(log, "aes_key_bytes", static_cast<double>(aes_key.size()));
    cJSON_AddNumberToObject(log, "byte_size", static_cast<double>(byte_size));
    EmitEventLog(log);

    if (!curator_url_.empty() && !cdn_url.empty() && !aes_key.empty()) {
        ui_.ShowThinking("收到图片", "红方块正在等待图文提取...");
        if (!CurateAttachment(from_user,
                              "wechat_image",
                              "",
                              cdn_url.c_str(),
                              aes_key.c_str(),
                              "image",
                              byte_size)) {
            EmitEventLog(NewEventLog("curator", "attachment_failed"));
            ui_.ShowError("图片整理失败", "请先发送图片中要上屏的文字");
            if (from_user && from_user[0] != '\0') {
                SendTextMessage(from_user, "图片我收到了，但云端图片整理暂不可用。请先把图片里要贴到屏幕的文字或摘要发来。");
            }
        }
        return;
    }

    ui_.ShowPlatformPrompt("图片", "");
    if (from_user && from_user[0] != '\0') {
        if (cdn_url.empty() || aes_key.empty()) {
            SendTextMessage(from_user, "图片我收到了，但这次没有拿到可解析的图片链接或密钥。请先发送图片里要贴到屏幕的文字。");
        } else {
            SendTextMessage(from_user, "图片我收到了。当前图片 OCR/视觉整理还在接入，请先发送图片里要贴到屏幕的文字或摘要。");
        }
    }
}

void WechatBot::HandleFile(const cJSON* file_item, const char* from_user) {
    TouchActivity();

    const std::string file_name = JsonString(file_item, "file_name");
    const std::string cdn_url = MediaUrlFromItem(file_item);
    const std::string aes_key = AesKeyFromItem(file_item);
    const size_t byte_size = SizeFromItem(file_item);
    const cJSON* media = JsonObject(file_item, "media");

    cJSON* log = NewEventLog("wechat", "file_received");
    cJSON_AddBoolToObject(log, "has_file_item", file_item != nullptr);
    cJSON_AddStringToObject(log, "file", PreviewText(file_name.c_str()).c_str());
    cJSON_AddStringToObject(log, "file_keys", JsonObjectKeys(file_item).c_str());
    cJSON_AddStringToObject(log, "media_keys", JsonObjectKeys(media).c_str());
    cJSON_AddBoolToObject(log, "has_media_url", !cdn_url.empty());
    cJSON_AddBoolToObject(log, "has_aes_key", !aes_key.empty());
    cJSON_AddNumberToObject(log, "aes_key_bytes", static_cast<double>(aes_key.size()));
    cJSON_AddNumberToObject(log, "byte_size", static_cast<double>(byte_size));
    EmitEventLog(log);

    if (!curator_url_.empty() && !cdn_url.empty() && !aes_key.empty()) {
        ui_.ShowThinking("收到文件", "红方块正在等待内容提取...");
        if (!CurateAttachment(from_user,
                              "wechat_file",
                              file_name.c_str(),
                              cdn_url.c_str(),
                              aes_key.c_str(),
                              "file",
                              byte_size)) {
            EmitEventLog(NewEventLog("curator", "attachment_failed"));
            ui_.ShowError("文件整理失败", "请先发送要上屏的摘要");
            if (from_user && from_user[0] != '\0') {
                SendTextMessage(from_user, "文件我收到了，但云端文件整理暂不可用。请先把要贴到屏幕的文字或摘要发来。");
            }
        }
        return;
    }

    HandleUnsupported(from_user, "文件", file_name.c_str());
}

void WechatBot::HandleUnsupported(const char* from_user, const char* kind, const char* file_name) {
    TouchActivity();
    cJSON* log = NewEventLog("wechat", "unsupported_item");
    cJSON_AddStringToObject(log, "kind", kind ? kind : "");
    cJSON_AddStringToObject(log, "file", PreviewText(file_name).c_str());
    EmitEventLog(log);
    ui_.ShowPlatformPrompt(kind, file_name);
    if (from_user && from_user[0] != '\0') {
        std::string reply = CONFIG_WEC_PLATFORM_HINT;
        reply += "。当前开源固件免费支持微信文本微笺。";
        SendTextMessage(from_user, reply);
    }
}

void WechatBot::AddAiConfig(cJSON* root) const {
    cJSON* ai = cJSON_CreateObject();
    cJSON_AddStringToObject(ai, "provider", ai_provider_.empty() ? "weclawbot" : ai_provider_.c_str());
    if (!ai_endpoint_.empty()) {
        cJSON_AddStringToObject(ai, "endpoint", ai_endpoint_.c_str());
    }
    if (!ai_model_.empty()) {
        cJSON_AddStringToObject(ai, "model", ai_model_.c_str());
    }
    if (!ai_token_.empty()) {
        cJSON_AddStringToObject(ai, "token", ai_token_.c_str());
    }
    cJSON_AddItemToObject(root, "ai", ai);
}

void WechatBot::AddScreenContext(cJSON* root) const {
    cJSON* screen = cJSON_CreateObject();
    const Note* current = notes_.Current();
    const std::string wechat_id = WechatId();
    cJSON_AddNumberToObject(screen, "version", 2);
    if (!wechat_id.empty()) {
        cJSON_AddStringToObject(screen, "wechat_id", wechat_id.c_str());
    }
    cJSON_AddBoolToObject(screen, "has_note", current != nullptr && !current->text.empty());
    cJSON_AddNumberToObject(screen, "note_count", static_cast<double>(notes_.Count()));
    cJSON_AddNumberToObject(screen, "note_index", static_cast<double>(notes_.CurrentIndex()));
    if (current) {
        cJSON* note = cJSON_CreateObject();
        cJSON_AddStringToObject(note, "text", current->text.c_str());
        cJSON_AddStringToObject(note, "canonical_text", current->text.c_str());
        cJSON_AddStringToObject(note, "from", current->from.c_str());
        cJSON_AddStringToObject(note, "time_label", current->time_label.c_str());
        cJSON_AddStringToObject(note, "revision", current->screen_revision.c_str());
        cJSON_AddStringToObject(note, "render_id", current->render_id.c_str());
        cJSON_AddNumberToObject(note, "page_count",
                                static_cast<double>(current->screen_frames.size()));
        cJSON_AddNumberToObject(note, "created_at", static_cast<double>(current->created_at));
        cJSON_AddItemToObject(screen, "current_note", note);
    }
    cJSON_AddItemToObject(root, "screen", screen);
}

void WechatBot::AddWechatIdentity(cJSON* root, const char* from_user) const {
    const std::string wechat_id = WechatId();
    const char* sender_ref = from_user ? from_user : "";
    if (!wechat_id.empty()) {
        cJSON_AddStringToObject(root, "wechat_id", wechat_id.c_str());
        cJSON_AddStringToObject(root, "screen_id", wechat_id.c_str());
    }
    cJSON_AddStringToObject(root, "sender_ref", sender_ref);

    cJSON* source = cJSON_CreateObject();
    if (!wechat_id.empty()) {
        cJSON_AddStringToObject(source, "wechat_id", wechat_id.c_str());
    }
    cJSON_AddStringToObject(source, "sender_ref", sender_ref);
    cJSON_AddItemToObject(root, "source", source);
}

bool WechatBot::CurateText(const char* from_user, const char* text, bool voice_transcript) {
    if (curator_url_.empty() || !text || text[0] == '\0') {
        return false;
    }

    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "esp32_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", voice_transcript ? "wechat_voice_transcript" : "wechat_text");
    cJSON_AddStringToObject(root, "text", text);
    AddWechatIdentity(root, from_user);
    AddScreenContext(root);
    AddAiConfig(root);

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    cJSON* request_log = NewEventLog("curator", "request");
    cJSON_AddStringToObject(request_log, "event_id", event_id);
    cJSON_AddBoolToObject(request_log, "voice_transcript", voice_transcript);
    cJSON_AddNumberToObject(request_log, "bytes", text ? static_cast<double>(std::strlen(text)) : 0);
    cJSON_AddBoolToObject(request_log, "has_current_note", notes_.Current() != nullptr);
    if (const Note* current = notes_.Current()) {
        cJSON_AddStringToObject(request_log, "screen_revision", current->screen_revision.c_str());
        cJSON_AddStringToObject(request_log, "render_id", current->render_id.c_str());
        cJSON_AddNumberToObject(request_log, "current_text_bytes",
                                static_cast<double>(current->text.size()));
    }
    cJSON_AddStringToObject(request_log, "ai_provider", ai_provider_.empty() ? "weclawbot" : ai_provider_.c_str());
    cJSON_AddBoolToObject(request_log, "ai_token_configured", !ai_token_.empty());
    EmitEventLog(request_log);

    HttpResponse response = HttpPost(curator_url_, body, false);
    cJSON* response_log = NewEventLog("curator", "http_response");
    cJSON_AddStringToObject(response_log, "event_id", event_id);
    cJSON_AddNumberToObject(response_log, "status", response.status);
    cJSON_AddNumberToObject(response_log, "esp_error", response.error);
    cJSON_AddBoolToObject(response_log, "overflow", response.overflow);
    cJSON_AddNumberToObject(response_log, "response_bytes", static_cast<double>(response.body.size()));
    EmitEventLog(response_log);
    if (!response.Ok() || response.body.empty()) {
        ESP_LOGW(kTag, "curator request failed status=%d err=%s",
                 response.status, esp_err_to_name(response.error));
        return false;
    }
    return ApplyCuratorDecision(from_user, std::move(response.body));
}

bool WechatBot::CurateAttachment(const char* from_user,
                                 const char* kind,
                                 const char* file_name,
                                 const char* cdn_url,
                                 const char* aes_key,
                                 const char* key_type,
                                 size_t byte_size) {
    if (curator_url_.empty() || !kind || !cdn_url || cdn_url[0] == '\0' ||
        !aes_key || aes_key[0] == '\0') {
        return false;
    }

    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "esp32_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", kind);
    cJSON_AddStringToObject(root, "filename", file_name ? file_name : "");
    AddWechatIdentity(root, from_user);
    AddScreenContext(root);

    cJSON* media = cJSON_CreateObject();
    cJSON_AddStringToObject(media, "cdn_url", cdn_url);
    cJSON_AddStringToObject(media, "url", cdn_url);
    cJSON_AddStringToObject(media, "aes_key", aes_key);
    cJSON_AddStringToObject(media, "key_type", key_type ? key_type : kind);
    cJSON_AddNumberToObject(media, "byte_size", static_cast<double>(byte_size));
    cJSON_AddItemToObject(root, "media", media);
    AddAiConfig(root);

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    cJSON* request_log = NewEventLog("curator", "attachment_request");
    cJSON_AddStringToObject(request_log, "event_id", event_id);
    cJSON_AddStringToObject(request_log, "kind", kind);
    cJSON_AddStringToObject(request_log, "file", PreviewText(file_name).c_str());
    cJSON_AddBoolToObject(request_log, "has_media_url", true);
    cJSON_AddBoolToObject(request_log, "has_aes_key", true);
    cJSON_AddBoolToObject(request_log, "has_current_note", notes_.Current() != nullptr);
    if (const Note* current = notes_.Current()) {
        cJSON_AddStringToObject(request_log, "screen_revision", current->screen_revision.c_str());
        cJSON_AddStringToObject(request_log, "render_id", current->render_id.c_str());
        cJSON_AddNumberToObject(request_log, "current_text_bytes",
                                static_cast<double>(current->text.size()));
    }
    cJSON_AddStringToObject(request_log, "ai_provider", ai_provider_.empty() ? "weclawbot" : ai_provider_.c_str());
    cJSON_AddBoolToObject(request_log, "ai_token_configured", !ai_token_.empty());
    EmitEventLog(request_log);

    HttpResponse response = HttpPost(curator_url_, body, false);
    cJSON* response_log = NewEventLog("curator", "http_response");
    cJSON_AddStringToObject(response_log, "event_id", event_id);
    cJSON_AddNumberToObject(response_log, "status", response.status);
    cJSON_AddNumberToObject(response_log, "esp_error", response.error);
    cJSON_AddBoolToObject(response_log, "overflow", response.overflow);
    cJSON_AddNumberToObject(response_log, "response_bytes", static_cast<double>(response.body.size()));
    EmitEventLog(response_log);
    if (!response.Ok() || response.body.empty()) {
        ESP_LOGW(kTag, "attachment curator request failed status=%d err=%s",
                 response.status, esp_err_to_name(response.error));
        return false;
    }
    return ApplyCuratorDecision(from_user, std::move(response.body));
}

bool WechatBot::ApplyCuratorDecision(const char* from_user, std::string response_body) {
    cJSON* root = cJSON_Parse(response_body.c_str());
    if (!root) {
        ESP_LOGW(kTag, "curator response is not JSON");
        return false;
    }
    std::string().swap(response_body);

    cJSON* inner = nullptr;
    cJSON* decision = root;
    cJSON* body_node = cJSON_GetObjectItem(root, "body");
    cJSON* action_node = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_node) && cJSON_IsString(body_node)) {
        inner = cJSON_Parse(body_node->valuestring);
        if (inner) {
            decision = inner;
        }
    }

    const char* action = ItemString(decision, "action");
    cJSON* decision_log = NewEventLog("curator", "decision");
    cJSON_AddStringToObject(decision_log, "event_id", ItemString(decision, "event_id"));
    cJSON_AddStringToObject(decision_log, "action", action);
    cJSON_AddBoolToObject(decision_log, "has_reply",
                          cJSON_IsString(cJSON_GetObjectItem(decision, "user_reply")));
    EmitEventLog(decision_log);

    const std::string recipient = from_user ? from_user : "";
    std::string reply_to_send;
    std::string preview_url_to_send;
    bool send_preview = false;
    bool success = false;

    if (std::strcmp(action, "set_idle_photo") == 0 ||
        std::strcmp(action, "replace_idle_photo") == 0) {
        cJSON* note = cJSON_GetObjectItem(decision, "note");
        cJSON* screen_state = cJSON_GetObjectItem(decision, "screen_state");
        std::string title = JsonString(note, "title");
        std::string body = JsonString(note, "body");
        if (body.empty()) {
            body = JsonString(note, "content");
        }
        std::string canonical_text = JsonString(screen_state, "canonical_text");
        std::string screen_revision = JsonString(screen_state, "revision");
        std::string render_id = JsonString(screen_state, "render_id");
        std::string reply = JsonString(decision, "user_reply");
        std::string preview_url = JsonString(decision, "screen_preview_url");

        std::vector<std::vector<uint8_t>> screen_frames;
        int screen_frame_width = 0;
        int screen_frame_height = 0;
        int screen_frame_stride = 0;
        const bool use_photo_frame =
            cJSON_IsArray(cJSON_GetObjectItem(decision, "screen_pages")) &&
            ExtractScreenFrames(decision, screen_frames, screen_frame_width,
                                screen_frame_height, screen_frame_stride,
                                WEC_RLCD_WIDTH, WEC_RLCD_HEIGHT);
        if (!use_photo_frame) {
            if (inner) cJSON_Delete(inner);
            cJSON_Delete(root);
            return false;
        }

        std::string photo_text = canonical_text;
        if (photo_text.empty()) {
            photo_text = body.empty() ? title : body;
        }
        if (photo_text.empty()) {
            photo_text = "照片相框";
        }
        cJSON* frame_log = NewEventLog("curator", "idle_photo_accepted");
        cJSON_AddNumberToObject(frame_log, "pages", static_cast<double>(screen_frames.size()));
        cJSON_AddNumberToObject(frame_log, "width", screen_frame_width);
        cJSON_AddNumberToObject(frame_log, "height", screen_frame_height);
        cJSON_AddNumberToObject(frame_log, "stride", screen_frame_stride);
        cJSON_AddStringToObject(frame_log, "revision", screen_revision.c_str());
        cJSON_AddStringToObject(frame_log, "render_id", render_id.c_str());
        EmitEventLog(frame_log);

        success = notes_.SetIdlePhoto(photo_text, from_user ? from_user : "",
                                      screen_revision, render_id,
                                      std::move(screen_frames),
                                      screen_frame_width, screen_frame_height,
                                      screen_frame_stride);
        RenderCurrentOrEmpty();
        if (reply.empty()) {
            reply = "已设为无微笺时的黑白相框。清空当前留言后会显示这张照片。";
        }
        reply_to_send = StripPreviewUrlFromReply(reply);
        preview_url_to_send = preview_url;
        send_preview = success && !preview_url_to_send.empty();
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_idle_photo_cleanup");
        if (success && !recipient.empty()) {
            SendTextMessage(recipient.c_str(), reply_to_send);
            if (send_preview) {
                SendPreviewImageMessage(recipient.c_str(), preview_url_to_send);
            }
        }
        return success;
    }

    if (std::strcmp(action, "create_note") == 0 ||
        std::strcmp(action, "update_note") == 0 ||
        std::strcmp(action, "replace_note") == 0 ||
        std::strcmp(action, "merge_note") == 0) {
        cJSON* note = cJSON_GetObjectItem(decision, "note");
        cJSON* screen_state = cJSON_GetObjectItem(decision, "screen_state");
        std::string title = JsonString(note, "title");
        std::string body = JsonString(note, "body");
        std::string canonical_text = JsonString(screen_state, "canonical_text");
        std::string screen_revision = JsonString(screen_state, "revision");
        std::string render_id = JsonString(screen_state, "render_id");
        std::string reply = JsonString(decision, "user_reply");
        std::string preview_url = JsonString(decision, "screen_preview_url");
        // Keep cloud previews enabled for WeChat, but render on-device through
        // LVGL-owned content-area bitmaps. Direct full-screen RLCD bitmap
        // pushes can fight later LVGL flushes and have shown all-black frames
        // on real hardware.
        const bool has_cloud_screen_pages =
            cJSON_IsArray(cJSON_GetObjectItem(decision, "screen_pages"));
        std::vector<std::vector<uint8_t>> screen_frames;
        int screen_frame_width = 0;
        int screen_frame_height = 0;
        int screen_frame_stride = 0;
        const bool use_cloud_screen_pages =
            has_cloud_screen_pages &&
            ExtractScreenFrames(decision, screen_frames, screen_frame_width,
                                screen_frame_height, screen_frame_stride,
                                WEC_CONTENT_BITMAP_WIDTH, WEC_CONTENT_BITMAP_HEIGHT);
        if (body.empty()) {
            body = JsonString(note, "content");
        }
        if (body.empty()) {
            if (inner) cJSON_Delete(inner);
            cJSON_Delete(root);
            return false;
        }

        std::string note_text;
        if (!title.empty() && title != "记事" && title != "备忘" && title != "提醒" &&
            title != "留言" && title != "微笺" && title != "今日提醒" &&
            body.find(title) == std::string::npos) {
            note_text = title;
            note_text += "\n";
        }
        note_text += body;
        if (!canonical_text.empty()) {
            note_text = canonical_text;
        }
        if (use_cloud_screen_pages) {
            cJSON* screen_log = NewEventLog("curator", "screen_pages_accepted");
            cJSON_AddNumberToObject(screen_log, "pages", static_cast<double>(screen_frames.size()));
            cJSON_AddNumberToObject(screen_log, "width", screen_frame_width);
            cJSON_AddNumberToObject(screen_log, "height", screen_frame_height);
            cJSON_AddNumberToObject(screen_log, "stride", screen_frame_stride);
            EmitEventLog(screen_log);
            cJSON_AddStringToObject(screen_log, "revision", screen_revision.c_str());
            cJSON_AddStringToObject(screen_log, "render_id", render_id.c_str());
            notes_.AddRendered(note_text, from_user ? from_user : "", screen_revision, render_id,
                               std::move(screen_frames),
                               screen_frame_width, screen_frame_height, screen_frame_stride);
        } else {
            if (has_cloud_screen_pages) {
                cJSON* screen_log = NewEventLog("curator", "screen_pages_ignored");
                cJSON_AddStringToObject(screen_log, "reason", "invalid_content_bitmap");
                EmitEventLog(screen_log);
            }
            notes_.Add(note_text, from_user ? from_user : "");
        }
        RenderCurrentOrEmpty();
        if (reply.empty()) {
            reply = "已整理成微笺并覆盖到屏幕。不合适可直接发“修改为...”，也可以发“/clear”清除。";
        }
        reply_to_send = StripPreviewUrlFromReply(reply);
        preview_url_to_send = preview_url;
        send_preview = !preview_url_to_send.empty();
        success = true;
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_decision_cleanup");
        if (!recipient.empty()) {
            SendTextMessage(recipient.c_str(), reply_to_send);
            if (send_preview) {
                SendPreviewImageMessage(recipient.c_str(), preview_url_to_send);
            }
        }
        return success;
    }

    if (std::strcmp(action, "clear_note") == 0) {
        std::string reply = JsonString(decision, "user_reply");
        notes_.ClearAll();
        RenderCurrentOrEmpty();
        if (reply.empty()) {
            reply = "已清除屏幕上的微笺。";
        }
        reply_to_send = reply;
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_decision_cleanup");
        if (!recipient.empty()) {
            SendTextMessage(recipient.c_str(), reply_to_send);
        }
        return true;
    }

    if (std::strcmp(action, "ignore") == 0) {
        std::string reply = JsonString(decision, "user_reply");
        reply_to_send = reply;
        RenderCurrentOrEmpty();
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_decision_cleanup");
        if (!reply_to_send.empty() && !recipient.empty()) {
            SendTextMessage(recipient.c_str(), reply_to_send);
        }
        return true;
    }

    if (std::strcmp(action, "clarify") == 0 ||
        std::strcmp(action, "service_required") == 0 ||
        std::strcmp(action, "reply_only") == 0 ||
        std::strcmp(action, "draft_note") == 0) {
        std::string reply = JsonString(decision, "user_reply");
        reply_to_send = reply;
        RenderCurrentOrEmpty();
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_decision_cleanup");
        if (!reply_to_send.empty() && !recipient.empty()) {
            SendTextMessage(recipient.c_str(), reply_to_send);
        }
        return true;
    }

    ESP_LOGW(kTag, "unsupported curator action: %s", action);
    if (inner) cJSON_Delete(inner);
    cJSON_Delete(root);
    return false;
}

bool WechatBot::SendTextMessage(const char* to_user, const std::string& text) {
    if (bot_token_.empty() || !to_user || to_user[0] == '\0') {
        EmitEventLog(NewEventLog("wechat", "sendmessage_skipped"));
        return false;
    }

    char client_id[40];
    std::snprintf(client_id, sizeof(client_id), "wec_%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", to_user);
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);
    cJSON_AddStringToObject(msg, "context_token", last_context_token_.c_str());

    cJSON* item_list = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "type", 1);
    cJSON* text_item = cJSON_CreateObject();
    cJSON_AddStringToObject(text_item, "text", text.c_str());
    cJSON_AddItemToObject(item, "text_item", text_item);
    cJSON_AddItemToArray(item_list, item);
    cJSON_AddItemToObject(msg, "item_list", item_list);
    cJSON_AddItemToObject(root, "msg", msg);
    AddBaseInfo(root);

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    HttpResponse response = HttpPost(base_url_ + "/ilink/bot/sendmessage",
                                     body,
                                     true,
                                     "application/json",
                                     false);
    cJSON* send_json = cJSON_Parse(response.body.c_str());
    const int ret = JsonInt(send_json, "ret", 0);
    const int errcode = JsonInt(send_json, "errcode", 0);
    if (send_json) {
        cJSON_Delete(send_json);
    }
    const bool ok = response.Ok() && !response.body.empty() && ret == 0 && errcode == 0;
    cJSON* log = NewEventLog("wechat", "sendmessage_result");
    cJSON_AddBoolToObject(log, "sent", ok);
    cJSON_AddNumberToObject(log, "status", response.status);
    cJSON_AddNumberToObject(log, "esp_error", response.error);
    cJSON_AddNumberToObject(log, "ret", ret);
    cJSON_AddNumberToObject(log, "errcode", errcode);
    cJSON_AddNumberToObject(log, "reply_bytes", static_cast<double>(text.size()));
    EmitEventLog(log);
    return ok;
}

bool WechatBot::SendPreviewImageMessage(const char* to_user, const std::string& preview_url) {
    if (bot_token_.empty() || !to_user || to_user[0] == '\0' || preview_url.empty()) {
        EmitEventLog(NewEventLog("wechat", "send_preview_image_skipped"));
        return false;
    }

    auto emit_retry_log = [](const char* stage, int attempt, const HttpResponse& response) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_retry");
        cJSON_AddStringToObject(log, "image_stage", stage);
        cJSON_AddNumberToObject(log, "attempt", attempt);
        cJSON_AddNumberToObject(log, "status", response.status);
        cJSON_AddNumberToObject(log, "esp_error", response.error);
        cJSON_AddBoolToObject(log, "will_retry", true);
        EmitEventLog(log);
    };

    std::string png = HttpGet(preview_url);
    if (png.empty()) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "download_preview");
        EmitEventLog(log);
        return false;
    }
    const size_t preview_size = png.size();

    unsigned char aes_key[16] = {};
    esp_fill_random(aes_key, sizeof(aes_key));
    const std::string aes_key_hex = HexEncode(aes_key, sizeof(aes_key));
    const std::string file_key = RandomHex(16);
    const std::string raw_md5 = Md5Hex(png);
    const size_t padded_size = AesEcbPaddedSize(png.size());
    if (aes_key_hex.empty() || file_key.empty() || raw_md5.empty()) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "prepare_crypto");
        EmitEventLog(log);
        return false;
    }

    cJSON* upload_root = cJSON_CreateObject();
    cJSON_AddStringToObject(upload_root, "filekey", file_key.c_str());
    cJSON_AddNumberToObject(upload_root, "media_type", 1);
    cJSON_AddStringToObject(upload_root, "to_user_id", to_user);
    cJSON_AddNumberToObject(upload_root, "rawsize", static_cast<double>(preview_size));
    cJSON_AddStringToObject(upload_root, "rawfilemd5", raw_md5.c_str());
    cJSON_AddNumberToObject(upload_root, "filesize", static_cast<double>(padded_size));
    cJSON_AddBoolToObject(upload_root, "no_need_thumb", true);
    cJSON_AddStringToObject(upload_root, "aeskey", aes_key_hex.c_str());
    AddBaseInfo(upload_root);

    char* upload_raw = cJSON_PrintUnformatted(upload_root);
    std::string upload_body = upload_raw ? upload_raw : "{}";
    if (upload_raw) {
        cJSON_free(upload_raw);
    }
    cJSON_Delete(upload_root);

    HttpResponse upload_ticket;
    for (int attempt = 1; attempt <= kPreviewImageRetryAttempts; ++attempt) {
        upload_ticket = HttpPost(base_url_ + "/ilink/bot/getuploadurl",
                                 upload_body,
                                 true,
                                 "application/json",
                                 false);
        if (upload_ticket.Ok() && !upload_ticket.body.empty()) {
            break;
        }
        if (attempt < kPreviewImageRetryAttempts) {
            emit_retry_log("getuploadurl", attempt, upload_ticket);
            vTaskDelay(pdMS_TO_TICKS(kPreviewImageRetryBaseDelayMs * attempt));
        }
    }
    if (!upload_ticket.Ok() || upload_ticket.body.empty()) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "getuploadurl");
        cJSON_AddNumberToObject(log, "attempts", kPreviewImageRetryAttempts);
        cJSON_AddNumberToObject(log, "status", upload_ticket.status);
        cJSON_AddNumberToObject(log, "esp_error", upload_ticket.error);
        cJSON_AddNumberToObject(log, "preview_bytes", static_cast<double>(preview_size));
        EmitEventLog(log);
        return false;
    }

    cJSON* upload_json = cJSON_Parse(upload_ticket.body.c_str());
    const int upload_ret = JsonInt(upload_json, "ret", 0);
    const int upload_errcode = JsonInt(upload_json, "errcode", 0);
    std::string upload_full_url = JsonString(upload_json, "upload_full_url");
    std::string upload_param = JsonString(upload_json, "upload_param");
    if (upload_json) {
        cJSON_Delete(upload_json);
    }
    if (upload_ret != 0 || upload_errcode != 0 || (upload_full_url.empty() && upload_param.empty())) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "parse_upload_param");
        cJSON_AddNumberToObject(log, "getupload_status", upload_ticket.status);
        cJSON_AddNumberToObject(log, "ret", upload_ret);
        cJSON_AddNumberToObject(log, "errcode", upload_errcode);
        cJSON_AddBoolToObject(log, "has_upload_full_url", !upload_full_url.empty());
        cJSON_AddBoolToObject(log, "has_upload_param", !upload_param.empty());
        EmitEventLog(log);
        return false;
    }

    std::string ciphertext = EncryptAesEcbPkcs7(png, aes_key);
    if (ciphertext.empty()) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "encrypt");
        EmitEventLog(log);
        return false;
    }

    std::string upload_url = !upload_full_url.empty()
                                 ? upload_full_url
                                 : std::string(kWechatCdnBaseUrl) +
                                       "/upload?encrypted_query_param=" + UrlEncode(upload_param) +
                                       "&filekey=" + UrlEncode(file_key);
    std::string().swap(png);
    std::string().swap(upload_body);
    std::string().swap(upload_ticket.body);
    std::string().swap(upload_full_url);
    std::string().swap(upload_param);

    cJSON* heap_log = NewEventLog("wechat", "send_preview_image_heap");
    cJSON_AddStringToObject(heap_log, "image_stage", "before_cdn_upload");
    cJSON_AddNumberToObject(heap_log, "free_heap", static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(heap_log, "largest_free_block", static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(heap_log, "preview_bytes", static_cast<double>(preview_size));
    cJSON_AddNumberToObject(heap_log, "cipher_bytes", static_cast<double>(ciphertext.size()));
    EmitEventLog(heap_log);

    HttpResponse cdn_upload;
    const std::string upload_proxy_url = CdnUploadProxyEndpoint(curator_url_);
    for (int attempt = 1; attempt <= kPreviewImageRetryAttempts; ++attempt) {
        cdn_upload = HttpPost(upload_proxy_url,
                              ciphertext,
                              false,
                              "application/octet-stream",
                              true,
                              "x-weclawbot-upload-target",
                              upload_url.c_str());
        if (cdn_upload.Ok() && !cdn_upload.encrypted_param.empty()) {
            break;
        }
        if (attempt < kPreviewImageRetryAttempts) {
            emit_retry_log("cdn_upload", attempt, cdn_upload);
            vTaskDelay(pdMS_TO_TICKS(kPreviewImageRetryBaseDelayMs * attempt));
        }
    }
    if (!cdn_upload.Ok() || cdn_upload.encrypted_param.empty()) {
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "cdn_upload");
        cJSON_AddNumberToObject(log, "attempts", kPreviewImageRetryAttempts);
        cJSON_AddNumberToObject(log, "status", cdn_upload.status);
        cJSON_AddNumberToObject(log, "esp_error", cdn_upload.error);
        cJSON_AddBoolToObject(log, "has_encrypted_param", !cdn_upload.encrypted_param.empty());
        cJSON_AddNumberToObject(log, "cipher_bytes", static_cast<double>(ciphertext.size()));
        cJSON_AddNumberToObject(log, "free_heap", static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
        cJSON_AddNumberToObject(log, "largest_free_block", static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        EmitEventLog(log);
        return false;
    }

    char client_id[48];
    std::snprintf(client_id, sizeof(client_id), "wec_img_%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", to_user);
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);
    cJSON_AddStringToObject(msg, "context_token", last_context_token_.c_str());

    cJSON* item_list = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "type", 2);
    cJSON* image_item = cJSON_CreateObject();
    cJSON* media = cJSON_CreateObject();
    cJSON_AddStringToObject(media, "encrypt_query_param", cdn_upload.encrypted_param.c_str());
    cJSON_AddStringToObject(media, "aes_key", Base64EncodeText(aes_key_hex).c_str());
    cJSON_AddNumberToObject(media, "encrypt_type", 1);
    cJSON_AddItemToObject(image_item, "media", media);
    cJSON_AddNumberToObject(image_item, "mid_size", static_cast<double>(ciphertext.size()));
    cJSON_AddItemToObject(item, "image_item", image_item);
    cJSON_AddItemToArray(item_list, item);
    cJSON_AddItemToObject(msg, "item_list", item_list);
    cJSON_AddItemToObject(root, "msg", msg);
    AddBaseInfo(root);

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    HttpResponse response;
    for (int attempt = 1; attempt <= kPreviewImageRetryAttempts; ++attempt) {
        response = HttpPost(base_url_ + "/ilink/bot/sendmessage",
                            body,
                            true,
                            "application/json",
                            false);
        cJSON* attempt_json = cJSON_Parse(response.body.c_str());
        const int attempt_ret = JsonInt(attempt_json, "ret", 0);
        const int attempt_errcode = JsonInt(attempt_json, "errcode", 0);
        if (attempt_json) {
            cJSON_Delete(attempt_json);
        }
        if (response.Ok() && !response.body.empty() && attempt_ret == 0 && attempt_errcode == 0) {
            break;
        }
        if (attempt < kPreviewImageRetryAttempts) {
            emit_retry_log("sendmessage", attempt, response);
            vTaskDelay(pdMS_TO_TICKS(kPreviewImageRetryBaseDelayMs * attempt));
        }
    }
    cJSON* send_json = cJSON_Parse(response.body.c_str());
    const int ret = JsonInt(send_json, "ret", 0);
    const int errcode = JsonInt(send_json, "errcode", 0);
    if (send_json) {
        cJSON_Delete(send_json);
    }
    const bool ok = response.Ok() && !response.body.empty() && ret == 0 && errcode == 0;
    cJSON* log = NewEventLog("wechat", "send_preview_image_result");
    cJSON_AddBoolToObject(log, "sent", ok);
    cJSON_AddStringToObject(log, "image_stage", "sendmessage");
    cJSON_AddNumberToObject(log, "attempts", kPreviewImageRetryAttempts);
    cJSON_AddNumberToObject(log, "status", response.status);
    cJSON_AddNumberToObject(log, "esp_error", response.error);
    cJSON_AddNumberToObject(log, "ret", ret);
    cJSON_AddNumberToObject(log, "errcode", errcode);
    cJSON_AddNumberToObject(log, "preview_bytes", static_cast<double>(preview_size));
    cJSON_AddNumberToObject(log, "cipher_bytes", static_cast<double>(ciphertext.size()));
    EmitEventLog(log);
    return ok;
}

std::string WechatBot::HttpGet(const std::string& url) {
    HttpResponse response;
    const bool locked = TakeHttpMutex(pdMS_TO_TICKS(WEC_HTTP_TIMEOUT_MS + 5000));
    if (!locked) {
        ESP_LOGW(kTag, "HTTP GET lock timeout url=%s", url.c_str());
        return "";
    }
    std::string uin = MakeWechatUin();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = WEC_HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.keep_alive_enable = false;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        GiveHttpMutex(locked);
        return "";
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "ilink-app-clientversion", "1");
    esp_http_client_set_header(client, "authorizationtype", "ilink_bot_token");
    esp_http_client_set_header(client, "authorization", "Bearer ");
    esp_http_client_set_header(client, "x-wechat-uin", uin.c_str());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    GiveHttpMutex(locked);
    if (err != ESP_OK || response.overflow || status >= 400) {
        ESP_LOGW(kTag, "GET failed status=%d err=%s url=%s", status, esp_err_to_name(err), url.c_str());
        return "";
    }
    return response.body;
}

WechatBot::HttpResponse WechatBot::HttpPost(const std::string& url,
                                            const std::string& body,
                                            bool with_auth,
                                            const char* content_type,
                                            bool include_wechat_uin,
                                            const char* extra_header_key,
                                            const char* extra_header_value) {
    HttpResponse response;
    if (url.empty()) {
        response.error = ESP_ERR_INVALID_ARG;
        return response;
    }
    const bool locked = TakeHttpMutex(pdMS_TO_TICKS(WEC_HTTP_TIMEOUT_MS + 5000));
    if (!locked) {
        response.error = ESP_ERR_TIMEOUT;
        return response;
    }
    std::string auth;
    std::string uin;
    if (with_auth) {
        auth = "Bearer " + bot_token_;
        if (include_wechat_uin) {
            uin = MakeWechatUin();
        }
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = WEC_HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.keep_alive_enable = false;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        response.error = ESP_ERR_NO_MEM;
        GiveHttpMutex(locked);
        return response;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type ? content_type : "application/json");
    esp_http_client_set_header(client, "accept", "*/*");
    if (extra_header_key && extra_header_value) {
        esp_http_client_set_header(client, extra_header_key, extra_header_value);
    }
    if (with_auth) {
        esp_http_client_set_header(client, "authorizationtype", "ilink_bot_token");
        esp_http_client_set_header(client, "authorization", auth.c_str());
        if (!uin.empty()) {
            esp_http_client_set_header(client, "x-wechat-uin", uin.c_str());
        }
    }
    esp_http_client_set_post_field(client, body.data(), body.size());

    response.error = esp_http_client_perform(client);
    response.status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    GiveHttpMutex(locked);
    if (!response.Ok() && !response.IdleTimeout()) {
        ESP_LOGW(kTag, "POST failed status=%d err=%s url=%s",
                 response.status, esp_err_to_name(response.error), url.c_str());
    }
    return response;
}

std::string WechatBot::MakeWechatUin() const {
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(esp_random()));

    unsigned char encoded[32] = {};
    size_t out_len = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len,
                          reinterpret_cast<const unsigned char*>(digits), std::strlen(digits));
    return std::string(reinterpret_cast<char*>(encoded), out_len);
}

std::string WechatBot::WechatId() const {
    if (bot_id_.empty()) {
        return "";
    }
    return "u_" + bot_id_;
}

void WechatBot::TouchActivity() {
    last_activity_ = std::time(nullptr);
}

void WechatBot::RenderCurrentOrEmpty() {
    if (!notes_.Empty()) {
        ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    } else if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
    } else {
        ui_.ShowEmptyNotes();
    }
}

void WechatBot::EnsureHttpMutex() {
    if (!http_mutex_) {
        http_mutex_ = xSemaphoreCreateMutex();
        if (!http_mutex_) {
            ESP_LOGW(kTag, "failed to create HTTP mutex");
        }
    }
}

bool WechatBot::TakeHttpMutex(TickType_t timeout_ticks) {
    EnsureHttpMutex();
    if (!http_mutex_) {
        return true;
    }
    return xSemaphoreTake(http_mutex_, timeout_ticks) == pdTRUE;
}

void WechatBot::GiveHttpMutex(bool locked) {
    if (locked && http_mutex_) {
        xSemaphoreGive(http_mutex_);
    }
}

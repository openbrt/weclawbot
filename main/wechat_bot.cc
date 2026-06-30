#include "wechat_bot.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <strings.h>
#include <sys/time.h>
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
#include <sdkconfig.h>

#if CONFIG_TINYUSB_CDC_ENABLED
#define WEC_HAS_TINYUSB_CDC 1
#include <tinyusb_cdc_acm.h>
#endif

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
constexpr char kLegacyCuratorUrl[] = "https://weclawbot.link/api/curator";
constexpr char kByoaCuratorUrl[] = "https://weclawbot.link/byoa";
constexpr char kAgentNamespace[] = "agent";
constexpr char kAgentDeviceIdKey[] = "device_id";
constexpr char kAgentMqttUrlKey[] = "mqtt_url";
constexpr char kAgentMqttUserKey[] = "mqtt_user";
constexpr char kAgentMqttPassKey[] = "mqtt_pass";
constexpr char kAgentMqttClientKey[] = "mqtt_client";
constexpr char kAgentMqttControlKey[] = "mqtt_control";
constexpr char kAgentMqttEventsKey[] = "mqtt_events";
constexpr char kAgentMqttStatusKey[] = "mqtt_status";
constexpr char kAgentMqttOwnerKey[] = "mqtt_owner";
constexpr char kOfficialAgentOwner[] = "official";
constexpr char kByoaAgentOwner[] = "byoa";
constexpr int kByoaBootstrapRetrySeconds = 20;
constexpr int kOfficialBootstrapRetrySeconds = 45;
constexpr int kAgentPublishRetryAttempts = 12;
constexpr int kAgentPublishRetryDelayMs = 500;
constexpr int kPendingWechatTextMax = 3;
constexpr int kPendingWechatTextTtlSeconds = 5 * 60;

void ConfigureLocalTimezone() {
    setenv("TZ", "CST-8", 1);
    tzset();
}

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

bool JsonBool(const cJSON* object, const char* key, bool fallback = false) {
    if (!object || !key) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(const_cast<cJSON*>(object), key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

int MonthFromHttpDate(const char* month) {
    static constexpr const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(month, kMonths[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

bool SetTimeFromHttpDate(const std::string& value) {
    char weekday[4] = {};
    char month[4] = {};
    char zone[4] = {};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.c_str(), "%3[^,], %d %3s %d %d:%d:%d %3s",
                    weekday, &day, month, &year, &hour, &minute, &second, zone) != 8 ||
        std::strcmp(zone, "GMT") != 0) {
        return false;
    }
    const int month_index = MonthFromHttpDate(month);
    if (month_index < 0 || year < 2020 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    std::tm utc = {};
    utc.tm_year = year - 1900;
    utc.tm_mon = month_index;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    utc.tm_isdst = 0;
    // The firmware timezone is CST-8. mktime interprets this GMT value as
    // local time, so compensate by eight hours before setting the UTC epoch.
    const std::time_t local_epoch = std::mktime(&utc);
    if (local_epoch < 0) {
        return false;
    }
    const timeval tv = {
        .tv_sec = local_epoch + 8 * 60 * 60,
        .tv_usec = 0,
    };
    return settimeofday(&tv, nullptr) == 0;
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

void PrintDebugLine(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
#if WEC_HAS_TINYUSB_CDC
    WriteCdcBytes(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    static constexpr uint8_t newline[] = "\n";
    WriteCdcBytes(newline, 1);
#endif
}

void PrintJsonLine(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    if (raw) {
        std::string line = "WEC:";
        line += raw;
        PrintDebugLine(line);
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

std::string TrimWhitespace(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string NormalizeCommandForMatch(std::string value) {
    value = TrimWhitespace(std::move(value));
    std::string out;
    for (size_t i = 0; i < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch < 0x80) {
            if (!std::isspace(ch) && std::strchr(".,;:!?\"'()[]{}<>", ch) == nullptr) {
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
            ++i;
            continue;
        }
        bool skipped = false;
        for (const char* punct : {"。", "，", "、", "；", "：", "！", "？", "“", "”", "‘", "’", "（", "）", "《", "》"}) {
            const size_t len = std::strlen(punct);
            if (value.compare(i, len, punct) == 0) {
                i += len;
                skipped = true;
                break;
            }
        }
        if (!skipped) {
            out.push_back(value[i++]);
            while (i < value.size() && (static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) {
                out.push_back(value[i++]);
            }
        }
    }
    return out;
}

bool IsHelpCommand(const std::string& input) {
    const std::string command = NormalizeCommandForMatch(input);
    return command == "/help" || command == "help" || command == "帮助" ||
           input == "官网" || input == "网站" || input == "设置" ||
           input == "配置" || input == "怎么配置";
}

bool IsClearPhotoCommand(const std::string& input) {
    const std::string command = NormalizeCommandForMatch(input);
    return command == "/clearphoto" || command == "/clearimage" ||
           command == "清除照片" || command == "清空照片" ||
           command == "删除照片" || command == "移除照片" ||
           command == "清除图片" || command == "清空图片" ||
           command == "删除图片" || command == "移除图片" ||
           command == "清除相框" || command == "清空相框" ||
           command == "清除照片屏" || command == "清空照片屏";
}

bool IsClearAllCommand(const std::string& input) {
    const std::string command = NormalizeCommandForMatch(input);
    return command == "/clearall" || command == "全清" ||
           command == "全部清空" || command == "全部清除" ||
           command == "清空全部" || command == "清除全部" ||
           command == "清空所有" || command == "清除所有" ||
           command == "清空所有屏幕" || command == "清除所有屏幕" ||
           command == "清空所有微笺" || command == "清除所有微笺" ||
           command == "清空所有留言" || command == "清除所有留言";
}

bool IsClearCurrentCommand(const std::string& input) {
    const std::string command = NormalizeCommandForMatch(input);
    return command == "/clear" || command == "清屏" ||
           command == "清空屏幕" || command == "清除屏幕" ||
           command == "删除屏幕" || command == "清掉屏幕" ||
           command == "清空当前屏幕" || command == "清除当前屏幕" ||
           command == "清空微笺" || command == "清除微笺" ||
           command == "删除微笺" || command == "清空当前微笺" ||
           command == "清除当前微笺" || command == "清空留言" ||
           command == "清除留言" || command == "删除留言" ||
           command == "清空当前留言" || command == "清除当前留言";
}

bool IsRecaptureCommand(const std::string& input) {
    const std::string command = NormalizeCommandForMatch(input);
    return command == "+" || command == "＋" || command == "贴上" ||
           command == "重新贴上" || command == "上屏" || command == "贴到屏幕";
}

std::string HelpReply() {
    return std::string("WeClawBot 入口：") + WEC_PRODUCT_URL +
           "\n\n也可以直接把设备 USB-C 连到电脑，打开 WECLAWBOT U 盘里的安装页或配置页。"
           "\n常用命令：/next 下一页，/prev 上一页，/clear 或“清屏”清除当前微笺，“修改为...”修正，+ 或“贴上”回捞上一条，/clear all 或“全清”清空全部微笺。";
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

bool IsFutureUtcIso8601(const std::string& value) {
    // Accept the UTC form emitted by both strftime and JavaScript Date:
    // YYYY-MM-DDTHH:MM:SSZ or YYYY-MM-DDTHH:MM:SS.sssZ. This keeps expiry
    // validation independent of device timezone configuration.
    const bool whole_seconds = value.size() == 20 && value[19] == 'Z';
    const bool milliseconds = value.size() == 24 && value[19] == '.' && value[23] == 'Z';
    if ((!whole_seconds && !milliseconds) || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        !std::isdigit(static_cast<unsigned char>(value[0]))) {
        return false;
    }
    for (size_t i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    if (milliseconds) {
        for (size_t i = 20; i < 23; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
                return false;
            }
        }
    }
    const int year = std::atoi(value.substr(0, 4).c_str());
    const int month = std::atoi(value.substr(5, 2).c_str());
    const int day = std::atoi(value.substr(8, 2).c_str());
    const int hour = std::atoi(value.substr(11, 2).c_str());
    const int minute = std::atoi(value.substr(14, 2).c_str());
    const int second = std::atoi(value.substr(17, 2).c_str());
    if (year < 2020 || month < 1 || month > 12 || day < 1 || hour > 23 ||
        minute > 59 || second > 59) {
        return false;
    }
    static constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int max_day = kMonthDays[month - 1] + (month == 2 && leap ? 1 : 0);
    if (day > max_day) {
        return false;
    }

    // Days from 1970-01-01, adapted from the public-domain civil-date formula.
    int adjusted_year = year - (month <= 2 ? 1 : 0);
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned shifted_month = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned doy = (153 * shifted_month + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    const int64_t timestamp = days * 86400 + hour * 3600 + minute * 60 + second;
    return timestamp > std::time(nullptr);
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

bool WechatBot::WechatIngressEnabled() const {
    return !IsByoaMode();
}

int WechatBot::AgentPairingSecondsLeft() const {
    const int64_t now = std::time(nullptr);
    return agent_pairing_expires_at_ > now
               ? static_cast<int>(agent_pairing_expires_at_ - now)
               : 0;
}

int WechatBot::AgentThinkingSecondsLeft() const {
    const int64_t now = std::time(nullptr);
    return agent_thinking_deadline_ > now
               ? static_cast<int>(agent_thinking_deadline_ - now)
               : 0;
}

const char* WechatBot::AgentTransportState() const {
    if (!agent_paired_) {
        return IsByoaMode() ? "awaiting_pairing" : "provisioning_required";
    }
    return agent_mqtt_.Connected() ? "online" : "reconnecting";
}

std::string WechatBot::AgentDeviceId() const {
    return AgentTransportDeviceId();
}

esp_err_t WechatBot::HttpEventHandler(esp_http_client_event_t* event) {
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (!response) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "x-encrypted-param") == 0) {
            response->encrypted_param = event->header_value;
        } else if (strcasecmp(event->header_key, "date") == 0) {
            response->date_header = event->header_value;
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

    if (IsByoaMode()) {
        RunByoaMode();
        return;
    }

    custom_agent_mode_ = false;
    EnsureAgentDeviceId();
    LoadAgentCredentials();
    if (agent_paired_) {
        agent_last_bootstrap_attempt_ = std::time(nullptr);
        agent_mqtt_.Connect(agent_credentials_);
    }
    StartAgentPump();

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

bool WechatBot::IsIlinkSessionInvalid(int http_status,
                                      int ret,
                                      int errcode,
                                      const cJSON* payload) const {
    (void)ret;
    (void)errcode;
    if (http_status == 401 || http_status == 403) {
        return true;
    }

    std::string message = FirstJsonString(payload, {
        "errmsg", "err_msg", "error_message", "message", "error", "detail",
    });
    if (message.empty()) {
        return false;
    }
    std::transform(message.begin(), message.end(), message.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    static constexpr const char* kInvalidMarkers[] = {
        "token invalid", "token expired", "token expire", "token revoked", "token logout",
        "session invalid", "session expired", "unauthorized", "not authorized",
        "not login", "not logged", "auth failed", "authentication failed",
        "token失效", "token过期", "token无效", "令牌失效", "令牌过期", "令牌无效",
        "登录失效", "登录过期", "登录无效", "会话失效", "会话过期", "会话无效",
        "重新扫码", "被挤下线", "其他设备", "其它设备", "别处登录",
    };
    for (const char* marker : kInvalidMarkers) {
        if (message.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void WechatBot::InvalidateIlinkSession(const char* operation,
                                       int http_status,
                                       int ret,
                                       int errcode,
                                       const cJSON* payload) {
    if (relogin_requested_.exchange(true)) {
        return;
    }
    const std::string message = FirstJsonString(payload, {
        "errmsg", "err_msg", "error_message", "message", "error", "detail",
    });
    ESP_LOGW(kTag, "iLink session invalid during %s status=%d ret=%d errcode=%d msg=%s",
             operation ? operation : "unknown", http_status, ret, errcode, message.c_str());
    cJSON* log = NewEventLog("wechat", "ilink_session_invalid");
    cJSON_AddStringToObject(log, "operation", operation ? operation : "unknown");
    cJSON_AddNumberToObject(log, "status", http_status);
    cJSON_AddNumberToObject(log, "ret", ret);
    cJSON_AddNumberToObject(log, "errcode", errcode);
    if (!message.empty()) {
        cJSON_AddStringToObject(log, "message", message.c_str());
    }
    EmitEventLog(log);

    ClearCredentials();
    bot_token_.clear();
    bot_id_.clear();
    qrcode_token_.clear();
    cursor_.clear();
    connected_ = false;
    login_state_ = LoginState::kStarting;
    qr_seconds_left_ = 0;
    ui_.ShowError("微信连接已失效", "已在另一台设备连接，正在返回二维码");
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

bool WechatBot::WechatLoopbackText(const char* text) {
    if (!text || text[0] == '\0') {
        return false;
    }
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("loopback_text", PreviewText(text).c_str());
        return false;
    }
    cJSON* log = NewEventLog("wechat", "loopback_text");
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    EmitEventLog(log);
    HandleText(nullptr, text, true);
    return true;
}

bool WechatBot::WechatLoopbackImage(const char* url) {
    if (!url || url[0] == '\0') {
        return false;
    }
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("loopback_image", url);
        return false;
    }
    cJSON* item = cJSON_CreateObject();
    cJSON* media = cJSON_CreateObject();
    cJSON_AddStringToObject(media, "full_url", url);
    cJSON_AddStringToObject(media, "aes_key", "00000000000000000000000000000000");
    cJSON_AddStringToObject(media, "key_type", "image");
    cJSON_AddItemToObject(item, "media", media);
    cJSON* log = NewEventLog("wechat", "loopback_image");
    cJSON_AddStringToObject(log, "url", url);
    EmitEventLog(log);
    HandleImage(item, nullptr);
    cJSON_Delete(item);
    return true;
}

void WechatBot::SyncTime() {
    ConfigureLocalTimezone();

    std::time_t existing = 0;
    std::time(&existing);
    std::tm existing_tm = {};
    localtime_r(&existing, &existing_tm);
    if (existing_tm.tm_year >= 120) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &existing_tm);
        std::printf("WEC:{\"scope\":\"time\",\"stage\":\"time_valid\","
                    "\"local\":\"%s\",\"ok\":true,\"type\":\"event\"}\n", buf);
        ui_.Tick();
        return;
    }

    static std::atomic_bool initialized{false};
    static std::atomic_bool syncing{false};
    static std::atomic_uint ntp_server_index{0};
    if (syncing.exchange(true)) {
        return;
    }

    static constexpr const char* kNtpServers[] = {
        "ntp.aliyun.com",
        "ntp.tencent.com",
        "time.cloudflare.com",
    };
    const char* ntp_server = kNtpServers[ntp_server_index.fetch_add(1) % 3];
    esp_err_t err = ESP_OK;
    if (!initialized.load()) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
        config.smooth_sync = false;
        config.start = false;
        err = esp_netif_sntp_init(&config);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            initialized = true;
            err = ESP_OK;
        }
    } else {
        esp_sntp_setservername(0, ntp_server);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(err));
        syncing = false;
        return;
    }

    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SNTP start failed: %s", esp_err_to_name(err));
        syncing = false;
        return;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    std::time_t now = 0;
    std::time(&now);
    std::tm tm = {};
    localtime_r(&now, &tm);
    bool synced = err == ESP_OK && tm.tm_year >= 120;
    if (!synced) {
        std::string http_date;
        const std::string health = HttpGet(WEC_PRODUCT_URL "/api/health", &http_date);
        if (!health.empty() && SetTimeFromHttpDate(http_date)) {
            std::time(&now);
            localtime_r(&now, &tm);
            synced = tm.tm_year >= 120;
            if (synced) {
                ESP_LOGI(kTag, "Time synced from HTTPS Date: %s", http_date.c_str());
            }
        }
    }
    if (synced) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ESP_LOGI(kTag, "Time synced: %s", buf);
        std::printf("WEC:{\"scope\":\"time\",\"stage\":\"sntp_sync\","
                    "\"local\":\"%s\",\"ok\":true,\"type\":\"event\"}\n", buf);
    } else {
        ESP_LOGW(kTag, "Time sync timed out: %s", esp_err_to_name(err));
        std::printf("WEC:{\"scope\":\"time\",\"stage\":\"sntp_sync\","
                    "\"error\":\"%s\",\"ok\":false,\"type\":\"event\"}\n",
                    esp_err_to_name(err));
    }
    syncing = false;
    ui_.Tick();
}

void WechatBot::RetryTimeSync() {
    SyncTime();
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

    // The former public path stays valid on the server, but migrate an
    // untouched historical default so the configuration page and firmware use
    // the one stable gateway address going forward.
    if (curator_url_.empty() || curator_url_ == kLegacyCuratorUrl) {
        curator_url_ = CONFIG_WEC_CURATOR_URL;
        // Persist the migration as well as applying it in memory. Otherwise
        // the serial configurator can keep showing a retired URL even though
        // the firmware is already using the new official gateway.
        if (nvs_open(WEC_CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, kCuratorUrlKey, curator_url_.c_str());
            nvs_commit(nvs);
            nvs_close(nvs);
        }
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

bool WechatBot::IsByoaMode() const {
    return curator_url_ == kByoaCuratorUrl;
}

void WechatBot::LogWechatIgnored(const char* kind, const char* detail) {
    cJSON* log = NewEventLog("wechat", "ingress_ignored");
    cJSON_AddStringToObject(log, "reason", "byoa_agent_owns_screen");
    cJSON_AddStringToObject(log, "kind", kind ? kind : "");
    if (detail && detail[0] != '\0') {
        cJSON_AddStringToObject(log, "detail", detail);
    }
    EmitEventLog(log);
}

void WechatBot::ClearPendingWechatTextEvents() {
    pending_agent_next_flush_at_ = 0;
    EnsurePendingAgentMutex();
    if (!pending_agent_mutex_) {
        pending_wechat_texts_.clear();
        return;
    }
    if (xSemaphoreTake(pending_agent_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
        pending_wechat_texts_.clear();
        xSemaphoreGive(pending_agent_mutex_);
    }
}

std::string WechatBot::AgentStorageOwner() const {
    return IsByoaMode() ? kByoaAgentOwner : kOfficialAgentOwner;
}

std::string WechatBot::AgentTransportDeviceId() const {
    if (agent_device_id_.empty()) {
        return "";
    }
    if (IsByoaMode()) {
        return agent_device_id_;
    }
    return std::string("off_") + agent_device_id_;
}

void WechatBot::EnsureAgentDeviceId() {
    nvs_handle_t nvs = 0;
    if (nvs_open(kAgentNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    size_t length = 0;
    if (nvs_get_str(nvs, kAgentDeviceIdKey, nullptr, &length) == ESP_OK && length > 1) {
        agent_device_id_.assign(length, '\0');
        if (nvs_get_str(nvs, kAgentDeviceIdKey, agent_device_id_.data(), &length) == ESP_OK &&
            !agent_device_id_.empty() && agent_device_id_.back() == '\0') {
            agent_device_id_.pop_back();
        }
    }
    if (agent_device_id_.empty()) {
        agent_device_id_ = "wec_" + RandomHex(8);
        nvs_set_str(nvs, kAgentDeviceIdKey, agent_device_id_.c_str());
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

void WechatBot::LoadAgentCredentials() {
    agent_credentials_ = {};
    agent_paired_ = false;
    std::string owner;
    nvs_handle_t nvs = 0;
    if (nvs_open(kAgentNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    auto read = [nvs](const char* key, std::string* value) {
        size_t length = 0;
        if (!value || nvs_get_str(nvs, key, nullptr, &length) != ESP_OK || length <= 1) {
            return;
        }
        value->assign(length, '\0');
        if (nvs_get_str(nvs, key, value->data(), &length) == ESP_OK &&
            !value->empty() && value->back() == '\0') {
            value->pop_back();
        }
    };
    read(kAgentMqttUrlKey, &agent_credentials_.url);
    read(kAgentMqttUserKey, &agent_credentials_.username);
    read(kAgentMqttPassKey, &agent_credentials_.password);
    read(kAgentMqttClientKey, &agent_credentials_.client_id);
    read(kAgentMqttControlKey, &agent_credentials_.control_topic);
    read(kAgentMqttEventsKey, &agent_credentials_.events_topic);
    read(kAgentMqttStatusKey, &agent_credentials_.status_topic);
    read(kAgentMqttOwnerKey, &owner);
    nvs_close(nvs);
    const std::string expected_owner = AgentStorageOwner();
    const bool legacy_byoa_credentials = owner.empty() && IsByoaMode();
    if (!legacy_byoa_credentials && owner != expected_owner) {
        agent_credentials_ = {};
        return;
    }
    agent_paired_ = agent_credentials_.Valid() && !agent_credentials_.events_topic.empty() &&
                    !agent_credentials_.status_topic.empty();
}

void WechatBot::SaveAgentCredentials() {
    if (!agent_credentials_.Valid()) {
        return;
    }
    nvs_handle_t nvs = 0;
    if (nvs_open(kAgentNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, kAgentMqttUrlKey, agent_credentials_.url.c_str());
    nvs_set_str(nvs, kAgentMqttUserKey, agent_credentials_.username.c_str());
    nvs_set_str(nvs, kAgentMqttPassKey, agent_credentials_.password.c_str());
    nvs_set_str(nvs, kAgentMqttClientKey, agent_credentials_.client_id.c_str());
    nvs_set_str(nvs, kAgentMqttControlKey, agent_credentials_.control_topic.c_str());
    nvs_set_str(nvs, kAgentMqttEventsKey, agent_credentials_.events_topic.c_str());
    nvs_set_str(nvs, kAgentMqttStatusKey, agent_credentials_.status_topic.c_str());
    const std::string owner = AgentStorageOwner();
    nvs_set_str(nvs, kAgentMqttOwnerKey, owner.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
}

void WechatBot::ClearAgentCredentials() {
    agent_mqtt_.Disconnect();
    agent_credentials_ = {};
    agent_paired_ = false;
    agent_online_announced_ = false;
    nvs_handle_t nvs = 0;
    if (nvs_open(kAgentNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, kAgentMqttUrlKey);
        nvs_erase_key(nvs, kAgentMqttUserKey);
        nvs_erase_key(nvs, kAgentMqttPassKey);
        nvs_erase_key(nvs, kAgentMqttClientKey);
        nvs_erase_key(nvs, kAgentMqttControlKey);
        nvs_erase_key(nvs, kAgentMqttEventsKey);
        nvs_erase_key(nvs, kAgentMqttStatusKey);
        nvs_erase_key(nvs, kAgentMqttOwnerKey);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool WechatBot::ParseAgentMqttCredentials(const cJSON* mqtt,
                                          AgentMqtt::Credentials* credentials) const {
    if (!mqtt || !credentials) {
        return false;
    }
    cJSON* topics = cJSON_GetObjectItem(const_cast<cJSON*>(mqtt), "topics");
    credentials->url = JsonString(mqtt, "url");
    credentials->username = JsonString(mqtt, "username");
    credentials->password = JsonString(mqtt, "password");
    credentials->client_id = JsonString(mqtt, "client_id");
    credentials->control_topic = JsonString(topics, "control");
    credentials->events_topic = JsonString(topics, "events");
    credentials->status_topic = JsonString(topics, "status");
    return credentials->Valid();
}

bool WechatBot::BeginOfficialAgentSession() {
    if (curator_url_.empty()) {
        return false;
    }
    EnsureAgentDeviceId();
    const std::string device_id = AgentTransportDeviceId();
    if (device_id.empty()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.gateway.v1");
    cJSON_AddStringToObject(root, "operation", "bootstrap");
    cJSON_AddStringToObject(root, "mode", "official");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "firmware", WEC_FIRMWARE_VERSION);
    AddDeviceContext(root);
    char* raw = cJSON_PrintUnformatted(root);
    const std::string body = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);

    agent_last_bootstrap_attempt_ = std::time(nullptr);
    HttpResponse response = HttpPost(curator_url_, body, false);
    if (!response.Ok() || response.body.empty()) {
        cJSON* log = NewEventLog("agent", "official_bootstrap_failed");
        cJSON_AddNumberToObject(log, "status", response.status);
        cJSON_AddNumberToObject(log, "esp_error", response.error);
        EmitEventLog(log);
        return false;
    }

    cJSON* payload = cJSON_Parse(response.body.c_str());
    cJSON* mqtt = payload ? cJSON_GetObjectItem(payload, "mqtt") : nullptr;
    AgentMqtt::Credentials credentials;
    const bool valid = payload && JsonString(payload, "schema") == "weclawbot.gateway.bootstrap.v1" &&
                       ParseAgentMqttCredentials(mqtt, &credentials);
    if (payload) cJSON_Delete(payload);
    if (!valid) {
        EmitEventLog(NewEventLog("agent", "official_bootstrap_invalid"));
        return false;
    }

    agent_credentials_ = std::move(credentials);
    agent_paired_ = true;
    agent_online_announced_ = false;
    SaveAgentCredentials();
    EmitEventLog(NewEventLog("agent", "official_bootstrap_ok"));
    return true;
}

bool WechatBot::BeginByoaPairing() {
    EnsureAgentDeviceId();
    if (agent_device_id_.empty()) {
        ui_.ShowError("智能体配对失败", "无法创建设备标识");
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.byoa.v1");
    cJSON_AddStringToObject(root, "operation", "bootstrap");
    cJSON_AddStringToObject(root, "device_id", agent_device_id_.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    const std::string body = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);

    agent_last_bootstrap_attempt_ = std::time(nullptr);
    HttpResponse response = HttpPost(curator_url_, body, false);
    if (!response.Ok() || response.body.empty()) {
        ui_.ShowError("智能体配对失败", "正在自动重试");
        return false;
    }
    cJSON* payload = cJSON_Parse(response.body.c_str());
    cJSON* pairing = payload ? cJSON_GetObjectItem(payload, "pairing") : nullptr;
    cJSON* temporary_mqtt = payload ? cJSON_GetObjectItem(payload, "bootstrap_mqtt") : nullptr;
    AgentMqtt::Credentials temporary;
    temporary.url = JsonString(temporary_mqtt, "url");
    temporary.username = JsonString(temporary_mqtt, "username");
    temporary.password = JsonString(temporary_mqtt, "password");
    temporary.client_id = JsonString(temporary_mqtt, "client_id");
    cJSON* topics = temporary_mqtt ? cJSON_GetObjectItem(temporary_mqtt, "topics") : nullptr;
    temporary.control_topic = JsonString(topics, "pairing");
    agent_pairing_code_ = JsonString(pairing, "code");
    if (payload) cJSON_Delete(payload);
    if (agent_pairing_code_.size() != 6 || !temporary.Valid() || !agent_mqtt_.Connect(temporary)) {
        ui_.ShowError("智能体配对失败", "正在自动重试");
        return false;
    }
    agent_pairing_expires_at_ = std::time(nullptr) + 10 * 60;
    ui_.ShowAgentPairingCode(agent_pairing_code_.c_str(), AgentPairingSecondsLeft());
    ESP_LOGI(kTag, "BYOA code shown for device %s", agent_device_id_.c_str());
    return true;
}

void WechatBot::RunByoaMode() {
    custom_agent_mode_ = true;
    connected_ = false;
    login_state_ = LoginState::kStarting;
    qr_seconds_left_ = 0;
    ClearPendingWechatTextEvents();
    EnsureAgentDeviceId();
    LoadAgentCredentials();
    if (agent_paired_) {
        if (agent_mqtt_.Connect(agent_credentials_)) {
            RenderAgentCurrentOrDashboard();
        } else {
            ui_.ShowError("智能体连接失败", "正在自动重连");
        }
    } else {
        BeginByoaPairing();
    }

    while (true) {
        ProcessAgentMessages();
        const int64_t now = std::time(nullptr);
        const bool pairing_code_still_valid = !agent_pairing_code_.empty() &&
                                              agent_pairing_expires_at_ > now;
        if (!agent_paired_ && !pairing_code_still_valid &&
            (agent_last_bootstrap_attempt_ == 0 || now - agent_last_bootstrap_attempt_ >= kByoaBootstrapRetrySeconds)) {
            BeginByoaPairing();
        }
        MaintainAgentMqtt();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void WechatBot::StartAgentPump() {
    if (agent_pump_task_) {
        return;
    }
    constexpr uint32_t kAgentPumpStack = 6144;
    const BaseType_t ok = xTaskCreate(AgentPumpTask, "agent_pump",
                                      kAgentPumpStack, this, 4,
                                      &agent_pump_task_);
    cJSON* log = NewEventLog("task", "create");
    cJSON_AddStringToObject(log, "name", "agent_pump");
    cJSON_AddNumberToObject(log, "stack", kAgentPumpStack);
    cJSON_AddBoolToObject(log, "ok", ok == pdPASS);
    cJSON_AddNumberToObject(log, "internal_free",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(log, "internal_largest",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    EmitEventLog(log);
    if (ok != pdPASS) {
        agent_pump_task_ = nullptr;
    }
}

void WechatBot::AgentPumpTask(void* arg) {
    auto* self = static_cast<WechatBot*>(arg);
    while (self) {
        self->ProcessAgentMessages();
        self->MaintainAgentMqtt();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(nullptr);
}

void WechatBot::MaintainAgentMqtt() {
    const int64_t now = std::time(nullptr);
    if (!IsByoaMode() && (!agent_paired_ || !agent_mqtt_.Connected()) &&
        (agent_last_bootstrap_attempt_ == 0 ||
         now - agent_last_bootstrap_attempt_ >= kOfficialBootstrapRetrySeconds)) {
        if (BeginOfficialAgentSession()) {
            agent_mqtt_.Connect(agent_credentials_);
        } else if (agent_paired_ && !agent_mqtt_.Connected()) {
            agent_mqtt_.Connect(agent_credentials_);
        }
    }
    if (agent_paired_ && agent_mqtt_.Connected() && !agent_online_announced_) {
        PublishAgentEvent("online");
        PublishAgentStatus("online");
        agent_online_announced_ = true;
    } else if (agent_paired_ && !agent_mqtt_.Connected()) {
        // esp-mqtt reconnects by itself. Publish a fresh context whenever
        // that live session comes back instead of assuming it is permanent.
        agent_online_announced_ = false;
    }
    if (!IsByoaMode()) {
        FlushPendingWechatTextEvents();
    }
    if (agent_thinking_deadline_ > 0 && now >= agent_thinking_deadline_) {
        const std::string expired_correlation_id = agent_activity_correlation_id_;
        RenderAgentCurrentOrDashboard();
        PublishAgentStatus("activity_expired", nullptr,
                           expired_correlation_id.empty() ? nullptr : expired_correlation_id.c_str());
    }
}

void WechatBot::ProcessAgentMessages() {
    AgentMqtt::Message message;
    while (agent_mqtt_.TakeMessage(&message)) {
        if (!agent_paired_) {
            cJSON* root = cJSON_Parse(message.payload.c_str());
            cJSON* mqtt = root ? cJSON_GetObjectItem(root, "mqtt") : nullptr;
            AgentMqtt::Credentials permanent;
            const bool valid = root && JsonString(root, "schema") == "weclawbot.byoa.device_credentials.v1" &&
                               ParseAgentMqttCredentials(mqtt, &permanent);
            if (root) cJSON_Delete(root);
            if (!valid) {
                continue;
            }
            agent_mqtt_.Disconnect();
            agent_credentials_ = std::move(permanent);
            SaveAgentCredentials();
            agent_paired_ = true;
            agent_pairing_code_.clear();
            agent_pairing_expires_at_ = 0;
            agent_online_announced_ = false;
            if (agent_mqtt_.Connect(agent_credentials_)) {
                RenderAgentCurrentOrDashboard();
                ESP_LOGI(kTag, "BYOA agent paired");
            } else {
                ui_.ShowError("智能体连接失败", "已配对，正在自动重连");
            }
            continue;
        }
        if (message.topic == agent_credentials_.control_topic) {
            HandleAgentControl(message.payload);
        }
    }
}

void WechatBot::HandleAgentControl(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root || JsonString(root, "schema") != "weclawbot.control.v1") {
        if (root) cJSON_Delete(root);
        PublishAgentStatus("rejected", "invalid_control");
        return;
    }
    const std::string kind = JsonString(root, "kind");
    if (kind == "activity") {
        cJSON* activity = cJSON_GetObjectItem(root, "activity");
        const std::string schema = JsonString(activity, "schema");
        const std::string state = JsonString(activity, "state");
        const std::string correlation_id = JsonString(activity, "correlation_id");
        const int ttl = JsonInt(activity, "ttl_seconds", 0);
        if (schema != "weclawbot.activity.v1" || (state != "thinking" && state != "idle") ||
            correlation_id.empty() || correlation_id.size() > 80) {
            cJSON_Delete(root);
            PublishAgentStatus("rejected", "invalid_activity");
            return;
        }
        if (state == "thinking") {
            if (ttl < 5 || ttl > 120) {
                cJSON_Delete(root);
                PublishAgentStatus("rejected", "invalid_activity_ttl");
                return;
            }
            ShowThinkingWithTimeout("智能体思考中", "AI 正在处理", ttl, correlation_id.c_str());
            PublishAgentStatus("activity", "thinking", correlation_id.c_str());
        } else {
            if (!agent_activity_correlation_id_.empty() &&
                agent_activity_correlation_id_ != correlation_id) {
                cJSON_Delete(root);
                PublishAgentStatus("rejected", "activity_correlation_mismatch",
                                   correlation_id.c_str());
                return;
            }
            RenderAgentCurrentOrDashboard();
            PublishAgentStatus("activity", "idle", correlation_id.c_str());
        }
        cJSON_Delete(root);
        return;
    }
    if (kind == "screen_document") {
        ApplyAgentScreenDocument(cJSON_GetObjectItem(root, "document"));
        cJSON_Delete(root);
        return;
    }
    if (kind == "screen_intent") {
        HandleAgentScreenIntent(cJSON_GetObjectItem(root, "intent"));
        cJSON_Delete(root);
        return;
    }
    if (kind == "wechat_reply") {
        HandleAgentWechatReply(root);
        cJSON_Delete(root);
        return;
    }
    if (kind == "screen_clear") {
        HandleAgentScreenClear(root);
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);
    PublishAgentStatus("rejected", "unknown_control_kind");
}

bool WechatBot::HandleAgentScreenIntent(const cJSON* intent) {
    if (!intent || JsonString(intent, "schema") != "weclawbot.screen_intent.v1") {
        PublishAgentStatus("rejected", "invalid_screen_intent");
        return false;
    }
    cJSON* origin = cJSON_GetObjectItem(const_cast<cJSON*>(intent), "origin");
    cJSON* document = cJSON_GetObjectItem(const_cast<cJSON*>(intent), "document");
    const std::string reply_target = JsonString(origin, "reply_target");
    std::string reply = JsonString(intent, "wechat_reply");
    reply = StripPreviewUrlFromReply(reply);

    const bool has_document = cJSON_IsObject(document);
    bool applied = true;
    if (has_document) {
        applied = ApplyAgentScreenDocument(document);
    }
    if (!applied) {
        if (!has_document) {
            RenderAgentCurrentOrDashboard();
        }
        return false;
    }
    if (!reply.empty() && !reply_target.empty() && !WechatIngressEnabled()) {
        LogWechatIgnored("reply", "screen_intent");
        if (!has_document) {
            RenderAgentCurrentOrDashboard();
            PublishAgentStatus("rejected", "wechat_disabled_in_byoa");
            return false;
        }
    } else if (!reply.empty() && !reply_target.empty()) {
        if (SendTextMessage(reply_target.c_str(), reply)) {
            if (!has_document) {
                RenderAgentCurrentOrDashboard();
            }
            PublishAgentStatus("reply_sent");
        } else {
            if (!has_document) {
                RenderAgentCurrentOrDashboard();
            }
            PublishAgentStatus("rejected", "wechat_reply_failed");
            return false;
        }
    } else if (!has_document) {
        RenderAgentCurrentOrDashboard();
        PublishAgentStatus("intent_handled", "no_document_or_reply");
    }
    return true;
}

bool WechatBot::HandleAgentWechatReply(const cJSON* root) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("reply", "wechat_reply_control");
        RenderAgentCurrentOrDashboard();
        PublishAgentStatus("rejected", "wechat_disabled_in_byoa");
        return false;
    }
    cJSON* origin = cJSON_GetObjectItem(const_cast<cJSON*>(root), "origin");
    std::string reply_target = JsonString(root, "reply_target");
    if (reply_target.empty()) {
        reply_target = JsonString(origin, "reply_target");
    }
    std::string reply = JsonString(root, "text");
    if (reply.empty()) {
        reply = JsonString(root, "wechat_reply");
    }
    reply = StripPreviewUrlFromReply(reply);
    if (reply_target.empty() || reply.empty()) {
        RenderAgentCurrentOrDashboard();
        PublishAgentStatus("rejected", "invalid_wechat_reply");
        return false;
    }
    if (!SendTextMessage(reply_target.c_str(), reply)) {
        RenderAgentCurrentOrDashboard();
        PublishAgentStatus("rejected", "wechat_reply_failed");
        return false;
    }
    RenderAgentCurrentOrDashboard();
    PublishAgentStatus("reply_sent");
    return true;
}

bool WechatBot::HandleAgentScreenClear(const cJSON* root) {
    const std::string target = JsonString(root, "target", "note");
    if (target == "photo" || target == "idle_photo") {
        notes_.ClearIdlePhoto();
    } else {
        notes_.ClearAll();
    }
    RenderAgentCurrentOrDashboard();
    PublishAgentStatus("applied", target == "photo" || target == "idle_photo" ? "clear_idle_photo" : "clear_note");

    cJSON* origin = cJSON_GetObjectItem(const_cast<cJSON*>(root), "origin");
    std::string reply_target = JsonString(root, "reply_target");
    if (reply_target.empty()) {
        reply_target = JsonString(origin, "reply_target");
    }
    std::string reply = JsonString(root, "wechat_reply");
    if (reply.empty()) {
        reply = JsonString(root, "reply");
    }
    reply = StripPreviewUrlFromReply(reply);
    if (!reply.empty() && !reply_target.empty() && !WechatIngressEnabled()) {
        LogWechatIgnored("reply", "screen_clear");
    } else if (!reply.empty() && !reply_target.empty()) {
        SendTextMessage(reply_target.c_str(), reply);
    }
    return true;
}

bool WechatBot::ApplyAgentScreenDocument(const cJSON* document) {
    auto reject = [this](const char* detail) {
        PublishAgentStatus("rejected", detail);
        return false;
    };
    const std::string target = JsonString(document, "target");
    const bool idle_photo_target = target == "idle_photo" || target == "photo";
    if (!document || JsonString(document, "schema") != "weclawbot.screen_document.v1" ||
        (target != "content" && !idle_photo_target) || JsonString(document, "kind") != "replace") {
        return reject("invalid_screen_document");
    }
    const std::string id = JsonString(document, "id");
    const std::string base_revision = JsonString(document, "base_revision");
    const std::string expires_at = JsonString(document, "expires_at");
    const bool force_replace = JsonBool(document, "force_replace") || base_revision == "*";
    if (id.empty() || id.size() > 80 || !IsFutureUtcIso8601(expires_at)) {
        return reject("invalid_document_identity_or_expiry");
    }
    if (!idle_photo_target && !force_replace) {
        const Note* current = notes_.Current();
        const std::string current_revision = current ? current->screen_revision : "";
        if (base_revision != current_revision) {
            return reject("stale_screen_revision");
        }
    }

    cJSON* pages = cJSON_GetObjectItem(const_cast<cJSON*>(document), "pages");
    if (!cJSON_IsArray(pages)) {
        return reject("invalid_screen_pages");
    }
    const int count = cJSON_GetArraySize(pages);
    const int max_pages = idle_photo_target ? 1 : 3;
    const int max_width = idle_photo_target ? WEC_RLCD_WIDTH : WEC_CONTENT_BITMAP_WIDTH;
    const int max_height = idle_photo_target ? WEC_RLCD_HEIGHT : WEC_CONTENT_BITMAP_HEIGHT;
    if (count < 1 || count > max_pages) {
        return reject("screen_page_count_out_of_range");
    }
    std::vector<std::vector<uint8_t>> frames;
    int width = 0;
    int height = 0;
    int stride = 0;
    for (int index = 0; index < count; ++index) {
        cJSON* page = cJSON_GetArrayItem(pages, index);
        const int page_width = JsonInt(page, "width");
        const int page_height = JsonInt(page, "height");
        const int page_stride = JsonInt(page, "stride");
        if (JsonString(page, "format") != "mono1" || page_width <= 0 ||
            page_height <= 0 || page_width > max_width ||
            page_height > max_height ||
            page_stride < (page_width + 7) / 8) {
            return reject("invalid_screen_page_geometry");
        }
        if (index == 0) {
            width = page_width;
            height = page_height;
            stride = page_stride;
        } else if (width != page_width || height != page_height || stride != page_stride) {
            return reject("screen_pages_must_share_geometry");
        }
        std::vector<uint8_t> bytes = Base64DecodeBytes(JsonString(page, "data_b64"));
        const size_t needed = static_cast<size_t>(page_stride) * static_cast<size_t>(page_height);
        if (bytes.size() != needed) {
            return reject("invalid_screen_page_data");
        }
        frames.push_back(std::move(bytes));
    }

    std::string text = JsonString(document, "text");
    if (text.empty()) {
        text = JsonString(document, "summary");
    }
    if (text.empty()) {
        text = idle_photo_target ? "照片相框" : "智能体屏幕内容";
    }
    if (idle_photo_target) {
        if (!notes_.SetIdlePhoto(std::move(text), "agent", id, id, std::move(frames),
                                 width, height, stride)) {
            return reject("idle_photo_save_failed");
        }
        ClearThinkingState();
        if (const Note* photo = notes_.IdlePhoto()) {
            ui_.ShowIdlePhoto(*photo);
        } else {
            RenderAgentCurrentOrDashboard();
        }
        PublishAgentStatus("applied", id.c_str());
        PublishAgentEvent("applied", id.c_str());
        return true;
    }
    notes_.AddRendered(std::move(text), "agent", id, id, std::move(frames), width, height, stride);
    ClearThinkingState();
    ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    PublishAgentStatus("applied", id.c_str());
    PublishAgentEvent("applied", id.c_str());
    return true;
}

bool WechatBot::PublishAgentJson(cJSON* root, const char* stage) {
    bool ok = false;
    bool printed = false;
    bool print_failed = false;
    size_t payload_bytes = 0;
    const bool paired = agent_paired_;
    const bool mqtt_connected = agent_mqtt_.Connected();
    const bool events_topic_configured = !agent_credentials_.events_topic.empty();
    if (root && paired && mqtt_connected && events_topic_configured) {
        char* raw = cJSON_PrintUnformatted(root);
        if (raw) {
            printed = true;
            payload_bytes = std::strlen(raw);
            ok = agent_mqtt_.Publish(agent_credentials_.events_topic, raw);
            cJSON_free(raw);
        } else {
            print_failed = true;
        }
    }
    cJSON* log = NewEventLog("agent", stage ? stage : "event_publish");
    cJSON_AddBoolToObject(log, "root_present", root != nullptr);
    cJSON_AddBoolToObject(log, "paired", paired);
    cJSON_AddBoolToObject(log, "mqtt_connected", mqtt_connected);
    cJSON_AddBoolToObject(log, "events_topic_configured", events_topic_configured);
    cJSON_AddBoolToObject(log, "json_printed", printed);
    cJSON_AddBoolToObject(log, "json_print_failed", print_failed);
    cJSON_AddNumberToObject(log, "payload_bytes", static_cast<double>(payload_bytes));
    cJSON_AddNumberToObject(log, "dma_free",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_DMA)));
    cJSON_AddNumberToObject(log, "dma_largest",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    cJSON_AddNumberToObject(log, "internal_free",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    cJSON_AddNumberToObject(log, "internal_largest",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    cJSON_AddNumberToObject(log, "mqtt_publish_result",
                            static_cast<double>(agent_mqtt_.LastPublishResult()));
    cJSON_AddNumberToObject(log, "mqtt_publish_qos",
                            static_cast<double>(agent_mqtt_.LastPublishQos()));
    cJSON_AddBoolToObject(log, "published", ok);
    cJSON_AddStringToObject(log, "transport_state", AgentTransportState());
    EmitEventLog(log);
    return ok;
}

bool WechatBot::PublishWechatTextEvent(const char* from_user,
                                       const char* text,
                                       bool voice_transcript) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(voice_transcript ? "voice_transcript_event" : "text_event",
                         PreviewText(text).c_str());
        return false;
    }
    if (!text || text[0] == '\0') {
        return false;
    }
    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "wxmqtt_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.device_event.v1");
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", voice_transcript ? "wechat_voice_transcript" : "wechat_text");
    cJSON_AddStringToObject(root, "text", text);
    AddWechatIdentity(root, from_user);
    cJSON* origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "kind", "wechat");
    cJSON_AddStringToObject(origin, "correlation_id", event_id);
    cJSON_AddStringToObject(origin, "reply_target", from_user ? from_user : "");
    cJSON_AddItemToObject(root, "origin", origin);
    AddScreenContext(root);
    AddDeviceContext(root);
    AddAiConfig(root);
    const bool ok = PublishAgentJson(root, "wechat_event_publish");
    cJSON_Delete(root);
    return ok;
}

bool WechatBot::PublishWechatTextEventWithRetry(const char* from_user,
                                                const char* text,
                                                bool voice_transcript) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(voice_transcript ? "voice_transcript_event" : "text_event",
                         PreviewText(text).c_str());
        return false;
    }
    for (int attempt = 0; attempt < kAgentPublishRetryAttempts; ++attempt) {
        if (agent_paired_ && agent_mqtt_.Connected() &&
            PublishWechatTextEvent(from_user, text, voice_transcript)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kAgentPublishRetryDelayMs));
    }
    cJSON* log = NewEventLog("agent", "wechat_event_mqtt_unavailable");
    cJSON_AddBoolToObject(log, "paired", agent_paired_);
    cJSON_AddBoolToObject(log, "mqtt_connected", agent_mqtt_.Connected());
    cJSON_AddStringToObject(log, "transport_state", AgentTransportState());
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    EmitEventLog(log);
    return false;
}

void WechatBot::EnsurePendingAgentMutex() {
    if (!pending_agent_mutex_) {
        pending_agent_mutex_ = xSemaphoreCreateMutex();
    }
}

bool WechatBot::QueuePendingWechatText(const char* from_user,
                                       const char* text,
                                       bool voice_transcript) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(voice_transcript ? "voice_transcript_queue" : "text_queue",
                         PreviewText(text).c_str());
        return false;
    }
    if (!text || text[0] == '\0') {
        return false;
    }
    EnsurePendingAgentMutex();
    if (!pending_agent_mutex_ ||
        xSemaphoreTake(pending_agent_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    const int64_t now = std::time(nullptr);
    pending_wechat_texts_.erase(
        std::remove_if(pending_wechat_texts_.begin(), pending_wechat_texts_.end(),
                       [now](const PendingWechatText& item) {
                           return item.queued_at > 0 &&
                                  now - item.queued_at > kPendingWechatTextTtlSeconds;
                       }),
        pending_wechat_texts_.end());
    if (static_cast<int>(pending_wechat_texts_.size()) >= kPendingWechatTextMax) {
        pending_wechat_texts_.erase(pending_wechat_texts_.begin());
    }
    pending_wechat_texts_.push_back({
        from_user ? from_user : "",
        text,
        voice_transcript,
        now,
    });
    const size_t pending_count = pending_wechat_texts_.size();
    xSemaphoreGive(pending_agent_mutex_);

    cJSON* log = NewEventLog("agent", "wechat_event_queued");
    cJSON_AddNumberToObject(log, "pending_count", static_cast<double>(pending_count));
    cJSON_AddStringToObject(log, "transport_state", AgentTransportState());
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    EmitEventLog(log);
    return true;
}

void WechatBot::FlushPendingWechatTextEvents() {
    if (!WechatIngressEnabled()) {
        ClearPendingWechatTextEvents();
        return;
    }
    if (!agent_paired_ || !agent_mqtt_.Connected()) {
        return;
    }
    const int64_t now = std::time(nullptr);
    if (pending_agent_next_flush_at_ > 0 && now < pending_agent_next_flush_at_) {
        return;
    }
    EnsurePendingAgentMutex();
    if (!pending_agent_mutex_) {
        return;
    }

    PendingWechatText item;
    bool has_item = false;
    if (xSemaphoreTake(pending_agent_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        pending_wechat_texts_.erase(
            std::remove_if(pending_wechat_texts_.begin(), pending_wechat_texts_.end(),
                           [now](const PendingWechatText& pending) {
                               return pending.queued_at > 0 &&
                                      now - pending.queued_at > kPendingWechatTextTtlSeconds;
                           }),
            pending_wechat_texts_.end());
        if (!pending_wechat_texts_.empty()) {
            item = pending_wechat_texts_.front();
            has_item = true;
        }
        xSemaphoreGive(pending_agent_mutex_);
    }
    if (!has_item) {
        return;
    }

    if (!PublishWechatTextEvent(item.from_user.c_str(),
                                item.text.c_str(),
                                item.voice_transcript)) {
        pending_agent_next_flush_at_ = now + 3;
        return;
    }

    pending_agent_next_flush_at_ = 0;
    size_t pending_count = 0;
    if (xSemaphoreTake(pending_agent_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (!pending_wechat_texts_.empty()) {
            pending_wechat_texts_.erase(pending_wechat_texts_.begin());
        }
        pending_count = pending_wechat_texts_.size();
        xSemaphoreGive(pending_agent_mutex_);
    }
    cJSON* log = NewEventLog("agent", "wechat_event_queued_published");
    cJSON_AddNumberToObject(log, "pending_count", static_cast<double>(pending_count));
    cJSON_AddStringToObject(log, "preview", PreviewText(item.text.c_str()).c_str());
    EmitEventLog(log);
}

bool WechatBot::PublishWechatFeedbackEvent(const char* from_user,
                                           const char* text,
                                           const char* feedback_type,
                                           const char* truth_text) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("feedback_event", feedback_type);
        return false;
    }
    if (!feedback_type || feedback_type[0] == '\0') {
        return false;
    }
    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "wxfb_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.device_event.v1");
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", "wechat_feedback");
    cJSON_AddStringToObject(root, "feedback_type", feedback_type);
    cJSON_AddStringToObject(root, "text", text ? text : "");
    if (truth_text && truth_text[0] != '\0') {
        cJSON_AddStringToObject(root, "truth_text", truth_text);
    }
    AddWechatIdentity(root, from_user);
    cJSON* origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "kind", "wechat");
    cJSON_AddStringToObject(origin, "correlation_id", event_id);
    cJSON_AddStringToObject(origin, "reply_target", from_user ? from_user : "");
    cJSON_AddItemToObject(root, "origin", origin);
    AddScreenContext(root);
    AddDeviceContext(root);
    const bool ok = PublishAgentJson(root, "wechat_feedback_publish");
    cJSON_Delete(root);
    return ok;
}

bool WechatBot::PublishWechatAttachmentEvent(const char* from_user,
                                             const char* kind,
                                             const char* file_name,
                                             const char* cdn_url,
                                             const char* aes_key,
                                             const char* key_type,
                                             size_t byte_size) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(kind ? kind : "attachment_event", PreviewText(file_name).c_str());
        return false;
    }
    if (!kind || kind[0] == '\0' || !cdn_url || cdn_url[0] == '\0' ||
        !aes_key || aes_key[0] == '\0') {
        return false;
    }
    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "wxmedia_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.device_event.v1");
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", kind);
    if (file_name && file_name[0] != '\0') {
        cJSON_AddStringToObject(root, "file_name", file_name);
    }
    cJSON* media = cJSON_CreateObject();
    cJSON_AddStringToObject(media, "cdn_url", cdn_url);
    cJSON_AddStringToObject(media, "aes_key", aes_key);
    cJSON_AddStringToObject(media, "key_type", key_type ? key_type : "");
    cJSON_AddNumberToObject(media, "byte_size", static_cast<double>(byte_size));
    cJSON_AddItemToObject(root, "media", media);
    AddWechatIdentity(root, from_user);
    cJSON* origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "kind", "wechat");
    cJSON_AddStringToObject(origin, "correlation_id", event_id);
    cJSON_AddStringToObject(origin, "reply_target", from_user ? from_user : "");
    cJSON_AddItemToObject(root, "origin", origin);
    AddScreenContext(root);
    AddDeviceContext(root);
    AddAiConfig(root);
    const bool ok = PublishAgentJson(root, "wechat_attachment_publish");
    cJSON_Delete(root);
    return ok;
}

void WechatBot::PublishAgentEvent(const char* kind, const char* detail) {
    if (!agent_paired_ || agent_credentials_.events_topic.empty()) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.device_event.v1");
    cJSON_AddStringToObject(root, "kind", kind ? kind : "event");
    const std::string device_id = AgentTransportDeviceId();
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    if (detail) cJSON_AddStringToObject(root, "detail", detail);
    AddDeviceContext(root);
    char* raw = cJSON_PrintUnformatted(root);
    if (raw) {
        agent_mqtt_.Publish(agent_credentials_.events_topic, raw);
        cJSON_free(raw);
    }
    cJSON_Delete(root);
}

void WechatBot::PublishAgentStatus(const char* kind,
                                   const char* detail,
                                   const char* activity_correlation_id) {
    const char* status_kind = kind ? kind : "status";
    agent_last_status_kind_ = status_kind;
    agent_last_status_detail_ = detail ? detail : "";
    agent_last_status_at_ = std::time(nullptr);
    if (std::strcmp(status_kind, "rejected") == 0) {
        agent_last_reject_detail_ = agent_last_status_detail_;
        agent_last_reject_at_ = agent_last_status_at_;
    }
    if (!agent_paired_ || agent_credentials_.status_topic.empty()) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "weclawbot.device_status.v1");
    cJSON_AddStringToObject(root, "kind", status_kind);
    const std::string device_id = AgentTransportDeviceId();
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    if (detail) cJSON_AddStringToObject(root, "detail", detail);
    if (activity_correlation_id && activity_correlation_id[0]) {
        cJSON_AddStringToObject(root, "activity_correlation_id", activity_correlation_id);
    }
    char* raw = cJSON_PrintUnformatted(root);
    if (raw) {
        agent_mqtt_.Publish(agent_credentials_.status_topic, raw);
        cJSON_free(raw);
    }
    cJSON_Delete(root);
}

void WechatBot::ShowThinkingWithTimeout(const char* status,
                                        const char* footer,
                                        int ttl_seconds,
                                        const char* activity_correlation_id) {
    ttl_seconds = std::max(5, std::min(ttl_seconds, 180));
    agent_thinking_deadline_ = std::time(nullptr) + ttl_seconds;
    agent_activity_correlation_id_ = activity_correlation_id ? activity_correlation_id : "";
    ui_.ShowThinking(status, footer);
    cJSON* log = NewEventLog("agent", "thinking_timeout_set");
    cJSON_AddNumberToObject(log, "ttl_seconds", ttl_seconds);
    if (!agent_activity_correlation_id_.empty()) {
        cJSON_AddStringToObject(log, "activity_correlation_id", agent_activity_correlation_id_.c_str());
    }
    EmitEventLog(log);
}

void WechatBot::ClearThinkingState() {
    agent_thinking_deadline_ = 0;
    agent_activity_correlation_id_.clear();
}

void WechatBot::RenderAgentCurrentOrDashboard() {
    ClearThinkingState();
    if (!notes_.Empty()) {
        ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    } else if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
    } else if (IsByoaMode()) {
        ui_.ShowAgentDashboard();
    } else {
        ui_.ShowEmptyNotes();
    }
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
        if (relogin_requested_) {
            return;
        }
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
        cJSON* failure = response.body.empty() ? nullptr : cJSON_Parse(response.body.c_str());
        if (IsIlinkSessionInvalid(response.status, 0, 0, failure)) {
            InvalidateIlinkSession("getupdates", response.status, 0, 0, failure);
            if (failure) cJSON_Delete(failure);
            return true;
        }
        if (failure) cJSON_Delete(failure);
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
        // A 2xx response with a non-zero protocol result means iLink rejected
        // this session. Retrying the same token cannot recover it; return to QR.
        if (IsIlinkSessionInvalid(response.status, ret, errcode, root) ||
            (response.status >= 200 && response.status < 300)) {
            InvalidateIlinkSession("getupdates", response.status, ret, errcode, root);
            cJSON_Delete(root);
            return true;
        }
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

    if (!WechatIngressEnabled()) {
        cJSON* ignored = NewEventLog("wechat", "item_ignored");
        cJSON_AddStringToObject(ignored, "reason", "byoa_agent_owns_screen");
        cJSON_AddNumberToObject(ignored, "item_type", type->valueint);
        cJSON_AddBoolToObject(ignored, "has_sender", from_user && from_user[0] != '\0');
        EmitEventLog(ignored);
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
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(allow_commands ? "text" : "voice_transcript",
                         PreviewText(text).c_str());
        return;
    }
    TouchActivity();

    cJSON* log = NewEventLog("wechat", allow_commands ? "text_received" : "voice_transcript_received");
    cJSON_AddStringToObject(log, "preview", PreviewText(text).c_str());
    cJSON_AddNumberToObject(log, "bytes", text ? static_cast<double>(std::strlen(text)) : 0);
    cJSON_AddBoolToObject(log, "commands_enabled", allow_commands);
    EmitEventLog(log);

    const std::string command = allow_commands ? TrimWhitespace(text ? text : "") : "";

    if (allow_commands && IsHelpCommand(command)) {
        EmitEventLog(NewEventLog("command", "help"));
        if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, HelpReply());
        }
        return;
    }
    if (allow_commands && command == "/next") {
        EmitEventLog(NewEventLog("command", "next"));
        if (!ui_.ShowNextNotePage(notes_.All(), notes_.CurrentIndex())) {
            RenderCurrentOrEmpty();
        }
        return;
    }
    if (allow_commands && command == "/prev") {
        EmitEventLog(NewEventLog("command", "prev"));
        if (!ui_.ShowPreviousNotePage(notes_.All(), notes_.CurrentIndex())) {
            RenderCurrentOrEmpty();
        }
        return;
    }
    if (allow_commands && IsRecaptureCommand(command)) {
        EmitEventLog(NewEventLog("feedback", "recapture_requested"));
        if (PublishWechatFeedbackEvent(from_user, command.c_str(), "recapture")) {
            ShowThinkingWithTimeout("正在回捞刚才的内容", "智能体正在处理");
        } else if (!curator_url_.empty()) {
            ShowThinkingWithTimeout("正在回捞刚才的内容", "红方块思考中...");
            if (!SendCuratorFeedback(from_user, command.c_str(), "recapture", nullptr, true)) {
                RenderCurrentOrEmpty();
                if (from_user && from_user[0] != '\0') {
                    SendTextMessage(from_user, "暂时没找到刚才被忽略的内容。请直接重发想贴到屏幕上的文字。");
                }
            }
        } else if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "当前没有启用云端整理，无法回捞上一条消息。请直接重发想贴到屏幕上的文字。");
        }
        return;
    }
    if (allow_commands && IsClearPhotoCommand(command)) {
        EmitEventLog(NewEventLog("command", "clear_idle_photo"));
        notes_.ClearIdlePhoto();
        RenderCurrentOrEmpty();
        if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "已清除照片屏。日历和留言会继续轮播。");
        }
        return;
    }
    if (allow_commands && IsClearCurrentCommand(command)) {
        EmitEventLog(NewEventLog("command", "clear_current"));
        notes_.ClearCurrent();
        RenderCurrentOrEmpty();
        if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "已清空屏幕。照片屏会保留，可发“清空照片”单独清除。");
        }
        if (!PublishWechatFeedbackEvent(from_user, command.c_str(), "rapid_clear")) {
            SendCuratorFeedback(from_user, command.c_str(), "rapid_clear");
        }
        return;
    }
    if (allow_commands && IsClearAllCommand(command)) {
        EmitEventLog(NewEventLog("command", "clear_all"));
        notes_.ClearAll();
        RenderCurrentOrEmpty();
        if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "已清空全部微笺。照片屏会保留，可发“清空照片”单独清除。");
        }
        if (!PublishWechatFeedbackEvent(from_user, command.c_str(), "rapid_clear")) {
            SendCuratorFeedback(from_user, command.c_str(), "rapid_clear");
        }
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
            if (!PublishWechatFeedbackEvent(from_user, text, "correction", replacement.c_str())) {
                SendCuratorFeedback(from_user, text, "correction", replacement.c_str());
            }
        } else if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "当前没有可修改的微笺。请先发送一条想显示在屏上的内容。");
        }
        return;
    }

    if (PublishWechatTextEventWithRetry(from_user, text, !allow_commands)) {
        cJSON* start_log = NewEventLog("agent", "wechat_event_forwarded");
        cJSON_AddStringToObject(start_log, "preview", PreviewText(text).c_str());
        EmitEventLog(start_log);
        ShowThinkingWithTimeout("正在交给智能体", "屏幕会在处理完成后更新");
        return;
    }

    if (agent_paired_ || IsByoaMode()) {
        if (QueuePendingWechatText(from_user, text, !allow_commands)) {
            ShowThinkingWithTimeout("智能体通道重连中", "恢复后会自动处理");
            if (from_user && from_user[0] != '\0') {
                SendTextMessage(from_user, "智能体通道正在重连，恢复后会自动贴到屏幕。");
            }
            return;
        }
        ui_.ShowError("智能体通道重连中", "本地暂存失败，请稍后再发");
        if (from_user && from_user[0] != '\0') {
            SendTextMessage(from_user, "智能体通道正在重连，本地暂存失败；请稍后再发一次。");
        }
        return;
    }

    if (!curator_url_.empty()) {
        cJSON* start_log = NewEventLog("curator", "start");
        cJSON_AddStringToObject(start_log, "preview", PreviewText(text).c_str());
        EmitEventLog(start_log);
        ShowThinkingWithTimeout("正在整理微信内容", "红方块思考中...");
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
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("image");
        return;
    }
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

    if (PublishWechatAttachmentEvent(from_user,
                                     "wechat_image",
                                     "",
                                     cdn_url.c_str(),
                                     aes_key.c_str(),
                                     "image",
                                     byte_size)) {
        ShowThinkingWithTimeout("收到图片", "智能体正在处理图像");
        return;
    }

    if (!curator_url_.empty() && !cdn_url.empty() && !aes_key.empty()) {
        ShowThinkingWithTimeout("收到图片", "红方块正在等待图文提取...");
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
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("file", PreviewText(JsonString(file_item, "file_name").c_str()).c_str());
        return;
    }
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

    if (PublishWechatAttachmentEvent(from_user,
                                     "wechat_file",
                                     file_name.c_str(),
                                     cdn_url.c_str(),
                                     aes_key.c_str(),
                                     "file",
                                     byte_size)) {
        ShowThinkingWithTimeout("收到文件", "智能体正在处理文件");
        return;
    }

    if (!curator_url_.empty() && !cdn_url.empty() && !aes_key.empty()) {
        ShowThinkingWithTimeout("收到文件", "红方块正在等待内容提取...");
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
    if (!WechatIngressEnabled()) {
        LogWechatIgnored(kind ? kind : "unsupported", PreviewText(file_name).c_str());
        return;
    }
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

void WechatBot::AddDeviceContext(cJSON* root) const {
    // This is intentionally a hardware/transport contract, not a prompt or
    // content policy. A user-selected agent receives it on every event and
    // decides what to show within these bounds.
    cJSON* context = cJSON_CreateObject();
    cJSON_AddStringToObject(context, "schema", "weclawbot.device_context.v1");
    cJSON_AddStringToObject(context, "firmware", WEC_FIRMWARE_VERSION);
    const std::string mqtt_device_id = AgentTransportDeviceId();
    const std::string device_id = !mqtt_device_id.empty() ? mqtt_device_id : WechatId();
    if (!device_id.empty()) {
        cJSON_AddStringToObject(context, "device_id", device_id.c_str());
    }

    cJSON* canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(canvas, "width", WEC_RLCD_WIDTH);
    cJSON_AddNumberToObject(canvas, "height", WEC_RLCD_HEIGHT);
    cJSON_AddStringToObject(canvas, "color", "mono1");
    cJSON_AddStringToObject(canvas, "refresh", "reflective_slow");
    cJSON_AddItemToObject(context, "canvas", canvas);

    cJSON* viewport = cJSON_CreateObject();
    cJSON_AddStringToObject(viewport, "id", "content");
    cJSON_AddNumberToObject(viewport, "x", WEC_CONTENT_BITMAP_X);
    cJSON_AddNumberToObject(viewport, "y", WEC_CONTENT_BITMAP_Y);
    cJSON_AddNumberToObject(viewport, "width", WEC_CONTENT_BITMAP_WIDTH);
    cJSON_AddNumberToObject(viewport, "height", WEC_CONTENT_BITMAP_HEIGHT);
    cJSON_AddStringToObject(viewport, "format", "mono1");
    cJSON_AddNumberToObject(viewport, "max_pages", 3);
    cJSON_AddNumberToObject(viewport, "auto_page_seconds", CONFIG_WEC_AUTO_PAGE_SEC);
    cJSON_AddItemToObject(context, "content_viewport", viewport);

    cJSON* chrome = cJSON_CreateObject();
    cJSON_AddStringToObject(chrome, "owner", "firmware");
    cJSON_AddStringToObject(chrome, "reserved", "status_bar,footer");
    cJSON_AddItemToObject(context, "chrome", chrome);

    cJSON* wechat_transport = cJSON_CreateObject();
    const bool agent_available = agent_paired_ && agent_mqtt_.Connected();
    if (WechatIngressEnabled()) {
        cJSON_AddStringToObject(wechat_transport, "mode", "ilink_getupdates_long_poll");
        cJSON_AddStringToObject(wechat_transport, "direction", "wechat_to_device_event");
        cJSON_AddNumberToObject(wechat_transport, "request_timeout_ms", WEC_HTTP_TIMEOUT_MS);
        cJSON_AddNumberToObject(wechat_transport, "idle_retry_delay_ms", CONFIG_WEC_POLL_INTERVAL_MS);
        cJSON_AddBoolToObject(wechat_transport, "agent_push_supported", agent_available);
    } else {
        cJSON_AddStringToObject(wechat_transport, "mode", "disabled");
        cJSON_AddStringToObject(wechat_transport, "direction", "ignored");
        cJSON_AddStringToObject(wechat_transport, "reason", "byoa_agent_owns_screen");
        cJSON_AddBoolToObject(wechat_transport, "agent_push_supported", false);
    }
    cJSON_AddItemToObject(context, "wechat_transport", wechat_transport);

    // Direct agent delivery is a separately paired MQTT/TLS channel. It is
    // advertised now so external agent skills can plan against a stable
    // contract, but it cannot be used until a device has been provisioned.
    cJSON* agent_transport = cJSON_CreateObject();
    cJSON_AddStringToObject(agent_transport, "mode", "mqtt_tls_pubsub");
    cJSON_AddStringToObject(agent_transport, "direction", "device_events_and_agent_control");
    cJSON_AddStringToObject(agent_transport, "owner", IsByoaMode() ? kByoaAgentOwner : kOfficialAgentOwner);
    cJSON_AddStringToObject(agent_transport, "state", AgentTransportState());
    cJSON_AddBoolToObject(agent_transport, "available", agent_available);
    cJSON_AddBoolToObject(agent_transport, "screen_document_available", agent_available);
    cJSON_AddBoolToObject(agent_transport, "activity_available", agent_available);
    cJSON_AddStringToObject(agent_transport, "activity_correlation_id",
                            agent_activity_correlation_id_.c_str());
    cJSON_AddNumberToObject(agent_transport, "activity_seconds_left",
                            static_cast<double>(AgentThinkingSecondsLeft()));
    cJSON_AddBoolToObject(agent_transport, "queue_or_mailbox", false);
    cJSON_AddStringToObject(agent_transport, "delivery", "live_qos0_with_device_retry");
    cJSON_AddNumberToObject(agent_transport, "session_expiry_seconds", 0);
    cJSON_AddNumberToObject(agent_transport, "recommended_min_update_interval_ms", 60000);
    cJSON_AddItemToObject(context, "agent_transport", agent_transport);

    cJSON* state = cJSON_CreateObject();
    const Note* current = notes_.Current();
    cJSON_AddStringToObject(state, "screen_revision",
                            current ? current->screen_revision.c_str() : "");
    cJSON_AddNumberToObject(state, "note_count", static_cast<double>(notes_.Count()));
    cJSON_AddItemToObject(context, "state", state);
    cJSON_AddItemToObject(root, "device_context", context);
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
    cJSON_AddStringToObject(source, "kind", "wechat");
    if (!wechat_id.empty()) {
        cJSON_AddStringToObject(source, "wechat_id", wechat_id.c_str());
    }
    cJSON_AddStringToObject(source, "sender_ref", sender_ref);
    cJSON_AddStringToObject(source, "reply_target", sender_ref);
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
    AddDeviceContext(root);
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

bool WechatBot::SendCuratorFeedback(const char* from_user,
                                    const char* text,
                                    const char* feedback_type,
                                    const char* truth_text,
                                    bool apply_response) {
    if (curator_url_.empty() || !feedback_type || feedback_type[0] == '\0') {
        return false;
    }

    char event_id[48];
    std::snprintf(event_id, sizeof(event_id), "esp32fb_%ld_%08lx",
                  static_cast<long>(std::time(nullptr)),
                  static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "kind", "wechat_feedback");
    cJSON_AddStringToObject(root, "feedback_type", feedback_type);
    cJSON_AddStringToObject(root, "text", text ? text : "");
    if (truth_text && truth_text[0] != '\0') {
        cJSON_AddStringToObject(root, "truth_text", truth_text);
    }
    AddWechatIdentity(root, from_user);
    AddScreenContext(root);
    AddDeviceContext(root);

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    cJSON* request_log = NewEventLog("curator", "feedback_request");
    cJSON_AddStringToObject(request_log, "event_id", event_id);
    cJSON_AddStringToObject(request_log, "feedback_type", feedback_type);
    cJSON_AddStringToObject(request_log, "preview", PreviewText(text).c_str());
    cJSON_AddBoolToObject(request_log, "apply_response", apply_response);
    EmitEventLog(request_log);

    HttpResponse response = HttpPost(curator_url_, body, false);
    cJSON* response_log = NewEventLog("curator", "feedback_response");
    cJSON_AddStringToObject(response_log, "event_id", event_id);
    cJSON_AddNumberToObject(response_log, "status", response.status);
    cJSON_AddNumberToObject(response_log, "esp_error", response.error);
    cJSON_AddBoolToObject(response_log, "overflow", response.overflow);
    cJSON_AddNumberToObject(response_log, "response_bytes", static_cast<double>(response.body.size()));
    EmitEventLog(response_log);
    if (!response.Ok() || response.body.empty()) {
        ESP_LOGW(kTag, "curator feedback failed status=%d err=%s",
                 response.status, esp_err_to_name(response.error));
        return false;
    }
    if (apply_response) {
        return ApplyCuratorDecision(from_user, std::move(response.body));
    }
    return true;
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
    AddDeviceContext(root);

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
            reply = "已设为照片屏，会随日历、留言一起轮播显示。继续发送照片可替换照片屏。";
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
            cJSON_AddStringToObject(screen_log, "revision", screen_revision.c_str());
            cJSON_AddStringToObject(screen_log, "render_id", render_id.c_str());
            EmitEventLog(screen_log);
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

    if (std::strcmp(action, "clear_idle_photo") == 0 ||
        std::strcmp(action, "clear_photo") == 0) {
        std::string reply = JsonString(decision, "user_reply");
        notes_.ClearIdlePhoto();
        RenderCurrentOrEmpty();
        if (reply.empty()) {
            reply = "已清除照片屏。日历和留言会继续轮播。";
        }
        reply_to_send = reply;
        if (inner) cJSON_Delete(inner);
        cJSON_Delete(root);
        EmitHeapLog("curator", "after_idle_photo_clear");
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
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("sendmessage", PreviewText(text.c_str()).c_str());
        return false;
    }
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
    const bool session_invalid = IsIlinkSessionInvalid(response.status, ret, errcode, send_json);
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
    if (!ok && session_invalid) {
        InvalidateIlinkSession("sendmessage", response.status, ret, errcode, nullptr);
    }
    return ok;
}

bool WechatBot::SendPreviewImageMessage(const char* to_user, const std::string& preview_url) {
    if (!WechatIngressEnabled()) {
        LogWechatIgnored("send_preview_image", preview_url.c_str());
        return false;
    }
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
        cJSON* failure = upload_ticket.body.empty() ? nullptr : cJSON_Parse(upload_ticket.body.c_str());
        const bool session_invalid = IsIlinkSessionInvalid(upload_ticket.status, 0, 0, failure);
        if (failure) cJSON_Delete(failure);
        cJSON* log = NewEventLog("wechat", "send_preview_image_result");
        cJSON_AddBoolToObject(log, "sent", false);
        cJSON_AddStringToObject(log, "image_stage", "getuploadurl");
        cJSON_AddNumberToObject(log, "attempts", kPreviewImageRetryAttempts);
        cJSON_AddNumberToObject(log, "status", upload_ticket.status);
        cJSON_AddNumberToObject(log, "esp_error", upload_ticket.error);
        cJSON_AddNumberToObject(log, "preview_bytes", static_cast<double>(preview_size));
        EmitEventLog(log);
        if (session_invalid) {
            InvalidateIlinkSession("getuploadurl", upload_ticket.status, 0, 0, nullptr);
        }
        return false;
    }

    cJSON* upload_json = cJSON_Parse(upload_ticket.body.c_str());
    const int upload_ret = JsonInt(upload_json, "ret", 0);
    const int upload_errcode = JsonInt(upload_json, "errcode", 0);
    std::string upload_full_url = JsonString(upload_json, "upload_full_url");
    std::string upload_param = JsonString(upload_json, "upload_param");
    const bool upload_session_invalid = IsIlinkSessionInvalid(upload_ticket.status,
                                                               upload_ret,
                                                               upload_errcode,
                                                               upload_json);
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
        if (upload_session_invalid) {
            InvalidateIlinkSession("getuploadurl", upload_ticket.status,
                                   upload_ret, upload_errcode, nullptr);
        }
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
    const bool session_invalid = IsIlinkSessionInvalid(response.status, ret, errcode, send_json);
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
    if (!ok && session_invalid) {
        InvalidateIlinkSession("sendmessage_preview", response.status, ret, errcode, nullptr);
    }
    return ok;
}

std::string WechatBot::HttpGet(const std::string& url, std::string* date_header) {
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
    if (date_header) {
        *date_header = response.date_header;
    }
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
    ClearThinkingState();
    if (!notes_.Empty()) {
        ui_.ShowNotes(notes_.All(), notes_.CurrentIndex());
    } else if (const Note* photo = notes_.IdlePhoto()) {
        ui_.ShowIdlePhoto(*photo);
    } else if (IsByoaMode() && agent_paired_) {
        ui_.ShowAgentDashboard();
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

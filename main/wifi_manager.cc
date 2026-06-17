#include "wifi_manager.h"

#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <nvs.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "WifiManager";
constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;
constexpr int kMaxRetries = 20;
constexpr char kSsidKey[] = "wifi_ssid";
constexpr char kPasswordKey[] = "wifi_pass";

bool ReadString(nvs_handle_t nvs, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len <= 1) {
        return false;
    }
    std::string value(len, '\0');
    if (nvs_get_str(nvs, key, value.data(), &len) != ESP_OK) {
        return false;
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    out = value;
    return true;
}

const char* DisconnectReasonText(int reason) {
    switch (reason) {
        case 0:
            return "";
        case WIFI_REASON_NO_AP_FOUND:
            return "找不到接入点，请确认使用 2.4GHz Wi-Fi";
        case WIFI_REASON_AUTH_FAIL:
            return "认证失败，请检查 Wi-Fi 密码";
        case WIFI_REASON_ASSOC_FAIL:
            return "接入点拒绝关联";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "Wi-Fi 密钥握手超时";
        case WIFI_REASON_CONNECTION_FAIL:
            return "Wi-Fi 连接失败";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "接入点安全模式不兼容";
        default:
            return "Wi-Fi 已断开";
    }
}
}  // namespace

const char* WifiManager::LastDisconnectReasonText() const {
    return DisconnectReasonText(last_disconnect_reason_);
}

bool WifiManager::LoadCredentials() {
    ssid_.clear();
    password_.clear();

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        ReadString(nvs, kSsidKey, ssid_);
        ReadString(nvs, kPasswordKey, password_);
        nvs_close(nvs);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "nvs_open read failed: %s", esp_err_to_name(err));
    }

    if (ssid_.empty()) {
        ssid_ = CONFIG_WEC_WIFI_SSID;
        password_ = CONFIG_WEC_WIFI_PASSWORD;
    }

    return !ssid_.empty();
}

bool WifiManager::Connect() {
    LoadCredentials();
    last_disconnect_reason_ = 0;
    ip_address_.clear();
    if (ssid_.empty()) {
        ESP_LOGE(kTag, "Wi-Fi SSID is not configured");
        return false;
    }

    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(kTag, "xEventGroupCreate failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, this,
                                                        nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, this,
                                                        nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid_.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password_.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(kTag, "Connecting to %s", ssid_.c_str());
    EventBits_t bits = xEventGroupWaitBits(event_group_, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(45000));
    connected_ = bits & kConnectedBit;
    return connected_;
}

bool WifiManager::SaveCredentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 63) {
        return false;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, kSsidKey, ssid.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, kPasswordKey, password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Save credentials failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool WifiManager::ClearCredentials() {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open clear failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_erase_key(nvs, kSsidKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        esp_err_t pass_err = nvs_erase_key(nvs, kPasswordKey);
        if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
            err = pass_err;
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

void WifiManager::EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiManager*>(arg);
    static int retry_count = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        retry_count = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        self->connected_ = false;
        self->ip_address_.clear();
        self->last_disconnect_reason_ = event ? event->reason : 0;
        if (retry_count < kMaxRetries) {
            retry_count++;
            ESP_LOGW(kTag, "Wi-Fi disconnected: reason=%d (%s), retry %d/%d",
                     self->last_disconnect_reason_, self->LastDisconnectReasonText(), retry_count,
                     kMaxRetries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(self->event_group_, kFailedBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        char ip[16] = {};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        self->ip_address_ = ip;
        self->connected_ = true;
        self->last_disconnect_reason_ = 0;
        retry_count = 0;
        ESP_LOGI(kTag, "Got IP %s", ip);
        xEventGroupSetBits(self->event_group_, kConnectedBit);
    }
}

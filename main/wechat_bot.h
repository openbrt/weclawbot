#pragma once

#include <atomic>
#include <string>

#include <cJSON.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "note_store.h"
#include "ui.h"

class WechatBot {
public:
    WechatBot(Ui& ui, NoteStore& notes) : ui_(ui), notes_(notes) {}

    void Start();
    void RequestRelogin();
    void ClearSavedCredentials();
    bool Connected() const { return connected_; }
    const char* LoginStateText() const;
    int QrSecondsLeft() const { return qr_seconds_left_; }
    int64_t LastActivity() const { return last_activity_; }
    bool CurateLoopbackText(const char* text);
    bool CurateLoopbackImage(const char* url);

private:
    enum class LoginState {
        kStarting,
        kFetchingQr,
        kWaitingForScan,
        kScanned,
        kSwitchingLine,
        kConnected,
        kQrError,
    };

    struct HttpResponse {
        std::string body;
        std::string encrypted_param;
        bool overflow = false;
        esp_err_t error = ESP_OK;
        int status = -1;

        bool Ok() const { return error == ESP_OK && !overflow && status < 400; }
        bool IdleTimeout() const { return error == ESP_ERR_HTTP_EAGAIN; }
    };

    static esp_err_t HttpEventHandler(esp_http_client_event_t* event);

    void SyncTime();
    void LoadCredentials();
    void LoadCuratorConfig();
    void SaveCredentials();
    void ClearCredentials();

    bool DoQrLogin();
    bool FetchQrCode();
    bool PollQrStatus();
    bool DoGetUpdates();
    void RunGetUpdatesLoop();
    void DispatchItem(const cJSON* item, const char* from_user);
    void HandleText(const char* from_user, const char* text, bool allow_commands = true);
    void HandleImage(const cJSON* image_item, const char* from_user);
    void HandleFile(const cJSON* file_item, const char* from_user);
    void HandleUnsupported(const char* from_user, const char* kind, const char* file_name);
    void AddAiConfig(cJSON* root) const;
    void AddScreenContext(cJSON* root) const;
    void AddWechatIdentity(cJSON* root, const char* from_user) const;
    bool CurateText(const char* from_user, const char* text, bool voice_transcript);
    bool CurateAttachment(const char* from_user,
                          const char* kind,
                          const char* file_name,
                          const char* cdn_url,
                          const char* aes_key,
                          const char* key_type,
                          size_t byte_size);
    bool ApplyCuratorDecision(const char* from_user, std::string response_body);
    bool SendTextMessage(const char* to_user, const std::string& text);
    bool SendPreviewImageMessage(const char* to_user, const std::string& preview_url);

    std::string HttpGet(const std::string& url);
    HttpResponse HttpPost(const std::string& url,
                          const std::string& body,
                          bool with_auth,
                          const char* content_type = "application/json",
                          bool include_wechat_uin = true,
                          const char* extra_header_key = nullptr,
                          const char* extra_header_value = nullptr);
    std::string MakeWechatUin() const;
    std::string WechatId() const;
    void TouchActivity();
    void RenderCurrentOrEmpty();
    void EnsureHttpMutex();
    bool TakeHttpMutex(TickType_t timeout_ticks);
    void GiveHttpMutex(bool locked);

    Ui& ui_;
    NoteStore& notes_;

    std::string base_url_ = CONFIG_WEC_WECHAT_BASE_URL;
    std::string bot_token_;
    std::string bot_id_;
    std::string qrcode_token_;
    std::string cursor_;
    std::string last_context_token_;
    std::string curator_url_;
    std::string ai_provider_;
    std::string ai_token_;
    std::string ai_endpoint_;
    std::string ai_model_;
    SemaphoreHandle_t http_mutex_ = nullptr;

    std::atomic_bool relogin_requested_{false};
    std::atomic_bool local_curator_active_{false};
    std::atomic_bool connected_{false};
    std::atomic<LoginState> login_state_{LoginState::kStarting};
    std::atomic_int qr_seconds_left_{0};
    int64_t last_activity_ = 0;
    int64_t qr_fetch_time_ = 0;
};

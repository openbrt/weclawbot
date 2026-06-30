#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "agent_mqtt.h"
#include "note_store.h"
#include "ui.h"

class WechatBot {
public:
    WechatBot(Ui& ui, NoteStore& notes) : ui_(ui), notes_(notes) {}

    void Start();
    void RequestRelogin();
    void ClearSavedCredentials();
    void ClearAgentCredentials();
    bool Connected() const { return connected_; }
    const char* LoginStateText() const;
    int QrSecondsLeft() const { return qr_seconds_left_; }
    int64_t LastActivity() const { return last_activity_; }
    bool WechatIngressEnabled() const;
    bool UsesCustomAgent() const { return custom_agent_mode_; }
    bool AgentPaired() const { return agent_paired_; }
    bool AgentMqttConnected() const { return agent_mqtt_.Connected(); }
    const char* AgentTransportState() const;
    std::string AgentDeviceId() const;
    const char* AgentPairingCode() const { return agent_pairing_code_.c_str(); }
    int AgentPairingSecondsLeft() const;
    const char* AgentLastStatusKind() const { return agent_last_status_kind_.c_str(); }
    const char* AgentLastStatusDetail() const { return agent_last_status_detail_.c_str(); }
    int64_t AgentLastStatusAt() const { return agent_last_status_at_; }
    const char* AgentLastRejectDetail() const { return agent_last_reject_detail_.c_str(); }
    int64_t AgentLastRejectAt() const { return agent_last_reject_at_; }
    const char* AgentActivityCorrelationId() const { return agent_activity_correlation_id_.c_str(); }
    int AgentThinkingSecondsLeft() const;
    bool AgentEventsTopicConfigured() const { return !agent_credentials_.events_topic.empty(); }
    bool AgentStatusTopicConfigured() const { return !agent_credentials_.status_topic.empty(); }
    void RetryTimeSync();
    bool CurateLoopbackText(const char* text);
    bool CurateLoopbackImage(const char* url);
    bool WechatLoopbackText(const char* text);
    bool WechatLoopbackImage(const char* url);

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
        std::string date_header;
        bool overflow = false;
        esp_err_t error = ESP_OK;
        int status = -1;

        bool Ok() const { return error == ESP_OK && !overflow && status < 400; }
        bool IdleTimeout() const { return error == ESP_ERR_HTTP_EAGAIN; }
    };

    struct PendingWechatText {
        std::string from_user;
        std::string text;
        bool voice_transcript = false;
        int64_t queued_at = 0;
    };

    static esp_err_t HttpEventHandler(esp_http_client_event_t* event);

    void SyncTime();
    void LoadCredentials();
    void LoadCuratorConfig();
    void SaveCredentials();
    void ClearCredentials();
    void LoadAgentCredentials();
    void SaveAgentCredentials();
    void EnsureAgentDeviceId();

    bool DoQrLogin();
    bool FetchQrCode();
    bool PollQrStatus();
    bool DoGetUpdates();
    void RunGetUpdatesLoop();
    bool IsIlinkSessionInvalid(int http_status, int ret, int errcode, const cJSON* payload) const;
    void InvalidateIlinkSession(const char* operation,
                                int http_status,
                                int ret,
                                int errcode,
                                const cJSON* payload);
    void DispatchItem(const cJSON* item, const char* from_user);
    void HandleText(const char* from_user, const char* text, bool allow_commands = true);
    void HandleImage(const cJSON* image_item, const char* from_user);
    void HandleFile(const cJSON* file_item, const char* from_user);
    void HandleUnsupported(const char* from_user, const char* kind, const char* file_name);
    void AddAiConfig(cJSON* root) const;
    void AddScreenContext(cJSON* root) const;
    void AddDeviceContext(cJSON* root) const;
    void AddWechatIdentity(cJSON* root, const char* from_user) const;
    bool IsByoaMode() const;
    void LogWechatIgnored(const char* kind, const char* detail = nullptr);
    void ClearPendingWechatTextEvents();
    void RunByoaMode();
    bool BeginOfficialAgentSession();
    bool BeginByoaPairing();
    bool ParseAgentMqttCredentials(const cJSON* mqtt, AgentMqtt::Credentials* credentials) const;
    std::string AgentStorageOwner() const;
    std::string AgentTransportDeviceId() const;
    void StartAgentPump();
    static void AgentPumpTask(void* arg);
    void MaintainAgentMqtt();
    void ProcessAgentMessages();
    void HandleAgentControl(const std::string& payload);
    bool HandleAgentScreenIntent(const cJSON* intent);
    bool HandleAgentWechatReply(const cJSON* root);
    bool HandleAgentScreenClear(const cJSON* root);
    bool ApplyAgentScreenDocument(const cJSON* document);
    bool PublishWechatTextEventWithRetry(const char* from_user,
                                         const char* text,
                                         bool voice_transcript);
    bool PublishWechatTextEvent(const char* from_user, const char* text, bool voice_transcript);
    bool QueuePendingWechatText(const char* from_user, const char* text, bool voice_transcript);
    void FlushPendingWechatTextEvents();
    void EnsurePendingAgentMutex();
    bool PublishWechatFeedbackEvent(const char* from_user,
                                    const char* text,
                                    const char* feedback_type,
                                    const char* truth_text = nullptr);
    bool PublishWechatAttachmentEvent(const char* from_user,
                                      const char* kind,
                                      const char* file_name,
                                      const char* cdn_url,
                                      const char* aes_key,
                                      const char* key_type,
                                      size_t byte_size);
    bool PublishAgentJson(cJSON* root, const char* stage);
    void PublishAgentEvent(const char* kind, const char* detail = nullptr);
    void PublishAgentStatus(const char* kind,
                            const char* detail = nullptr,
                            const char* activity_correlation_id = nullptr);
    void ShowThinkingWithTimeout(const char* status,
                                 const char* footer,
                                 int ttl_seconds = 90,
                                 const char* activity_correlation_id = nullptr);
    void ClearThinkingState();
    void RenderAgentCurrentOrDashboard();
    bool CurateText(const char* from_user, const char* text, bool voice_transcript);
    bool SendCuratorFeedback(const char* from_user,
                             const char* text,
                             const char* feedback_type,
                             const char* truth_text = nullptr,
                             bool apply_response = false);
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

    std::string HttpGet(const std::string& url, std::string* date_header = nullptr);
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
    AgentMqtt agent_mqtt_;
    AgentMqtt::Credentials agent_credentials_;
    std::string agent_device_id_;
    std::string agent_pairing_code_;
    std::string agent_last_status_kind_;
    std::string agent_last_status_detail_;
    std::string agent_last_reject_detail_;
    std::string agent_activity_correlation_id_;
    int64_t agent_pairing_expires_at_ = 0;
    int64_t agent_last_bootstrap_attempt_ = 0;
    int64_t agent_thinking_deadline_ = 0;
    int64_t agent_last_status_at_ = 0;
    int64_t agent_last_reject_at_ = 0;
    bool custom_agent_mode_ = false;
    bool agent_paired_ = false;
    bool agent_online_announced_ = false;
    TaskHandle_t agent_pump_task_ = nullptr;
    SemaphoreHandle_t http_mutex_ = nullptr;
    SemaphoreHandle_t pending_agent_mutex_ = nullptr;
    std::vector<PendingWechatText> pending_wechat_texts_;
    int64_t pending_agent_next_flush_at_ = 0;

    std::atomic_bool relogin_requested_{false};
    std::atomic_bool local_curator_active_{false};
    std::atomic_bool connected_{false};
    std::atomic<LoginState> login_state_{LoginState::kStarting};
    std::atomic_int qr_seconds_left_{0};
    int64_t last_activity_ = 0;
    int64_t qr_fetch_time_ = 0;
};

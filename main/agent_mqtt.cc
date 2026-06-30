#include "agent_mqtt.h"

#include <utility>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "AgentMqtt";
// A full 368 x 206 mono1 page is 9,476 bytes before base64. Three pages in
// one JSON control document are about 39 KiB. Keep a modest ceiling above
// that contract, rather than accidentally accepting an arbitrary upload.
constexpr size_t kMaxQueuedMessages = 2;
constexpr size_t kMaxMessageBytes = 48 * 1024;
}

AgentMqtt::AgentMqtt() {
    queue_mutex_ = xSemaphoreCreateMutex();
}

AgentMqtt::~AgentMqtt() {
    Disconnect();
    if (queue_mutex_) {
        vSemaphoreDelete(queue_mutex_);
        queue_mutex_ = nullptr;
    }
}

bool AgentMqtt::Connect(const Credentials& credentials) {
    if (!credentials.Valid()) {
        ESP_LOGW(kTag, "agent MQTT credentials incomplete");
        return false;
    }

    Disconnect();
    credentials_ = credentials;
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = credentials_.url.c_str();
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.username = credentials_.username.c_str();
    config.credentials.client_id = credentials_.client_id.c_str();
    config.credentials.authentication.password = credentials_.password.c_str();
    config.session.disable_clean_session = false;
    config.session.keepalive = 60;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.network.reconnect_timeout_ms = 3000;
    config.network.timeout_ms = 10000;
    config.task.stack_size = 4096;
    config.buffer.size = 512;
    config.buffer.out_size = 512;

    ESP_LOGI(kTag, "start mqtt heap internal=%u largest=%u dma=%u dma_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));

    client_ = esp_mqtt_client_init(&config);
    if (!client_) {
        ESP_LOGE(kTag, "esp_mqtt_client_init failed");
        return false;
    }
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
    const esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_mqtt_client_start failed: %s heap internal=%u largest=%u dma=%u dma_largest=%u",
                 esp_err_to_name(err),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return false;
    }
    return true;
}

void AgentMqtt::Disconnect() {
    connected_ = false;
    if (client_) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    if (queue_mutex_ && xSemaphoreTake(queue_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        messages_.clear();
        incoming_topic_.clear();
        incoming_payload_.clear();
        incoming_total_ = 0;
        xSemaphoreGive(queue_mutex_);
    }
}

bool AgentMqtt::Publish(const std::string& topic, const std::string& payload) {
    last_publish_qos_ = 0;
    if (!client_) {
        last_publish_result_ = -1001;
        ESP_LOGW(kTag, "publish skipped: client missing");
        return false;
    }
    if (!connected_.load()) {
        last_publish_result_ = -1002;
        ESP_LOGW(kTag, "publish skipped: disconnected");
        return false;
    }
    if (topic.empty()) {
        last_publish_result_ = -1003;
        ESP_LOGW(kTag, "publish skipped: empty topic");
        return false;
    }
    if (payload.empty()) {
        last_publish_result_ = -1004;
        ESP_LOGW(kTag, "publish skipped: empty payload");
        return false;
    }
    const int msg_id = esp_mqtt_client_publish(client_, topic.c_str(), payload.data(),
                                               static_cast<int>(payload.size()), 0, 0);
    last_publish_result_ = msg_id;
    if (msg_id < 0) {
        ESP_LOGW(kTag, "publish failed topic=%s bytes=%d result=%d connected=%d",
                 topic.c_str(), static_cast<int>(payload.size()), msg_id,
                 connected_.load() ? 1 : 0);
    }
    return msg_id >= 0;
}

bool AgentMqtt::TakeMessage(Message* message) {
    if (!message || !queue_mutex_ || xSemaphoreTake(queue_mutex_, 0) != pdTRUE) {
        return false;
    }
    if (messages_.empty()) {
        xSemaphoreGive(queue_mutex_);
        return false;
    }
    *message = std::move(messages_.front());
    messages_.pop_front();
    xSemaphoreGive(queue_mutex_);
    return true;
}

void AgentMqtt::EventHandler(void* handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void* event_data) {
    (void)base;
    (void)event_id;
    auto* self = static_cast<AgentMqtt*>(handler_args);
    if (self && event_data) {
        self->HandleEvent(static_cast<esp_mqtt_event_handle_t>(event_data));
    }
}

void AgentMqtt::HandleEvent(esp_mqtt_event_handle_t event) {
    if (!event || event->client != client_) {
        return;
    }
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            connected_ = true;
            ESP_LOGI(kTag, "connected; subscribing to control");
            esp_mqtt_client_subscribe(event->client, credentials_.control_topic.c_str(), 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            connected_ = false;
            break;
        case MQTT_EVENT_DATA: {
            if (!queue_mutex_ || xSemaphoreTake(queue_mutex_, pdMS_TO_TICKS(25)) != pdTRUE) {
                return;
            }
            if (event->current_data_offset == 0) {
                incoming_topic_.assign(event->topic ? event->topic : "", event->topic_len);
                incoming_payload_.clear();
                incoming_total_ = event->total_data_len;
            }
            if (incoming_total_ <= 0 || incoming_total_ > static_cast<int>(kMaxMessageBytes) ||
                event->data_len < 0 || incoming_payload_.size() + static_cast<size_t>(event->data_len) > kMaxMessageBytes) {
                incoming_topic_.clear();
                incoming_payload_.clear();
                incoming_total_ = 0;
                xSemaphoreGive(queue_mutex_);
                return;
            }
            incoming_payload_.append(event->data, event->data_len);
            if (static_cast<int>(incoming_payload_.size()) >= incoming_total_) {
                EnqueueMessage(std::move(incoming_topic_), std::move(incoming_payload_));
                incoming_topic_.clear();
                incoming_payload_.clear();
                incoming_total_ = 0;
            }
            xSemaphoreGive(queue_mutex_);
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(kTag, "MQTT transport error");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(kTag, "published msg_id=%d", event->msg_id);
            break;
        default:
            break;
    }
}

void AgentMqtt::EnqueueMessage(std::string topic, std::string payload) {
    if (messages_.size() >= kMaxQueuedMessages) {
        messages_.pop_front();
    }
    messages_.push_back({std::move(topic), std::move(payload)});
}

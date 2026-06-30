#pragma once

#include <atomic>
#include <deque>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>

class AgentMqtt {
public:
    struct Credentials {
        std::string url;
        std::string username;
        std::string password;
        std::string client_id;
        std::string control_topic;
        std::string events_topic;
        std::string status_topic;

        bool Valid() const {
            return !url.empty() && !username.empty() && !password.empty() &&
                   !client_id.empty() && !control_topic.empty();
        }
    };

    struct Message {
        std::string topic;
        std::string payload;
    };

    AgentMqtt();
    ~AgentMqtt();

    bool Connect(const Credentials& credentials);
    void Disconnect();
    bool Publish(const std::string& topic, const std::string& payload);
    bool TakeMessage(Message* message);
    bool Connected() const { return client_ != nullptr && connected_.load(); }
    const Credentials& CurrentCredentials() const { return credentials_; }
    int LastPublishResult() const { return last_publish_result_.load(); }
    int LastPublishQos() const { return last_publish_qos_.load(); }

private:
    static void EventHandler(void* handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void* event_data);
    void HandleEvent(esp_mqtt_event_handle_t event);
    void EnqueueMessage(std::string topic, std::string payload);

    Credentials credentials_;
    esp_mqtt_client_handle_t client_ = nullptr;
    SemaphoreHandle_t queue_mutex_ = nullptr;
    std::deque<Message> messages_;
    std::string incoming_topic_;
    std::string incoming_payload_;
    int incoming_total_ = 0;
    std::atomic_bool connected_{false};
    std::atomic_int last_publish_result_{0};
    std::atomic_int last_publish_qos_{-1};
};

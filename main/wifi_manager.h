#pragma once

#include <string>

#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class WifiManager {
public:
    bool LoadCredentials();
    bool Connect();
    bool Connected() const { return connected_; }
    std::string IpAddress() const { return ip_address_; }
    std::string ConfiguredSsid() const { return ssid_; }
    bool HasConfiguredSsid() const { return !ssid_.empty(); }
    int LastDisconnectReason() const { return last_disconnect_reason_; }
    const char* LastDisconnectReasonText() const;

    static bool SaveCredentials(const std::string& ssid, const std::string& password);
    static bool ClearCredentials();

private:
    static void EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    EventGroupHandle_t event_group_ = nullptr;
    bool connected_ = false;
    std::string ip_address_;
    std::string ssid_;
    std::string password_;
    int last_disconnect_reason_ = 0;
};

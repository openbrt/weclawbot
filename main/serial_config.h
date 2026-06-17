#pragma once

#include <cstdint>

#include "note_store.h"
#include "ui.h"
#include "wechat_bot.h"
#include "wifi_manager.h"

class SerialConfig {
public:
    SerialConfig(Ui& ui, WifiManager& wifi, NoteStore& notes, WechatBot& bot)
        : ui_(ui), wifi_(wifi), notes_(notes), bot_(bot) {}

    bool Start();

private:
    static void TaskThunk(void* arg);

    bool InstallChannels();
    void Run();
    int ReadByte(uint8_t* out, int timeout_ms);
    void HandleLine(const char* line);
    void HandleGpioScan(const std::string& input);
    void SendStatus();
    void SendOk(const char* type, const char* message = nullptr);
    void SendError(const char* code, const char* message);
    bool HandleSet(const char* json);

    Ui& ui_;
    WifiManager& wifi_;
    NoteStore& notes_;
    WechatBot& bot_;
};

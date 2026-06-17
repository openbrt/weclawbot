#pragma once

#include <memory>

#include "button_wrapper.h"
#include "note_store.h"
#include "ui.h"
#include "wechat_bot.h"

class ButtonManager {
public:
    ButtonManager(Ui& ui, NoteStore& notes, WechatBot& bot)
        : ui_(ui), notes_(notes), bot_(bot) {}

    bool Init();

private:
    void HandleLeftShortPress();
    void HandleLeftLongPress();
    void HandleRightShortPress();
    void HandleRightLongPress();
    void RenderCurrentOrEmpty();
    void ClearTextNotes();
    void ClearAllAndRelogin();

    Ui& ui_;
    NoteStore& notes_;
    WechatBot& bot_;
    std::unique_ptr<GpioButton> left_button_;
    std::unique_ptr<GpioButton> right_button_;
    bool initialized_ = false;
};

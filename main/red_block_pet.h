#pragma once

#include <lvgl.h>

enum class RedBlockPetState {
    kIdle = 0,
    kListening,
    kThinking,
    kSpeaking,
    kSurprise,
    kSleep,
    kMessage,
};

void* RedBlockPetCreate(lv_obj_t* parent);
void RedBlockPetDestroy(void* handle);
void RedBlockPetSetState(void* handle, RedBlockPetState state);
void RedBlockPetSetEyeAngle(void* handle, float angle);
void RedBlockPetSetAudioLevel(void* handle, float db);
void RedBlockPetSetVadSpeaking(void* handle, bool talking);


#include "red_block_pet.h"

#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kIdleFrameMs = 100;
constexpr uint32_t kThinkingFrameMs = 50;

lv_coord_t Scale(float s, int v) {
    return static_cast<lv_coord_t>(std::lround(s * static_cast<float>(v)));
}

lv_coord_t RoundCoord(float v) {
    return static_cast<lv_coord_t>(std::lround(v));
}

float Clamp(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

lv_coord_t MinCoord(lv_coord_t a, lv_coord_t b) {
    return (a < b) ? a : b;
}

lv_color_t Ink() {
    return lv_color_black();
}

lv_color_t Paper() {
    return lv_color_white();
}

lv_obj_t* MakeObj(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* Box(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
              lv_color_t bg, lv_color_t border, lv_coord_t radius, lv_coord_t border_width) {
    lv_obj_t* obj = MakeObj(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, border_width, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

struct PetCore {
    lv_obj_t* root = nullptr;
    lv_obj_t* left_eye = nullptr;
    lv_obj_t* right_eye = nullptr;
    lv_obj_t* left_pupil = nullptr;
    lv_obj_t* right_pupil = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* thought_dots[3] = {};
    lv_coord_t center_x = 200;
    lv_coord_t home_y = 40;
    lv_coord_t width = 0;
    lv_coord_t height = 0;
    lv_coord_t left_eye_cx = 0;
    lv_coord_t right_eye_cx = 0;
    lv_coord_t eye_cy = 0;
    lv_coord_t eye_open_w = 0;
    lv_coord_t eye_open_h = 0;
    lv_coord_t eye_travel = 0;
    lv_coord_t pupil_size = 0;
    float scale = 1.0f;
    float phase = 0.2f;
};

struct RedBlockPetHandle {
    lv_obj_t* container = nullptr;
    lv_timer_t* anim_timer = nullptr;
    RedBlockPetState state = RedBlockPetState::kIdle;
    float eye_angle = 90.0f;
    float audio_level_db = -60.0f;
    bool vad_speaking = false;
    uint32_t state_started_ms = 0;
    uint32_t blink_started_ms = 0;
    uint32_t next_blink_ms = 0;
    PetCore pet;
};

void PlaceCentered(lv_obj_t* obj, lv_coord_t cx, lv_coord_t cy, lv_coord_t w, lv_coord_t h) {
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(obj, MinCoord(w, h) / 2, 0);
}

void CreateEyePair(PetCore* pet, lv_obj_t* parent,
                   lv_coord_t left_cx, lv_coord_t right_cx, lv_coord_t eye_cy,
                   lv_coord_t eye_size, lv_coord_t pupil_size, lv_coord_t eye_travel) {
    pet->left_eye = Box(parent, eye_size, eye_size, Paper(), Paper(), eye_size / 2, 0);
    pet->right_eye = Box(parent, eye_size, eye_size, Paper(), Paper(), eye_size / 2, 0);
    PlaceCentered(pet->left_eye, left_cx, eye_cy, eye_size, eye_size);
    PlaceCentered(pet->right_eye, right_cx, eye_cy, eye_size, eye_size);

    pet->left_pupil = Box(pet->left_eye, pupil_size, pupil_size, Ink(), Ink(), pupil_size / 2, 0);
    pet->right_pupil = Box(pet->right_eye, pupil_size, pupil_size, Ink(), Ink(), pupil_size / 2, 0);
    lv_obj_align(pet->left_pupil, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(pet->right_pupil, LV_ALIGN_CENTER, 0, 0);

    pet->left_eye_cx = left_cx;
    pet->right_eye_cx = right_cx;
    pet->eye_cy = eye_cy;
    pet->eye_open_w = eye_size;
    pet->eye_open_h = eye_size;
    pet->eye_travel = eye_travel;
    pet->pupil_size = pupil_size;
}

void ScheduleNextBlink(RedBlockPetHandle* handle, uint32_t now_ms) {
    const float seed = 0.5f + 0.5f * std::sinf(static_cast<float>(now_ms) * 0.0017f +
                                               handle->pet.phase * 2.1f);
    handle->next_blink_ms = now_ms + 3000u + static_cast<uint32_t>(std::lround(seed * 3000.0f));
}

float BlinkOpenFactor(RedBlockPetHandle* handle, uint32_t now_ms) {
    if (handle->state == RedBlockPetState::kSleep) return 0.14f;

    if (handle->blink_started_ms == 0 && now_ms >= handle->next_blink_ms) {
        handle->blink_started_ms = now_ms;
    }
    if (handle->blink_started_ms == 0) {
        return 1.0f;
    }

    const uint32_t elapsed = now_ms - handle->blink_started_ms;
    if (elapsed >= 140u) {
        handle->blink_started_ms = 0;
        ScheduleNextBlink(handle, now_ms);
        return 1.0f;
    }

    const float progress = static_cast<float>(elapsed) / 140.0f;
    return Clamp(std::fabs(2.0f * progress - 1.0f), 0.08f, 1.0f);
}

float BreathingGain(const RedBlockPetHandle* handle) {
    const float level = Clamp((handle->audio_level_db + 60.0f) / 60.0f, 0.0f, 1.0f);
    float gain = 1.0f + level * 0.45f;
    if (handle->state == RedBlockPetState::kThinking) gain += 0.15f;
    if (handle->state == RedBlockPetState::kSpeaking) gain += 0.08f;
    return gain;
}

float SurprisePop(const RedBlockPetHandle* handle, uint32_t now_ms) {
    if (handle->state != RedBlockPetState::kSurprise) return 1.0f;
    const float elapsed = static_cast<float>(now_ms - handle->state_started_ms) / 1000.0f;
    if (elapsed >= 0.60f) return 1.0f;
    return 1.0f + 0.5f * std::exp(-7.0f * elapsed) * std::fabs(std::cos(18.0f * elapsed));
}

void LayoutEye(lv_obj_t* eye, lv_obj_t* pupil, lv_coord_t cx, lv_coord_t cy,
               lv_coord_t eye_w, lv_coord_t eye_h, lv_coord_t pupil_size,
               lv_coord_t tx, lv_coord_t ty, bool hide_pupil) {
    if (!eye || !pupil) return;

    PlaceCentered(eye, cx, cy, eye_w, eye_h);
    lv_obj_set_size(pupil, pupil_size, pupil_size);
    lv_obj_align(pupil, LV_ALIGN_CENTER, 0, 0);
    if (hide_pupil) {
        lv_obj_add_flag(pupil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(pupil, 0, 0);
        lv_obj_set_style_translate_y(pupil, 0, 0);
    } else {
        lv_obj_clear_flag(pupil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(pupil, tx, 0);
        lv_obj_set_style_translate_y(pupil, ty, 0);
    }
}

void CreateRedBlock(RedBlockPetHandle* handle, lv_obj_t* parent) {
    PetCore& pet = handle->pet;
    const float scale = 1.15f;
    pet.center_x = 200;
    pet.home_y = 40;
    pet.width = Scale(scale, 104);
    pet.height = Scale(scale, 130);
    pet.scale = scale;
    pet.phase = 0.2f;

    pet.root = MakeObj(parent);
    lv_obj_set_size(pet.root, pet.width, pet.height);
    lv_obj_set_pos(pet.root, pet.center_x - pet.width / 2, pet.home_y);

    pet.body = Box(pet.root, Scale(scale, 82), Scale(scale, 96), Ink(), Ink(), Scale(scale, 20), 0);
    lv_obj_align(pet.body, LV_ALIGN_TOP_MID, 0, Scale(scale, 10));

    CreateEyePair(&pet, pet.body, Scale(scale, 24), Scale(scale, 58), Scale(scale, 40),
                  Scale(scale, 32), Scale(scale, 12), Scale(scale, 9));

    for (int i = 0; i < 3; ++i) {
        const lv_coord_t dot = Scale(scale, 7 + i * 2);
        pet.thought_dots[i] = Box(pet.root, dot, dot, Ink(), Ink(), dot / 2, 0);
        lv_obj_add_flag(pet.thought_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void UpdateThoughtDots(RedBlockPetHandle* handle, float t) {
    PetCore& pet = handle->pet;
    const bool thinking = handle->state == RedBlockPetState::kThinking;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* dot = pet.thought_dots[i];
        if (!dot) continue;
        if (!thinking) {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        const float lift = 0.5f + 0.5f * std::sinf(t * 3.1f + pet.phase + static_cast<float>(i) * 1.2f);
        const lv_coord_t x = Scale(pet.scale, 58 + i * 13);
        const lv_coord_t y = Scale(pet.scale, 22 - i * 9) - RoundCoord(lift * 4.0f);
        lv_obj_set_pos(dot, x, y);
        lv_obj_set_size(dot, Scale(pet.scale, 7 + i * 2), Scale(pet.scale, 7 + i * 2));
    }
}

void UpdateEyes(RedBlockPetHandle* handle, float t) {
    PetCore& pet = handle->pet;
    const uint32_t now_ms = lv_tick_get();
    float ex = 0.0f;
    float ey = 0.0f;

    switch (handle->state) {
        case RedBlockPetState::kListening:
            ex = Clamp((handle->eye_angle - 90.0f) / 90.0f * static_cast<float>(pet.eye_travel),
                       -static_cast<float>(pet.eye_travel), static_cast<float>(pet.eye_travel));
            ey = -0.5f;
            break;
        case RedBlockPetState::kThinking:
            ex = std::sinf(t * 8.5f + pet.phase) * (static_cast<float>(pet.eye_travel) * 0.95f);
            ey = std::cosf(t * 5.0f + pet.phase) * (static_cast<float>(pet.eye_travel) * 0.16f);
            break;
        case RedBlockPetState::kSpeaking:
            ex = std::sinf(t * 17.0f + pet.phase) * 1.2f +
                 std::cosf(t * 23.0f + pet.phase * 1.3f) * 0.7f;
            ey = std::cosf(t * 19.0f + pet.phase) * 0.9f;
            break;
        case RedBlockPetState::kSurprise:
            ex = 0.0f;
            ey = -1.0f;
            break;
        case RedBlockPetState::kSleep:
            ex = 0.0f;
            ey = 0.0f;
            break;
        case RedBlockPetState::kMessage:
            ex = 0.0f;
            ey = -static_cast<float>(pet.eye_travel) * 0.25f;
            break;
        case RedBlockPetState::kIdle:
        default:
            ex = std::sinf(t * 0.38f + pet.phase) * (static_cast<float>(pet.eye_travel) * 0.96f);
            ey = std::cosf(t * 0.27f + pet.phase * 0.7f) *
                 (static_cast<float>(pet.eye_travel) * 0.32f);
            break;
    }

    float eye_open = BlinkOpenFactor(handle, now_ms);
    if (handle->state == RedBlockPetState::kThinking) {
        eye_open = Clamp(eye_open * (0.92f + 0.08f * std::sinf(t * 5.0f + pet.phase)), 0.12f, 1.0f);
    }

    const lv_coord_t eye_w = pet.eye_open_w;
    const lv_coord_t eye_h = RoundCoord(std::fmax(2.0f, static_cast<float>(pet.eye_open_h) * eye_open));

    float pupil_scale = 1.0f + 0.08f * std::sinf(t * 1.8f + pet.phase);
    if (handle->state == RedBlockPetState::kSurprise) pupil_scale *= SurprisePop(handle, now_ms);
    if (handle->state == RedBlockPetState::kSpeaking) pupil_scale *= 1.05f;

    const lv_coord_t pupil_size = RoundCoord(std::fmax(3.0f, static_cast<float>(pet.pupil_size) * pupil_scale));
    const bool hide_pupil = (handle->state == RedBlockPetState::kSleep) || (eye_h <= 4);

    LayoutEye(pet.left_eye, pet.left_pupil, pet.left_eye_cx, pet.eye_cy, eye_w, eye_h,
              pupil_size, RoundCoord(ex), RoundCoord(ey), hide_pupil);
    LayoutEye(pet.right_eye, pet.right_pupil, pet.right_eye_cx, pet.eye_cy, eye_w, eye_h,
              pupil_size, RoundCoord(ex), RoundCoord(ey), hide_pupil);
}

void UpdateRedBlock(RedBlockPetHandle* handle, float t) {
    PetCore& pet = handle->pet;
    if (!pet.root) return;

    if (handle->vad_speaking) {
        lv_obj_set_pos(pet.root, pet.center_x - pet.width / 2, pet.home_y);
        handle->eye_angle = 90.0f;
        const RedBlockPetState original = handle->state;
        handle->state = RedBlockPetState::kListening;
        UpdateEyes(handle, t);
        handle->state = original;
        return;
    }

    const float speed = (handle->state == RedBlockPetState::kThinking) ? 2.0f : 1.0f;
    const float breath = BreathingGain(handle);
    const float bob = std::sinf(t * 1.25f * speed + pet.phase);
    const lv_coord_t x = pet.center_x - pet.width / 2 +
                         RoundCoord(std::sinf(t * 0.42f + pet.phase) * 5.0f);
    lv_coord_t y = pet.home_y + RoundCoord(bob * 7.0f * breath);
    if (handle->state == RedBlockPetState::kMessage) y += 8;
    lv_obj_set_pos(pet.root, x, y);
    UpdateEyes(handle, t);
    UpdateThoughtDots(handle, t);
}

void AnimationTimerCallback(lv_timer_t* timer) {
    auto* handle = static_cast<RedBlockPetHandle*>(lv_timer_get_user_data(timer));
    if (!handle || !handle->container) return;
    const float t = static_cast<float>(lv_tick_get()) / 1000.0f;
    UpdateRedBlock(handle, t);
}

}  // namespace

void* RedBlockPetCreate(lv_obj_t* parent) {
    auto* handle = new RedBlockPetHandle();
    handle->state = RedBlockPetState::kIdle;
    handle->eye_angle = 90.0f;
    handle->audio_level_db = -60.0f;
    handle->state_started_ms = lv_tick_get();
    ScheduleNextBlink(handle, handle->state_started_ms);

    handle->container = MakeObj(parent ? parent : lv_screen_active());
    lv_obj_set_size(handle->container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(handle->container, 0, 0);
    lv_obj_set_style_bg_color(handle->container, Paper(), 0);
    lv_obj_set_style_bg_opa(handle->container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(handle->container, LV_OBJ_FLAG_SCROLLABLE);

    CreateRedBlock(handle, handle->container);
    lv_obj_move_foreground(handle->container);
    handle->anim_timer = lv_timer_create(AnimationTimerCallback, kIdleFrameMs, handle);
    return handle;
}

void RedBlockPetDestroy(void* raw_handle) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    if (handle->anim_timer) {
        lv_timer_delete(handle->anim_timer);
        handle->anim_timer = nullptr;
    }
    if (handle->container) {
        lv_obj_delete(handle->container);
        handle->container = nullptr;
    }
    delete handle;
}

void RedBlockPetSetState(void* raw_handle, RedBlockPetState state) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    handle->state = state;
    handle->state_started_ms = lv_tick_get();
    if (state != RedBlockPetState::kSleep) {
        ScheduleNextBlink(handle, handle->state_started_ms);
    }
    handle->blink_started_ms = 0;
    if (handle->anim_timer) {
        const uint32_t period = state == RedBlockPetState::kThinking ? kThinkingFrameMs
                                : kIdleFrameMs;
        lv_timer_set_period(handle->anim_timer, period);
    }
}

void RedBlockPetSetPosition(void* raw_handle, int center_x, int home_y) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    handle->pet.center_x = static_cast<lv_coord_t>(center_x);
    handle->pet.home_y = static_cast<lv_coord_t>(home_y);
    if (handle->pet.root) {
        lv_obj_set_pos(handle->pet.root,
                       handle->pet.center_x - handle->pet.width / 2,
                       handle->pet.home_y);
    }
}

void RedBlockPetSetEyeAngle(void* raw_handle, float angle) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    handle->eye_angle = Clamp(angle, 0.0f, 180.0f);
}

void RedBlockPetSetAudioLevel(void* raw_handle, float db) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    handle->audio_level_db = Clamp(db, -60.0f, 0.0f);
}

void RedBlockPetSetVadSpeaking(void* raw_handle, bool talking) {
    if (!raw_handle) return;
    auto* handle = static_cast<RedBlockPetHandle*>(raw_handle);
    handle->vad_speaking = talking;
    if (talking && handle->state == RedBlockPetState::kIdle) {
        RedBlockPetSetState(raw_handle, RedBlockPetState::kListening);
    } else if (!talking && handle->state == RedBlockPetState::kListening) {
        RedBlockPetSetState(raw_handle, RedBlockPetState::kIdle);
    }
}

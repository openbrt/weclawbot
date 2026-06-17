#include "note_store.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <nvs.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "NoteStore";
constexpr char kNotesKey[] = "notes_json";
constexpr char kIndexKey[] = "cur";
constexpr char kStorageBasePath[] = "/storage";
constexpr char kScreenCachePath[] = "/storage/screen.bin";
constexpr char kScreenCacheTempPath[] = "/storage/screen.tmp";
constexpr char kIdlePhotoCachePath[] = "/storage/idle_photo.bin";
constexpr char kIdlePhotoCacheTempPath[] = "/storage/idle_photo.tmp";
constexpr uint32_t kScreenCacheMagic = 0x57454353;
constexpr uint32_t kScreenCacheVersion = 2;
constexpr uint32_t kMaxScreenCachePages = 3;

struct ScreenCacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t page_count;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t frame_bytes;
    uint32_t text_bytes;
    uint32_t revision_bytes;
    uint32_t render_id_bytes;
};

bool EnsureStorageMounted() {
    static bool attempted = false;
    static bool mounted = false;
    if (attempted) {
        return mounted;
    }
    attempted = true;
    const esp_vfs_spiffs_conf_t config = {
        .base_path = kStorageBasePath,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&config);
    mounted = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    if (!mounted) {
        ESP_LOGW(kTag, "SPIFFS mount failed: %s", esp_err_to_name(err));
    }
    return mounted;
}

void RemoveScreenCache() {
    if (!EnsureStorageMounted()) {
        return;
    }
    std::remove(kScreenCachePath);
    std::remove(kScreenCacheTempPath);
}

void RemoveIdlePhotoCache() {
    if (!EnsureStorageMounted()) {
        return;
    }
    std::remove(kIdlePhotoCachePath);
    std::remove(kIdlePhotoCacheTempPath);
}

bool SaveFrameCache(const Note* note,
                    const char* path,
                    const char* temp_path,
                    uint32_t max_pages,
                    uint32_t max_width,
                    uint32_t max_height) {
    if (!note || note->screen_frames.empty()) {
        std::remove(path);
        std::remove(temp_path);
        return true;
    }
    if (!EnsureStorageMounted() || note->screen_frames.size() > max_pages ||
        note->screen_frame_width <= 0 ||
        static_cast<uint32_t>(note->screen_frame_width) > max_width ||
        note->screen_frame_height <= 0 ||
        static_cast<uint32_t>(note->screen_frame_height) > max_height ||
        note->screen_frame_stride < (note->screen_frame_width + 7) / 8) {
        return false;
    }

    const size_t frame_bytes = static_cast<size_t>(note->screen_frame_stride) *
                               static_cast<size_t>(note->screen_frame_height);
    const std::string& text = note->text;
    const std::string& revision = note->screen_revision;
    const std::string& render_id = note->render_id;
    if (text.empty() || text.size() > CONFIG_WEC_MAX_NOTE_BYTES ||
        revision.size() > 80 || render_id.size() > 96) {
        return false;
    }
    for (const auto& frame : note->screen_frames) {
        if (frame.size() < frame_bytes) {
            return false;
        }
    }

    FILE* file = std::fopen(temp_path, "wb");
    if (!file) {
        return false;
    }
    const ScreenCacheHeader header = {
        .magic = kScreenCacheMagic,
        .version = kScreenCacheVersion,
        .page_count = static_cast<uint32_t>(note->screen_frames.size()),
        .width = static_cast<uint32_t>(note->screen_frame_width),
        .height = static_cast<uint32_t>(note->screen_frame_height),
        .stride = static_cast<uint32_t>(note->screen_frame_stride),
        .frame_bytes = static_cast<uint32_t>(frame_bytes),
        .text_bytes = static_cast<uint32_t>(text.size()),
        .revision_bytes = static_cast<uint32_t>(revision.size()),
        .render_id_bytes = static_cast<uint32_t>(render_id.size()),
    };
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
    ok = ok && std::fwrite(text.data(), text.size(), 1, file) == 1;
    if (!revision.empty()) {
        ok = ok && std::fwrite(revision.data(), revision.size(), 1, file) == 1;
    }
    if (!render_id.empty()) {
        ok = ok && std::fwrite(render_id.data(), render_id.size(), 1, file) == 1;
    }
    for (const auto& frame : note->screen_frames) {
        ok = ok && std::fwrite(frame.data(), frame_bytes, 1, file) == 1;
    }
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        std::remove(temp_path);
        return false;
    }
    std::remove(path);
    if (std::rename(temp_path, path) != 0) {
        std::remove(temp_path);
        return false;
    }
    return true;
}

bool LoadFrameCache(Note* note,
                    const char* path,
                    uint32_t max_pages,
                    uint32_t max_width,
                    uint32_t max_height,
                    bool remove_on_error) {
    if (!note || !EnsureStorageMounted()) {
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    ScreenCacheHeader header = {};
    bool ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
              header.magic == kScreenCacheMagic &&
              header.version == kScreenCacheVersion &&
              header.page_count > 0 && header.page_count <= max_pages &&
              header.width > 0 && header.width <= max_width &&
              header.height > 0 && header.height <= max_height &&
              header.stride >= (header.width + 7) / 8 &&
              header.frame_bytes == header.stride * header.height &&
              header.text_bytes > 0 && header.text_bytes <= CONFIG_WEC_MAX_NOTE_BYTES &&
              header.revision_bytes <= 80 && header.render_id_bytes <= 96;
    std::string text(header.text_bytes, '\0');
    std::string revision(header.revision_bytes, '\0');
    std::string render_id(header.render_id_bytes, '\0');
    if (ok) {
        ok = std::fread(text.data(), text.size(), 1, file) == 1;
    }
    if (ok && !revision.empty()) {
        ok = std::fread(revision.data(), revision.size(), 1, file) == 1;
    }
    if (ok && !render_id.empty()) {
        ok = std::fread(render_id.data(), render_id.size(), 1, file) == 1;
    }
    std::vector<std::vector<uint8_t>> frames;
    if (ok) {
        frames.resize(header.page_count);
        for (auto& frame : frames) {
            frame.resize(header.frame_bytes);
            ok = ok && std::fread(frame.data(), header.frame_bytes, 1, file) == 1;
        }
    }
    std::fclose(file);
    if (!ok) {
        if (remove_on_error) {
            std::remove(path);
        }
        return false;
    }
    note->screen_frames = std::move(frames);
    note->text = std::move(text);
    note->screen_revision = std::move(revision);
    note->render_id = std::move(render_id);
    note->screen_frame_width = static_cast<int>(header.width);
    note->screen_frame_height = static_cast<int>(header.height);
    note->screen_frame_stride = static_cast<int>(header.stride);
    return true;
}

bool SaveScreenCache(const Note* note) {
    if (!note || note->screen_frames.empty()) {
        RemoveScreenCache();
        return true;
    }
    return SaveFrameCache(note, kScreenCachePath, kScreenCacheTempPath,
                          kMaxScreenCachePages, WEC_CONTENT_BITMAP_WIDTH,
                          WEC_CONTENT_BITMAP_HEIGHT);
}

bool LoadScreenCache(Note* note) {
    const bool ok = LoadFrameCache(note, kScreenCachePath, kMaxScreenCachePages,
                                   WEC_CONTENT_BITMAP_WIDTH, WEC_CONTENT_BITMAP_HEIGHT,
                                   false);
    if (!ok) {
        RemoveScreenCache();
    }
    return ok;
}

bool IsUtf8Continuation(uint8_t c) {
    return (c & 0xC0) == 0x80;
}
}  // namespace

bool NoteStore::Load() {
    notes_.clear();
    current_index_ = 0;
    idle_photo_ = Note{};
    idle_photo_valid_ = LoadFrameCache(&idle_photo_, kIdlePhotoCachePath, 1,
                                       WEC_RLCD_WIDTH, WEC_RLCD_HEIGHT, true);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_NOTES_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t len = 0;
    err = nvs_get_str(nvs, kNotesKey, nullptr, &len);
    if (err == ESP_OK && len > 1) {
        std::string json(len, '\0');
        err = nvs_get_str(nvs, kNotesKey, json.data(), &len);
        if (err == ESP_OK) {
            if (!json.empty() && json.back() == '\0') {
                json.pop_back();
            }
            cJSON* root = cJSON_Parse(json.c_str());
            if (cJSON_IsArray(root)) {
                const int count = cJSON_GetArraySize(root);
                for (int i = 0; i < count; ++i) {
                    cJSON* item = cJSON_GetArrayItem(root, i);
                    cJSON* text = cJSON_GetObjectItem(item, "text");
                    cJSON* from = cJSON_GetObjectItem(item, "from");
                    cJSON* time_label = cJSON_GetObjectItem(item, "time");
                    cJSON* screen_revision = cJSON_GetObjectItem(item, "screen_revision");
                    cJSON* render_id = cJSON_GetObjectItem(item, "render_id");
                    cJSON* created_at = cJSON_GetObjectItem(item, "created");
                    if (!cJSON_IsString(text)) {
                        continue;
                    }
                    Note note;
                    note.text = text->valuestring;
                    note.from = cJSON_IsString(from) ? from->valuestring : "";
                    note.time_label = cJSON_IsString(time_label) ? time_label->valuestring : "";
                    note.screen_revision = cJSON_IsString(screen_revision) ? screen_revision->valuestring : "";
                    note.render_id = cJSON_IsString(render_id) ? render_id->valuestring : "";
                    note.created_at = cJSON_IsNumber(created_at) ? static_cast<int64_t>(created_at->valuedouble) : 0;
                    notes_.push_back(std::move(note));
                }
            }
            cJSON_Delete(root);
        }
    }

    uint32_t idx = 0;
    if (nvs_get_u32(nvs, kIndexKey, &idx) == ESP_OK && idx < notes_.size()) {
        current_index_ = idx;
    }
    nvs_close(nvs);

    const bool compacted = notes_.size() > 1;
    if (compacted) {
        Note active = notes_[current_index_ < notes_.size() ? current_index_ : 0];
        notes_.clear();
        notes_.push_back(std::move(active));
        current_index_ = 0;
    }
    if (!notes_.empty()) {
        LoadScreenCache(&notes_[0]);
    }
    if (compacted) {
        Save();
    }
    return true;
}

bool NoteStore::Save() const {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEC_NOTES_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }

    cJSON* root = cJSON_CreateArray();
    for (const auto& note : notes_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", note.text.c_str());
        cJSON_AddStringToObject(item, "from", note.from.c_str());
        cJSON_AddStringToObject(item, "time", note.time_label.c_str());
        cJSON_AddStringToObject(item, "screen_revision", note.screen_revision.c_str());
        cJSON_AddStringToObject(item, "render_id", note.render_id.c_str());
        cJSON_AddNumberToObject(item, "created", static_cast<double>(note.created_at));
        cJSON_AddItemToArray(root, item);
    }

    char* raw = cJSON_PrintUnformatted(root);
    std::string json = raw ? raw : "[]";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    err = nvs_set_str(nvs, kNotesKey, json.c_str());
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, kIndexKey, static_cast<uint32_t>(current_index_));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Save failed: %s", esp_err_to_name(err));
        return false;
    }
    const Note* active = notes_.empty() ? nullptr : &notes_[current_index_];
    if (!SaveScreenCache(active)) {
        ESP_LOGW(kTag, "Screen cache save failed");
        return false;
    }
    return true;
}

void NoteStore::Add(std::string text, std::string from) {
    Note note;
    note.created_at = std::time(nullptr);
    note.time_label = MakeTimeLabel(note.created_at);
    note.from = TrimForStorage(from, 80);
    note.text = TrimForStorage(text, CONFIG_WEC_MAX_NOTE_BYTES);

    notes_.clear();
    notes_.push_back(std::move(note));
    current_index_ = 0;
    Save();
}

void NoteStore::AddRendered(std::string text,
                            std::string from,
                            std::string screen_revision,
                            std::string render_id,
                            std::vector<std::vector<uint8_t>> frames,
                            int width,
                            int height,
                            int stride) {
    Note note;
    note.created_at = std::time(nullptr);
    note.time_label = MakeTimeLabel(note.created_at);
    note.from = TrimForStorage(from, 80);
    note.text = TrimForStorage(text, CONFIG_WEC_MAX_NOTE_BYTES);
    note.screen_revision = TrimForStorage(screen_revision, 80);
    note.render_id = TrimForStorage(render_id, 96);
    note.screen_frames = std::move(frames);
    note.screen_frame_width = width;
    note.screen_frame_height = height;
    note.screen_frame_stride = stride;

    notes_.clear();
    notes_.push_back(std::move(note));
    current_index_ = 0;
    Save();
}

bool NoteStore::ReplaceCurrent(std::string text, std::string from) {
    if (notes_.empty() || current_index_ >= notes_.size()) {
        return false;
    }
    notes_[current_index_].text = TrimForStorage(text, CONFIG_WEC_MAX_NOTE_BYTES);
    if (!from.empty()) {
        notes_[current_index_].from = TrimForStorage(from, 80);
    }
    notes_[current_index_].created_at = std::time(nullptr);
    notes_[current_index_].time_label = MakeTimeLabel(notes_[current_index_].created_at);
    notes_[current_index_].screen_frames.clear();
    notes_[current_index_].screen_frame_width = 0;
    notes_[current_index_].screen_frame_height = 0;
    notes_[current_index_].screen_frame_stride = 0;
    Save();
    return true;
}

bool NoteStore::Next(bool persist) {
    if (notes_.empty()) {
        return false;
    }
    current_index_ = (current_index_ + 1) % notes_.size();
    if (persist) {
        Save();
    }
    return true;
}

bool NoteStore::Previous(bool persist) {
    if (notes_.empty()) {
        return false;
    }
    current_index_ = current_index_ == 0 ? notes_.size() - 1 : current_index_ - 1;
    if (persist) {
        Save();
    }
    return true;
}

bool NoteStore::ClearCurrent() {
    if (notes_.empty()) {
        return false;
    }
    notes_.erase(notes_.begin() + current_index_);
    if (current_index_ >= notes_.size() && !notes_.empty()) {
        current_index_ = notes_.size() - 1;
    }
    if (notes_.empty()) {
        current_index_ = 0;
    }
    Save();
    return true;
}

void NoteStore::ClearAll() {
    notes_.clear();
    current_index_ = 0;
    Save();
}

bool NoteStore::SetIdlePhoto(std::string text,
                             std::string from,
                             std::string screen_revision,
                             std::string render_id,
                             std::vector<std::vector<uint8_t>> frames,
                             int width,
                             int height,
                             int stride) {
    if (frames.empty() || width <= 0 || width > WEC_RLCD_WIDTH ||
        height <= 0 || height > WEC_RLCD_HEIGHT || stride < (width + 7) / 8) {
        return false;
    }

    Note photo;
    photo.created_at = std::time(nullptr);
    photo.time_label = MakeTimeLabel(photo.created_at);
    photo.from = TrimForStorage(from, 80);
    photo.text = TrimForStorage(text.empty() ? "照片相框" : text, CONFIG_WEC_MAX_NOTE_BYTES);
    photo.screen_revision = TrimForStorage(screen_revision, 80);
    photo.render_id = TrimForStorage(render_id, 96);
    photo.screen_frames = std::move(frames);
    photo.screen_frame_width = width;
    photo.screen_frame_height = height;
    photo.screen_frame_stride = stride;

    if (!SaveFrameCache(&photo, kIdlePhotoCachePath, kIdlePhotoCacheTempPath,
                        1, WEC_RLCD_WIDTH, WEC_RLCD_HEIGHT)) {
        ESP_LOGW(kTag, "Idle photo cache save failed");
        return false;
    }
    idle_photo_ = std::move(photo);
    idle_photo_valid_ = true;
    return true;
}

void NoteStore::ClearIdlePhoto() {
    idle_photo_ = Note{};
    idle_photo_valid_ = false;
    RemoveIdlePhotoCache();
}

const Note* NoteStore::Current() const {
    if (notes_.empty() || current_index_ >= notes_.size()) {
        return nullptr;
    }
    return &notes_[current_index_];
}

std::string NoteStore::TrimForStorage(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }
    size_t cut = max_bytes;
    while (cut > 0 && IsUtf8Continuation(static_cast<uint8_t>(value[cut]))) {
        --cut;
    }
    std::string out = value.substr(0, cut);
    out += "...";
    return out;
}

std::string NoteStore::MakeTimeLabel(int64_t timestamp) {
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm tm = {};
    localtime_r(&t, &tm);
    if (tm.tm_year < 120) {
        return "";
    }
    char buf[32];
    std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm);
    return buf;
}

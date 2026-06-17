#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Note {
    std::string text;
    std::string from;
    std::string time_label;
    std::string screen_revision;
    std::string render_id;
    int64_t created_at = 0;
    std::vector<std::vector<uint8_t>> screen_frames;
    int screen_frame_width = 0;
    int screen_frame_height = 0;
    int screen_frame_stride = 0;
};

class NoteStore {
public:
    bool Load();
    bool Save() const;

    void Add(std::string text, std::string from);
    void AddRendered(std::string text,
                     std::string from,
                     std::string screen_revision,
                     std::string render_id,
                     std::vector<std::vector<uint8_t>> frames,
                     int width,
                     int height,
                     int stride);
    bool ReplaceCurrent(std::string text, std::string from);
    bool Next(bool persist = true);
    bool Previous(bool persist = true);
    bool ClearCurrent();
    void ClearAll();
    bool SetIdlePhoto(std::string text,
                      std::string from,
                      std::string screen_revision,
                      std::string render_id,
                      std::vector<std::vector<uint8_t>> frames,
                      int width,
                      int height,
                      int stride);
    void ClearIdlePhoto();

    bool Empty() const { return notes_.empty(); }
    size_t Count() const { return notes_.size(); }
    size_t CurrentIndex() const { return current_index_; }
    const std::vector<Note>& All() const { return notes_; }
    const Note* Current() const;
    bool HasIdlePhoto() const { return idle_photo_valid_; }
    const Note* IdlePhoto() const { return idle_photo_valid_ ? &idle_photo_ : nullptr; }

private:
    static std::string TrimForStorage(const std::string& value, size_t max_bytes);
    static std::string MakeTimeLabel(int64_t timestamp);

    std::vector<Note> notes_;
    size_t current_index_ = 0;
    Note idle_photo_;
    bool idle_photo_valid_ = false;
};

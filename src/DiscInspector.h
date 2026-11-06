#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

struct ChapterInfo {
    int index = 0;
    std::string name;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct TrackInfo {
    int index = 0;
    std::string codec;
    std::string language;
    std::string type;        // "video", "audio", "subtitle"
    int64_t duration_ms = 0;
    int channels = 0;        // audio
    int width = 0;           // video/subtitle canvas
    int height = 0;          // video/subtitle canvas
    int stream_id = -1;      // MPEG-PS substream ID or MPEG-TS PID
    int code_extension = 0;  // DVD language/usage extension
    std::array<uint32_t, 16> palette{}; // DVD YCrCb subtitle palette
    bool has_palette = false;
    bool selected = false;
};

struct TitleInfo {
    int index = 0;
    std::string name;
    int64_t duration_ms = 0;
    int chapter_count = 0;
    std::vector<ChapterInfo> chapters;
    std::vector<TrackInfo> tracks;
    bool include_chapters = true;
    bool selected = false;
};

struct DiscInfo {
    std::string source_path;
    std::string disc_type;   // "DVD-Video", "Blu-ray", "ISO Image"
    std::string volume_label;
    std::vector<TitleInfo> titles;
    bool valid = false;
    std::string error;
};

class DiscInspector {
public:
    DiscInfo inspectDirectory(const std::string& path);
    DiscInfo inspectImage(const std::string& isoPath);

private:
    DiscInfo inspectDvd(const std::string& videoTsPath);
    DiscInfo inspectBluRay(const std::string& bdmvPath);
    DiscInfo inspectDvdIso(const std::string& isoPath);
    DiscInfo inspectBluRayIso(const std::string& isoPath);
    std::string detectVolumeLabel(const std::string& path);
};

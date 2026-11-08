#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

enum class SourceKind {
    Unknown,
    Folder,
    IsoImage,
    OpticalDrive
};

enum class DiscKind {
    Unknown,
    DvdVideo,
    BluRay
};

struct TrackOutputOptions {
    bool selected = false;
    bool mkv_default = false;
    bool mkv_forced = false;
};

struct TitleOutputOptions {
    bool selected = false;
    bool include_chapters = true;
};

struct DvdTitleIdentity {
    int vts = 0;
    int ttn = 0;
    int pgc = 0;
    int angle = 1;
    int angle_count = 1;
};

struct BlurayTitleIdentity {
    uint32_t playlist = 0;
    std::vector<std::string> clip_ids;
};

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
    bool forced_from_source = false;
    TrackOutputOptions output;
};

struct TitleInfo {
    int index = 0;
    std::string name;
    int64_t duration_ms = 0;
    int chapter_count = 0;
    std::vector<ChapterInfo> chapters;
    std::vector<TrackInfo> tracks;
    DvdTitleIdentity dvd;
    BlurayTitleIdentity bluray;
    TitleOutputOptions output;
};

struct DiscInfo {
    std::string source_path;
    SourceKind source_kind = SourceKind::Unknown;
    DiscKind disc_kind = DiscKind::Unknown;
    std::string volume_label;
    std::vector<TitleInfo> titles;
    bool valid = false;
    std::string error;
};

inline const char* sourceKindName(SourceKind kind) {
    switch (kind) {
    case SourceKind::Folder: return "folder";
    case SourceKind::IsoImage: return "iso";
    case SourceKind::OpticalDrive: return "optical-drive";
    default: return "unknown";
    }
}

inline std::string discTypeLabel(DiscKind discKind, SourceKind sourceKind) {
    std::string label;
    switch (discKind) {
    case DiscKind::DvdVideo: label = "DVD-Video"; break;
    case DiscKind::BluRay: label = "Blu-ray"; break;
    default: label = "Desconhecido"; break;
    }
    if (sourceKind == SourceKind::IsoImage) label += " (ISO)";
    if (sourceKind == SourceKind::OpticalDrive) label += " (unidade)";
    return label;
}

class DiscInspector {
public:
    DiscInfo inspectDirectory(const std::string& path);
    DiscInfo inspectImage(const std::string& isoPath);
    DiscInfo inspectOpticalDrive(const std::string& path);

private:
    DiscInfo inspectDvd(const std::string& videoTsPath);
    DiscInfo inspectBluRay(const std::string& bdmvPath);
    DiscInfo inspectDvdIso(const std::string& isoPath);
    DiscInfo inspectBluRayIso(const std::string& isoPath);
    std::string detectVolumeLabel(const std::string& path);
};

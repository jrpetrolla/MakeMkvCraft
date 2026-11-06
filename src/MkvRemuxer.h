#pragma once

#include "DiscInspector.h"
#include <cstdint>
#include <functional>
#include <string>
#include <fstream>
#include <mutex>
#include <vector>

class Logger {
public:
    static Logger& instance();
    void init(const std::string& exePath);
    void log(const std::string& msg);
    void close();
private:
    std::mutex m_mutex;
    std::ofstream m_file;
    bool m_enabled = false;
};

#define LOG(msg) Logger::instance().log(msg)

class MkvRemuxer {
public:
    using ProgressCallback = std::function<void(int, const std::string&)>;

    explicit MkvRemuxer(ProgressCallback progressCallback = {});
    bool remuxTitle(const DiscInfo& disc, const TitleInfo& title, const std::string& outputPath, std::string& errorMsg);
    int remuxSelected(const DiscInfo& disc, const std::string& outputFolder, std::string& errorMsg);

private:
    struct DvdTimestampSegment {
        int64_t byte_offset = 0;
        int64_t raw_start_90k = 0;
        int64_t raw_end_90k = 0;
        int64_t adjusted_start_90k = 0;
        int cell_number = 0;
        int vobu_number = 0;

        int64_t adjustedEnd90k() const {
            return adjusted_start_90k + (raw_end_90k - raw_start_90k);
        }

        int64_t timestampOffset90k() const {
            return adjusted_start_90k - raw_start_90k;
        }
    };

    struct DvdClockSegment {
        int64_t byte_offset = 0;
        int64_t timestamp_offset_90k = 0;
        int64_t raw_scr_90k = 0;
        int64_t adjusted_scr_90k = 0;
        int cell_number = 0;
        int vobu_number = 0;
    };

    std::string buildOutputFileName(const DiscInfo& disc, const TitleInfo& title);
    void reportTitleProgress(int titlePermille, const std::string& phase,
                             int phasePermille);
    bool remuxWithLibav(const std::string& inputPath, const std::string& outputPath,
                        const TitleInfo& title, std::string& errorMsg,
                        const std::vector<DvdClockSegment>& dvdClockTimeline = {},
                        int titleProgressStart = 0, int titleProgressEnd = 1000);
    bool remuxDvdIso(const DiscInfo& disc, const TitleInfo& title,
                     const std::string& outputPath, std::string& errorMsg);

    ProgressCallback m_progressCallback;
    int64_t m_completedWork = 0;
    int64_t m_currentTitleWork = 1;
    int64_t m_totalWork = 1;
    int m_currentTitleOrdinal = 0;
    int m_selectedTitleCount = 0;
};

#include "MkvRemuxer.h"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <format>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

// FFmpeg's MPEG-PS demuxer sets this internal flag when it discovers a stream.
// Subtitle streams declared by the DVD IFO are created after probing, so the
// application must request the same full-frame parser explicitly. FFmpeg 9.0.2
// exports this symbol from avformat-63.dll.
void avpriv_stream_set_need_parsing(AVStream* stream, enum AVStreamParseType type);
}

namespace fs = std::filesystem;

static constexpr int64_t adaptiveDtsCorrection(int64_t previousDts,
                                               int64_t observedDts,
                                               bool allowEqualDts) {
    const int64_t minimumDts = previousDts + (allowEqualDts ? 0 : 1);
    return observedDts < minimumDts ? minimumDts - observedDts : 0;
}

static constexpr bool canFinalizeTerminalAdaptiveBoundaryAtEof(
    size_t observedContinuousAnchors,
    size_t invalidPositionedStreams,
    size_t invalidPositionlessStreams) {
    return observedContinuousAnchors > 0 &&
        invalidPositionedStreams == 0 &&
        invalidPositionlessStreams == 0;
}

static constexpr bool isEarlierClockSegment(size_t packetSegment,
                                            size_t boundarySegment) {
    return packetSegment < boundarySegment;
}

static constexpr bool shouldExposeTerminalScrCandidate(
    bool candidatePending, int64_t candidateByteOffset, int64_t totalBytes,
    int64_t previousSegmentByteOffset) {
    return candidatePending && candidateByteOffset > previousSegmentByteOffset &&
        candidateByteOffset < totalBytes;
}

static_assert(adaptiveDtsCorrection(31043, 31041, true) == 2);
static_assert(adaptiveDtsCorrection(27971, 27943, true) == 28);
static_assert(adaptiveDtsCorrection(5954529, 297, true) == 5954232);
static_assert(adaptiveDtsCorrection(1000, 1000, true) == 0);
static_assert(adaptiveDtsCorrection(1000, 1000, false) == 1);
static_assert(adaptiveDtsCorrection(1000, 1002, true) == 0);
static_assert(canFinalizeTerminalAdaptiveBoundaryAtEof(1, 0, 0));
static_assert(!canFinalizeTerminalAdaptiveBoundaryAtEof(0, 0, 0));
static_assert(!canFinalizeTerminalAdaptiveBoundaryAtEof(1, 1, 0));
static_assert(!canFinalizeTerminalAdaptiveBoundaryAtEof(1, 0, 1));
static_assert(isEarlierClockSegment(8, 9));
static_assert(!isEarlierClockSegment(9, 9));
static_assert(!isEarlierClockSegment(10, 9));
static_assert(shouldExposeTerminalScrCandidate(
    true, 3827357696, 3827363840, 0));
static_assert(!shouldExposeTerminalScrCandidate(
    true, 3827363840, 3827363840, 0));
static_assert(!shouldExposeTerminalScrCandidate(
    false, 3827357696, 3827363840, 0));

// --- Logger ---

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(const std::string& exePath) {
    std::scoped_lock lock(m_mutex);
    fs::path p(exePath);
    std::string logPath = (p.parent_path() / "log.txt").string();
    m_file.open(logPath, std::ios::app);
    m_enabled = m_file.is_open();
    if (m_enabled) {
        m_file << "\n========== Session " << timestamp() << " ==========\n";
        m_file.flush();
    }
}

void Logger::log(const std::string& msg) {
    std::scoped_lock lock(m_mutex);
    if (!m_enabled) return;
    m_file << "[" << timestamp() << "] " << msg << "\n";
    m_file.flush();
}

void Logger::close() {
    std::scoped_lock lock(m_mutex);
    if (m_file.is_open()) m_file.close();
    m_enabled = false;
}

MkvRemuxer::MkvRemuxer(ProgressCallback progressCallback)
    : m_progressCallback(std::move(progressCallback))
{
}

void MkvRemuxer::reportTitleProgress(int titlePermille, const std::string& phase,
                                     int phasePermille) {
    if (!m_progressCallback) return;

    titlePermille = std::clamp(titlePermille, 0, 1000);
    phasePermille = std::clamp(phasePermille, 0, 1000);
    int64_t overallNumerator =
        m_completedWork * 1000 + m_currentTitleWork * titlePermille;
    int overallPermille = static_cast<int>(std::clamp<int64_t>(
        overallNumerator / std::max<int64_t>(m_totalWork, 1), 0, 1000));
    std::string status = std::format(
        "Título {}/{} — {}: {:.1f}%", m_currentTitleOrdinal,
        m_selectedTitleCount, phase, phasePermille / 10.0);
    m_progressCallback(overallPermille, status);
}

static const char* trackTypeForMediaType(AVMediaType type) {
    switch (type) {
    case AVMEDIA_TYPE_VIDEO: return "video";
    case AVMEDIA_TYPE_AUDIO: return "audio";
    case AVMEDIA_TYPE_SUBTITLE: return "subtitle";
    default: return "";
    }
}

static const TrackInfo* findTrackInfo(const TitleInfo& title, const AVStream* stream,
                                      std::vector<bool>& usedTracks) {
    const char* type = trackTypeForMediaType(stream->codecpar->codec_type);
    if (!type[0]) return nullptr;

    for (size_t i = 0; i < title.tracks.size(); ++i) {
        const TrackInfo& track = title.tracks[i];
        if (!usedTracks[i] && track.type == type && track.stream_id >= 0 &&
            track.stream_id == stream->id) {
            usedTracks[i] = true;
            return &track;
        }
    }
    for (size_t i = 0; i < title.tracks.size(); ++i) {
        const TrackInfo& track = title.tracks[i];
        if (!usedTracks[i] && track.type == type && track.stream_id < 0) {
            usedTracks[i] = true;
            return &track;
        }
    }
    return nullptr;
}

static uint32_t dvdYCrCbToRgb(uint32_t yuv) {
    double y = (yuv >> 16) & 0xff;
    double cr = (yuv >> 8) & 0xff;
    double cb = yuv & 0xff;
    int r = static_cast<int>(1.164 * (y - 16) + 1.596 * (cr - 128));
    int g = static_cast<int>(1.164 * (y - 16) - 0.392 * (cb - 128) -
                             0.813 * (cr - 128));
    int b = static_cast<int>(1.164 * (y - 16) + 2.017 * (cb - 128));
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return static_cast<uint32_t>((r << 16) | (g << 8) | b);
}

static std::string buildVobSubCodecPrivate(const TrackInfo& track) {
    std::ostringstream out;
    out << "size: " << track.width << 'x' << track.height << "\n"
        << "org: 0, 0\n"
        << "scale: 100%, 100%\n"
        << "alpha: 100%\n"
        << "smooth: OFF\n"
        << "fadein/out: 50, 50\n"
        << "align: OFF at LEFT TOP\n"
        << "time offset: 0\n"
        << "forced subs: OFF\n"
        << "palette: ";
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < track.palette.size(); ++i) {
        if (i) out << ", ";
        out << std::setw(6) << dvdYCrCbToRgb(track.palette[i]);
    }
    out << "\ncustom colors: OFF, tridx: 0000, "
        << "colors: 000000, 000000, 000000, 000000\n";
    return out.str();
}

static bool setVobSubCodecPrivate(AVCodecParameters* parameters,
                                  const TrackInfo& track) {
    if (!parameters || !track.has_palette || track.width <= 0 || track.height <= 0) {
        return false;
    }
    std::string codecPrivate = buildVobSubCodecPrivate(track);
    uint8_t* data = static_cast<uint8_t*>(
        av_mallocz(codecPrivate.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!data) return false;
    std::memcpy(data, codecPrivate.data(), codecPrivate.size());
    av_freep(&parameters->extradata);
    parameters->extradata = data;
    parameters->extradata_size = static_cast<int>(codecPrivate.size());
    parameters->width = track.width;
    parameters->height = track.height;
    return true;
}

static bool addChaptersToOutput(AVFormatContext* output, const TitleInfo& title,
                                std::string& errorMsg) {
    if (!title.output.include_chapters) {
        LOG("remuxWithLibav: chapters omitted by user selection");
        return true;
    }

    std::vector<const ChapterInfo*> validChapters;
    for (const ChapterInfo& chapter : title.chapters) {
        if (chapter.start_ms >= 0 && chapter.end_ms > chapter.start_ms) {
            validChapters.push_back(&chapter);
        }
    }
    if (validChapters.empty()) return true;

    output->chapters = static_cast<AVChapter**>(
        av_calloc(validChapters.size(), sizeof(*output->chapters)));
    if (!output->chapters) {
        errorMsg = "Falha ao alocar capítulos do MKV.";
        return false;
    }

    for (const ChapterInfo* source : validChapters) {
        AVChapter* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(*chapter)));
        if (!chapter) {
            errorMsg = "Falha ao alocar capítulo do MKV.";
            return false;
        }
        chapter->id = source->index;
        chapter->time_base = AVRational{1, 1000};
        chapter->start = source->start_ms;
        chapter->end = source->end_ms;
        av_dict_set(&chapter->metadata, "title", source->name.c_str(), 0);
        output->chapters[output->nb_chapters++] = chapter;
    }
    LOG("remuxWithLibav: added " + std::to_string(output->nb_chapters) +
        " chapter(s) to MKV");
    return true;
}

static bool prepareDvdSubtitleStreams(AVFormatContext* input, const TitleInfo& title,
                                      std::string& errorMsg) {
    std::set<int> expectedSubtitleIds;
    for (const TrackInfo& track : title.tracks) {
        if (track.type == "subtitle" && track.codec == "DVD-VOBSub" &&
            track.output.selected && track.stream_id >= 0) {
            expectedSubtitleIds.insert(track.stream_id);
        }
    }
    if (expectedSubtitleIds.empty()) return true;

    int created = 0;
    for (int expectedId : expectedSubtitleIds) {
        bool found = false;
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            AVStream* stream = input->streams[i];
            if (stream->id == expectedId &&
                stream->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
                found = true;
                break;
            }
        }
        if (found) {
            for (unsigned i = 0; i < input->nb_streams; ++i) {
                AVStream* stream = input->streams[i];
                if (stream->id == expectedId &&
                    stream->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
                    avpriv_stream_set_need_parsing(stream, AVSTREAM_PARSE_FULL);
                    LOG("remuxWithLibav: enabled full DVD subtitle parser, index=" +
                        std::to_string(stream->index) + ", id=" +
                        std::format("0x{:x}", expectedId));
                    break;
                }
            }
            continue;
        }

        AVStream* stream = avformat_new_stream(input, nullptr);
        if (!stream) {
            errorMsg = "Falha ao preparar stream de legenda DVD.";
            LOG("remuxWithLibav: failed to create declared DVD subtitle stream id=" +
                std::format("0x{:x}", expectedId));
            return false;
        }
        stream->id = expectedId;
        stream->time_base = AVRational{1, 90000};
        stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        stream->codecpar->codec_id = AV_CODEC_ID_DVD_SUBTITLE;
        avpriv_stream_set_need_parsing(stream, AVSTREAM_PARSE_FULL);
        created++;
        LOG("remuxWithLibav: prepared DVD subtitle stream from IFO, index=" +
            std::to_string(stream->index) + ", id=" +
            std::format("0x{:x}", expectedId) + ", parser=full");
    }
    LOG("remuxWithLibav: DVD subtitle streams ready, declared=" +
        std::to_string(expectedSubtitleIds.size()) + ", created=" +
        std::to_string(created));
    return true;
}

static bool parseMpeg2PackScr90k(const uint8_t* data, int32_t length, int64_t& scr90k) {
    if (!data || length < 10 || data[0] != 0x00 || data[1] != 0x00 ||
        data[2] != 0x01 || data[3] != 0xba) {
        return false;
    }

    // MPEG-2 pack header: '01' prefix plus four marker bits.
    if ((data[4] & 0xc4) != 0x44 || (data[6] & 0x04) == 0 ||
        (data[8] & 0x04) == 0 || (data[9] & 0x01) == 0) {
        return false;
    }

    scr90k = (static_cast<int64_t>(data[4] & 0x38) << 27) |
             (static_cast<int64_t>(data[4] & 0x03) << 28) |
             (static_cast<int64_t>(data[5]) << 20) |
             (static_cast<int64_t>(data[6] >> 3) << 15) |
             (static_cast<int64_t>(data[6] & 0x03) << 13) |
             (static_cast<int64_t>(data[7]) << 5) |
             (data[8] >> 3);
    return true;
}

// --- DVD remux: follow the logical title for ISO, folder, or optical drive ---

bool MkvRemuxer::remuxDvdTitle(const DiscInfo& disc, const TitleInfo& title,
                                const std::string& outputPath, std::string& errorMsg) {
    LOG("remuxDvdIso: start, source=" + disc.source_path +
        ", title=" + std::to_string(title.index) +
        ", vts=" + std::to_string(title.dvd.vts) +
        ", ttn=" + std::to_string(title.dvd.ttn) +
        ", pgc=" + std::to_string(title.dvd.pgc) +
        ", inspected_angles=" + std::to_string(title.dvd.angle_count));

    dvdnav_t* dvdnav = nullptr;
    if (dvdnav_open(&dvdnav, disc.source_path.c_str()) != DVDNAV_STATUS_OK || !dvdnav) {
        errorMsg = "Não foi possível abrir a origem DVD com libdvdnav.";
        LOG("remuxDvdIso: dvdnav_open failed");
        return false;
    }
    LOG("remuxDvdIso: dvdnav_open OK");

    auto closeDvdnav = [&]() {
        if (dvdnav) {
            dvdnav_close(dvdnav);
            dvdnav = nullptr;
        }
    };

    int32_t titleCount = 0;
    if (dvdnav_get_number_of_titles(dvdnav, &titleCount) != DVDNAV_STATUS_OK ||
        title.index < 1 || title.index > titleCount) {
        errorMsg = "Título DVD inválido.";
        LOG("remuxDvdIso: invalid title index, available=" + std::to_string(titleCount));
        closeDvdnav();
        return false;
    }

    if (dvdnav_set_PGC_positioning_flag(dvdnav, 1) != DVDNAV_STATUS_OK) {
        errorMsg = "Não foi possível ativar posicionamento por PGC no DVD.";
        LOG("remuxDvdIso: dvdnav_set_PGC_positioning_flag failed");
        closeDvdnav();
        return false;
    }

    if (dvdnav_title_play(dvdnav, title.index) != DVDNAV_STATUS_OK) {
        errorMsg = "Não foi possível iniciar o título DVD selecionado.";
        LOG("remuxDvdIso: dvdnav_title_play failed: " +
            std::string(dvdnav_err_to_string(dvdnav)));
        closeDvdnav();
        return false;
    }

    int32_t currentAngle = 1;
    int32_t angleCount = 1;
    int32_t selectedAngle = 1;
    if (dvdnav_get_angle_info(dvdnav, &currentAngle, &angleCount) == DVDNAV_STATUS_OK &&
        angleCount > 0) {
        if (title.dvd.angle < 1 || title.dvd.angle > angleCount) {
            errorMsg = "Ângulo selecionado não existe neste título DVD.";
            LOG("remuxDvdIso: invalid selected angle=" +
                std::to_string(title.dvd.angle) + ", available=" +
                std::to_string(angleCount));
            closeDvdnav();
            return false;
        }
        selectedAngle = title.dvd.angle;
        if (dvdnav_angle_change(dvdnav, selectedAngle) != DVDNAV_STATUS_OK) {
            errorMsg = "Não foi possível selecionar o ângulo do título DVD.";
            LOG("remuxDvdIso: dvdnav_angle_change failed");
            closeDvdnav();
            return false;
        }
        if (angleCount != title.dvd.angle_count) {
            LOG("remuxDvdIso: angle count changed after inspection, inspected=" +
                std::to_string(title.dvd.angle_count) + ", playback=" +
                std::to_string(angleCount));
        }
    }
    LOG("remuxDvdIso: title playback started, angles=" + std::to_string(angleCount) +
        ", selected=" + std::to_string(selectedAngle));

    // Create temp VOB file next to output
    fs::path outPath(outputPath);
    fs::path tempPath = outPath.parent_path() / (outPath.stem().string() + "_temp.vob");
    LOG("remuxDvdIso: temp file = " + tempPath.string());

    FILE* tempFile = nullptr;
#ifdef _WIN32
    tempFile = _wfopen(tempPath.wstring().c_str(), L"wb");
#else
    tempFile = fopen(tempPath.string().c_str(), "wb");
#endif
    if (!tempFile) {
        errorMsg = "Não foi possível criar arquivo temporário: " + tempPath.string();
        LOG("remuxDvdIso: fopen temp failed");
        closeDvdnav();
        return false;
    }

    std::vector<DvdTimestampSegment> dvdTimeline;
    std::vector<DvdClockSegment> dvdClockTimeline;
    std::vector<uint8_t> buf(DVD_VIDEO_LB_LEN);
    int64_t total_bytes = 0;
    int64_t block_count = 0;
    int64_t written_block_count = 0;
    int64_t omitted_nav_block_count = 0;
    int64_t omitted_invalid_pack_count = 0;
    int64_t nav_count = 0;
    int discontinuity_count = 0;
    int scr_discontinuity_count = 0;
    int current_cell = 0;
    int current_program = 0;
    int last_chapter_pgcn = -1;
    int last_chapter_pgn = -1;
    std::vector<ChapterInfo> playbackChapters;
    bool selected_title_seen = false;
    bool finished = false;
    bool extraction_ok = true;
    constexpr int kMaxConsecutiveNavReadErrors = 500;
    int consecutive_nav_read_errors = 0;
    int nav_read_recoveries = 0;
    int lastExtractionPermille = -1;
    constexpr int64_t kMpegTimestampWrap90k = INT64_C(1) << 33;
    constexpr int64_t kMaxContinuousScrDelta90k = 700 * 90;
    constexpr int64_t kMaxBackwardScrJitter90k = 10 * 90;
    constexpr int kScrConfirmationSamples = 8;
    int64_t scr_unwrap_offset_90k = 0;
    int64_t last_observed_scr_90k = AV_NOPTS_VALUE;
    int64_t last_stable_scr_90k = AV_NOPTS_VALUE;
    int64_t last_adjusted_scr_90k = AV_NOPTS_VALUE;
    int64_t last_normal_scr_delta_90k = 1;
    int64_t current_scr_offset_90k = 0;
    bool pending_scr_discontinuity = false;
    int64_t pending_scr_byte_offset = 0;
    int64_t pending_raw_scr_90k = 0;
    int64_t pending_last_scr_90k = 0;
    int64_t pending_scr_delta_90k = 0;
    int pending_scr_confirmation_samples = 0;
    int pending_scr_cell = 0;
    int pending_scr_vobu = 0;
    int scr_candidate_replacements = 0;
    int scr_candidate_rejections = 0;
    int scr_terminal_provisional_segments = 0;

    auto selectedTitleIsActive = [&]() {
        int32_t activeTitle = 0;
        int32_t activePart = 0;
        if (dvdnav_current_title_info(dvdnav, &activeTitle, &activePart) != DVDNAV_STATUS_OK) {
            return false;
        }
        if (activeTitle == title.index) {
            selected_title_seen = true;
            return true;
        }
        if (selected_title_seen) {
            finished = true;
        }
        return false;
    };

    auto reportExtractionProgress = [&](bool complete) {
        int phasePermille = 1000;
        if (!complete) {
            uint32_t position = 0;
            uint32_t length = 0;
            if (dvdnav_get_position(dvdnav, &position, &length) != DVDNAV_STATUS_OK ||
                length == 0) {
                return;
            }
            phasePermille = static_cast<int>(std::clamp<int64_t>(
                static_cast<int64_t>(position) * 1000 / length, 0, 999));
        }
        if (phasePermille <= lastExtractionPermille) return;
        lastExtractionPermille = phasePermille;
        reportTitleProgress(phasePermille / 2, "Extraindo DVD", phasePermille);
    };

    auto processDvdBlock = [&](const uint8_t* data, int32_t length,
                               bool writePayload, const char* blockKind) {
        if (length != DVD_VIDEO_LB_LEN) {
            LOG("remuxDvdIso: unexpected DVD block length=" + std::to_string(length));
            return false;
        }

        const int64_t sourceBlockNumber = block_count + 1;
        const bool isMpegPack = data[0] == 0x00 && data[1] == 0x00 &&
            data[2] == 0x01 && data[3] == 0xba;

        int64_t rawScr90k = 0;
        if (isMpegPack && parseMpeg2PackScr90k(data, length, rawScr90k)) {
            int64_t unwrappedScr90k = rawScr90k + scr_unwrap_offset_90k;
            if (last_observed_scr_90k != AV_NOPTS_VALUE) {
                int64_t wrappedDelta = unwrappedScr90k - last_observed_scr_90k;
                if (wrappedDelta < -(kMpegTimestampWrap90k / 2)) {
                    scr_unwrap_offset_90k += kMpegTimestampWrap90k;
                    unwrappedScr90k += kMpegTimestampWrap90k;
                } else if (wrappedDelta > kMpegTimestampWrap90k / 2) {
                    scr_unwrap_offset_90k -= kMpegTimestampWrap90k;
                    unwrappedScr90k -= kMpegTimestampWrap90k;
                }
            }

            auto isContinuousScrDelta = [&](int64_t delta90k) {
                return delta90k <= kMaxContinuousScrDelta90k &&
                       delta90k >= -kMaxBackwardScrJitter90k;
            };
            auto acceptContinuousScr = [&](int64_t stableDelta90k) {
                last_adjusted_scr_90k =
                    unwrappedScr90k + current_scr_offset_90k;
                if (stableDelta90k > 0) {
                    last_normal_scr_delta_90k = stableDelta90k;
                }
                last_stable_scr_90k = unwrappedScr90k;
            };

            if (last_stable_scr_90k == AV_NOPTS_VALUE) {
                current_scr_offset_90k = -unwrappedScr90k;
                last_adjusted_scr_90k = 0;
                last_stable_scr_90k = unwrappedScr90k;
                dvdClockTimeline.push_back({
                    total_bytes, current_scr_offset_90k, unwrappedScr90k,
                    last_adjusted_scr_90k, current_cell, static_cast<int>(nav_count)
                });
                LOG("remuxDvdIso: SCR timeline started, raw_scr_90k=" +
                    std::to_string(unwrappedScr90k) + ", byte_offset=" +
                    std::to_string(total_bytes));
            } else if (!pending_scr_discontinuity) {
                const int64_t stableDelta90k =
                    unwrappedScr90k - last_stable_scr_90k;
                if (isContinuousScrDelta(stableDelta90k)) {
                    acceptContinuousScr(stableDelta90k);
                } else {
                    pending_scr_discontinuity = true;
                    pending_scr_byte_offset = total_bytes;
                    pending_raw_scr_90k = unwrappedScr90k;
                    pending_last_scr_90k = unwrappedScr90k;
                    pending_scr_delta_90k = stableDelta90k;
                    pending_scr_confirmation_samples = 0;
                    pending_scr_cell = current_cell;
                    pending_scr_vobu = static_cast<int>(nav_count);
                    LOG("remuxDvdIso: SCR discontinuity candidate at byte=" +
                        std::to_string(total_bytes) + ", block=" +
                        std::to_string(sourceBlockNumber) + ", VOBU=" +
                        std::to_string(nav_count) + ", cell=" +
                        std::to_string(current_cell) + ", delta_90k=" +
                        std::to_string(stableDelta90k));
                }
            } else {
                const int64_t confirmationDelta90k =
                    unwrappedScr90k - pending_last_scr_90k;
                if (isContinuousScrDelta(confirmationDelta90k)) {
                    pending_last_scr_90k = unwrappedScr90k;
                    pending_scr_confirmation_samples++;
                    if (pending_scr_confirmation_samples >=
                        kScrConfirmationSamples) {
                        const int64_t expectedDelta90k = std::clamp<int64_t>(
                            last_normal_scr_delta_90k, 1,
                            kMaxContinuousScrDelta90k);
                        last_adjusted_scr_90k += expectedDelta90k;
                        const int64_t previousOffset90k = current_scr_offset_90k;
                        current_scr_offset_90k =
                            last_adjusted_scr_90k - pending_raw_scr_90k;
                        dvdClockTimeline.push_back({
                            pending_scr_byte_offset, current_scr_offset_90k,
                            pending_raw_scr_90k, last_adjusted_scr_90k,
                            pending_scr_cell, pending_scr_vobu
                        });
                        scr_discontinuity_count++;
                        LOG("remuxDvdIso: SCR discontinuity at byte=" +
                            std::to_string(pending_scr_byte_offset) + ", VOBU=" +
                            std::to_string(pending_scr_vobu) + ", cell=" +
                            std::to_string(pending_scr_cell) + ", delta_90k=" +
                            std::to_string(pending_scr_delta_90k) +
                            ", confirmation_samples=" +
                            std::to_string(pending_scr_confirmation_samples) +
                            ", last_confirmation_delta_90k=" +
                            std::to_string(confirmationDelta90k) +
                            ", previous_offset_90k=" +
                            std::to_string(previousOffset90k) +
                            ", new_offset_90k=" +
                            std::to_string(current_scr_offset_90k));

                        pending_scr_discontinuity = false;
                        acceptContinuousScr(confirmationDelta90k);
                    }
                } else {
                    const int64_t stableDelta90k =
                        unwrappedScr90k - last_stable_scr_90k;
                    if (isContinuousScrDelta(stableDelta90k)) {
                        scr_candidate_rejections++;
                        LOG("remuxDvdIso: rejected transient SCR candidate at byte=" +
                            std::to_string(pending_scr_byte_offset) +
                            ", candidate_delta_90k=" +
                            std::to_string(pending_scr_delta_90k) +
                            ", resumed_delta_90k=" +
                            std::to_string(stableDelta90k));
                        pending_scr_discontinuity = false;
                        acceptContinuousScr(stableDelta90k);
                    } else {
                        scr_candidate_replacements++;
                        LOG("remuxDvdIso: replacing unconfirmed SCR candidate at byte=" +
                            std::to_string(pending_scr_byte_offset) +
                            " with byte=" + std::to_string(total_bytes) +
                            ", candidate_delta_90k=" +
                            std::to_string(pending_scr_delta_90k) +
                            ", replacement_delta_90k=" +
                            std::to_string(stableDelta90k));
                        pending_scr_byte_offset = total_bytes;
                        pending_raw_scr_90k = unwrappedScr90k;
                        pending_last_scr_90k = unwrappedScr90k;
                        pending_scr_delta_90k = stableDelta90k;
                        pending_scr_confirmation_samples = 0;
                        pending_scr_cell = current_cell;
                        pending_scr_vobu = static_cast<int>(nav_count);
                    }
                }
            }
            last_observed_scr_90k = unwrappedScr90k;
        }

        block_count++;
        if (!isMpegPack) {
            omitted_invalid_pack_count++;
            if (omitted_invalid_pack_count <= 10) {
                LOG("remuxDvdIso: omitting non-MPEG-pack DVD block, block=" +
                    std::to_string(sourceBlockNumber) + ", kind=" + blockKind +
                    ", prefix=" + std::format("{:02x}{:02x}{:02x}{:02x}",
                        data[0], data[1], data[2], data[3]));
            }
            if (block_count % 256 == 0) reportExtractionProgress(false);
            return true;
        }

        // NAV packs are consumed above to build the title clock, chapters and
        // discontinuity map. They are data-only for Matroska and were discarded
        // later by the muxer, so do not feed them to the generic MPEG-PS demuxer.
        // This also prevents expensive byte-by-byte resynchronization on damaged
        // or protection-generated NAV regions.
        if (!writePayload) {
            omitted_nav_block_count++;
            if (block_count % 256 == 0) reportExtractionProgress(false);
            return true;
        }

        if (fwrite(data, 1, static_cast<size_t>(length), tempFile) !=
            static_cast<size_t>(length)) {
            LOG("remuxDvdIso: fwrite failed after " + std::to_string(total_bytes) + " bytes");
            return false;
        }
        total_bytes += length;
        written_block_count++;
        if (block_count % 256 == 0) {
            reportExtractionProgress(false);
        }
        return true;
    };

    reportTitleProgress(0, "Extraindo DVD", 0);
    while (!finished) {
        int32_t event = DVDNAV_NOP;
        int32_t length = 0;
        if (dvdnav_get_next_block(dvdnav, buf.data(), &event, &length) != DVDNAV_STATUS_OK) {
            const std::string readError = dvdnav_err_to_string(dvdnav);
            consecutive_nav_read_errors++;
            LOG("remuxDvdIso: dvdnav_get_next_block failed: " + readError +
                ", recovery attempt=" + std::to_string(consecutive_nav_read_errors));

            if (consecutive_nav_read_errors > kMaxConsecutiveNavReadErrors) {
                errorMsg = "Falha ao navegar pelo título DVD após " +
                    std::to_string(kMaxConsecutiveNavReadErrors) +
                    " tentativas consecutivas: " + readError;
                LOG("remuxDvdIso: too many consecutive dvdnav read errors");
                extraction_ok = false;
                break;
            }

            // Some mastered/protected DVDs contain a bogus next-VOBU pointer.
            // Move to the next logical sector and let libdvdnav resynchronize at
            // the nearest VOBU boundary, as done by HandBrake's dvdnav reader.
            if (dvdnav_sector_search(dvdnav, 1, SEEK_CUR) != DVDNAV_STATUS_OK) {
                const std::string seekError = dvdnav_err_to_string(dvdnav);
                errorMsg = "Falha ao recuperar navegação do título DVD: " + seekError;
                LOG("remuxDvdIso: dvdnav_sector_search recovery failed: " + seekError);
                extraction_ok = false;
                break;
            }

            nav_read_recoveries++;
            continue;
        }
        switch (event) {
        case DVDNAV_BLOCK_OK:
            consecutive_nav_read_errors = 0;
            if (selectedTitleIsActive() &&
                !processDvdBlock(buf.data(), length, true, "payload")) {
                extraction_ok = false;
                finished = true;
            }
            break;

        case DVDNAV_NAV_PACKET:
            consecutive_nav_read_errors = 0;
            if (!selectedTitleIsActive()) {
                break;
            }
            {
                const pci_t* pci = dvdnav_get_current_nav_pci(dvdnav);
                if (!pci) {
                    errorMsg = "Pacote NAV do DVD sem informação PCI.";
                    LOG("remuxDvdIso: NAV packet without PCI");
                    extraction_ok = false;
                    finished = true;
                    break;
                }

                int64_t rawStart = static_cast<int64_t>(pci->pci_gi.vobu_s_ptm);
                int64_t rawEnd = static_cast<int64_t>(pci->pci_gi.vobu_e_ptm);
                if (rawEnd <= rawStart) {
                    errorMsg = "Intervalo temporal inválido em VOBU do DVD.";
                    LOG("remuxDvdIso: invalid VOBU PTM, start=" + std::to_string(rawStart) +
                        ", end=" + std::to_string(rawEnd));
                    extraction_ok = false;
                    finished = true;
                    break;
                }

                int64_t adjustedStart = dvdTimeline.empty()
                    ? 0
                    : dvdTimeline.back().adjustedEnd90k();
                if (!dvdTimeline.empty() && rawStart != dvdTimeline.back().raw_end_90k) {
                    discontinuity_count++;
                    LOG("remuxDvdIso: NAV discontinuity at VOBU " +
                        std::to_string(nav_count + 1) + ", cell=" +
                        std::to_string(current_cell) + ", previous_raw_end=" +
                        std::to_string(dvdTimeline.back().raw_end_90k) +
                        ", current_raw_start=" + std::to_string(rawStart) +
                        ", delta_90k=" +
                        std::to_string(rawStart - dvdTimeline.back().raw_end_90k));
                }

                dvdTimeline.push_back({
                    total_bytes,
                    rawStart,
                    rawEnd,
                    adjustedStart,
                    current_cell,
                    static_cast<int>(nav_count + 1)
                });
                nav_count++;

                if (!processDvdBlock(buf.data(), length, false, "NAV")) {
                    extraction_ok = false;
                    finished = true;
                }
            }
            break;

        case DVDNAV_CELL_CHANGE:
            {
                const auto* cell = reinterpret_cast<const dvdnav_cell_change_event_t*>(buf.data());
                current_cell = cell->cellN;
                current_program = cell->pgN;
                bool selectedTitleActive = selectedTitleIsActive();
                int32_t activeTitle = 0;
                int32_t pgcn = 0;
                int32_t pgn = 0;
                if (selectedTitleActive &&
                    dvdnav_current_title_program(dvdnav, &activeTitle, &pgcn, &pgn) ==
                        DVDNAV_STATUS_OK &&
                    activeTitle == title.index && pgcn > 0 && pgn > 0 &&
                    (pgcn != last_chapter_pgcn || pgn != last_chapter_pgn)) {
                    int64_t chapterStart90k = dvdTimeline.empty()
                        ? 0
                        : dvdTimeline.back().adjustedEnd90k();
                    ChapterInfo chapter;
                    chapter.index = static_cast<int>(playbackChapters.size()) + 1;
                    chapter.name = "Capítulo " + std::to_string(chapter.index);
                    chapter.start_ms = av_rescale_q(
                        chapterStart90k, AVRational{1, 90000}, AVRational{1, 1000});
                    playbackChapters.push_back(chapter);
                    last_chapter_pgcn = pgcn;
                    last_chapter_pgn = pgn;
                }
                LOG("remuxDvdIso: cell change, cell=" + std::to_string(current_cell) +
                    ", program=" + std::to_string(current_program) +
                    ", cell_start=" + std::to_string(cell->cell_start) +
                    ", cell_length=" + std::to_string(cell->cell_length));
            }
            break;

        case DVDNAV_STILL_FRAME:
            if (dvdnav_still_skip(dvdnav) != DVDNAV_STATUS_OK) {
                errorMsg = "Falha ao ignorar quadro estático do DVD.";
                extraction_ok = false;
                finished = true;
            }
            break;

        case DVDNAV_WAIT:
            if (dvdnav_wait_skip(dvdnav) != DVDNAV_STATUS_OK) {
                errorMsg = "Falha ao continuar após espera de navegação do DVD.";
                extraction_ok = false;
                finished = true;
            }
            break;

        case DVDNAV_STOP:
            finished = true;
            break;

        default:
            break;
        }
    }

    if (extraction_ok && !dvdClockTimeline.empty() &&
        shouldExposeTerminalScrCandidate(
            pending_scr_discontinuity, pending_scr_byte_offset, total_bytes,
            dvdClockTimeline.back().byte_offset)) {
        // EOF can arrive before the eight-SCR confirmation window. Expose the
        // byte boundary without trusting the incomplete SCR sample: retain the
        // previous base offset and let positioned media DTS anchors decide the
        // one global adaptive correction during muxing.
        dvdClockTimeline.push_back({
            pending_scr_byte_offset,
            current_scr_offset_90k,
            pending_raw_scr_90k,
            pending_raw_scr_90k + current_scr_offset_90k,
            pending_scr_cell,
            pending_scr_vobu
        });
        scr_terminal_provisional_segments++;
        LOG("remuxDvdIso: exposing terminal unconfirmed SCR candidate as "
            "provisional adaptive boundary, byte=" +
            std::to_string(pending_scr_byte_offset) + ", VOBU=" +
            std::to_string(pending_scr_vobu) + ", cell=" +
            std::to_string(pending_scr_cell) + ", candidate_delta_90k=" +
            std::to_string(pending_scr_delta_90k) +
            ", confirmation_samples=" +
            std::to_string(pending_scr_confirmation_samples) +
            ", payload_bytes=" +
            std::to_string(total_bytes - pending_scr_byte_offset) +
            ", inherited_offset_90k=" +
            std::to_string(current_scr_offset_90k));
    }

    if (fclose(tempFile) != 0) {
        LOG("remuxDvdIso: fclose temp failed");
        extraction_ok = false;
    }
    closeDvdnav();

    LOG("remuxDvdIso: extraction done, bytes=" + std::to_string(total_bytes) +
        ", source blocks=" + std::to_string(block_count) +
        ", written blocks=" + std::to_string(written_block_count) +
        ", NAV blocks omitted=" + std::to_string(omitted_nav_block_count) +
        ", invalid packs omitted=" + std::to_string(omitted_invalid_pack_count) +
        ", VOBUs=" + std::to_string(nav_count) +
        ", NAV discontinuities=" + std::to_string(discontinuity_count) +
        ", SCR discontinuities=" + std::to_string(scr_discontinuity_count) +
        ", SCR segments=" + std::to_string(dvdClockTimeline.size()) +
        ", SCR candidate replacements=" +
        std::to_string(scr_candidate_replacements) +
        ", SCR candidate rejections=" +
        std::to_string(scr_candidate_rejections) +
        ", SCR terminal provisional segments=" +
        std::to_string(scr_terminal_provisional_segments) +
        ", dvdnav recoveries=" + std::to_string(nav_read_recoveries));

    if (!extraction_ok || !selected_title_seen || total_bytes <= 0 || dvdTimeline.empty() ||
        dvdClockTimeline.empty()) {
        if (errorMsg.empty()) {
            errorMsg = "Extração lógica do título DVD incompleta.";
        }
        std::error_code ec;
        fs::remove(tempPath, ec);
        return false;
    }

    bool timelineValid = dvdTimeline.front().byte_offset == 0;
    for (size_t i = 0; timelineValid && i < dvdTimeline.size(); ++i) {
        const auto& segment = dvdTimeline[i];
        timelineValid = segment.raw_end_90k > segment.raw_start_90k;
        if (i > 0) {
            const auto& previous = dvdTimeline[i - 1];
            timelineValid = timelineValid &&
                segment.byte_offset > previous.byte_offset &&
                segment.adjusted_start_90k == previous.adjustedEnd90k();
        }
    }
    if (!timelineValid) {
        errorMsg = "Linha do tempo NAV do DVD é inconsistente.";
        LOG("remuxDvdIso: timeline validation failed");
        std::error_code ec;
        fs::remove(tempPath, ec);
        return false;
    }
    if (dvdClockTimeline.front().byte_offset != 0) {
        errorMsg = "O primeiro bloco do título DVD não contém um relógio SCR válido.";
        LOG("remuxDvdIso: SCR timeline does not start at byte zero, first_offset=" +
            std::to_string(dvdClockTimeline.front().byte_offset));
        std::error_code ec;
        fs::remove(tempPath, ec);
        return false;
    }
    reportExtractionProgress(true);

    int64_t timelineDurationMs =
        av_rescale_q(dvdTimeline.back().adjustedEnd90k(), AVRational{1, 90000},
                     AVRational{1, 1000});
    LOG("remuxDvdIso: timeline validated, duration_ms=" +
        std::to_string(timelineDurationMs) + ", inspected_duration_ms=" +
        std::to_string(title.duration_ms));

    TitleInfo remuxTitleInfo = title;
    remuxTitleInfo.duration_ms = timelineDurationMs;
    if (!playbackChapters.empty()) {
        for (size_t i = 0; i < playbackChapters.size(); ++i) {
            playbackChapters[i].end_ms = i + 1 < playbackChapters.size()
                ? playbackChapters[i + 1].start_ms
                : timelineDurationMs;
        }
        playbackChapters.erase(
            std::remove_if(playbackChapters.begin(), playbackChapters.end(),
                           [](const ChapterInfo& chapter) {
                               return chapter.end_ms <= chapter.start_ms;
                           }),
            playbackChapters.end());
        for (size_t i = 0; i < playbackChapters.size(); ++i) {
            playbackChapters[i].index = static_cast<int>(i + 1);
            playbackChapters[i].name = "Capítulo " + std::to_string(i + 1);
        }
        remuxTitleInfo.chapters = playbackChapters;
        remuxTitleInfo.chapter_count = static_cast<int>(playbackChapters.size());
    }
    LOG("remuxDvdIso: playback chapters=" +
        std::to_string(remuxTitleInfo.chapters.size()) +
        ", inspected chapters=" + std::to_string(title.chapters.size()));

    // Remux the temp VOB to MKV
    LOG("remuxDvdIso: starting remux of temp file to MKV");
    bool ok = remuxWithLibav(
        tempPath.string(), outputPath, remuxTitleInfo, errorMsg,
        dvdClockTimeline, 500, 1000);

    // Clean up temp file
    std::error_code ec;
    fs::remove(tempPath, ec);
    LOG("remuxDvdIso: temp file removed, remux result=" + std::string(ok ? "OK" : "FAIL"));

    return ok;
}

// --- Generic remux with libav ---

std::string MkvRemuxer::buildOutputFileName(const DiscInfo& disc, const TitleInfo& title) {
    std::string label = disc.volume_label;
    if (label.empty()) label = "output";
    for (auto& c : label) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    return std::format("{} - T{:02d}.mkv", label, title.index);
}

bool MkvRemuxer::remuxWithLibav(const std::string& inputPath, const std::string& outputPath,
                                 const TitleInfo& title, std::string& errorMsg,
                                 const std::vector<DvdClockSegment>& dvdClockTimeline,
                                 int titleProgressStart, int titleProgressEnd) {
    LOG("remuxWithLibav: input=" + inputPath + ", output=" + outputPath +
        ", expected_duration_ms=" + std::to_string(title.duration_ms) +
        ", DVD SCR segments=" + std::to_string(dvdClockTimeline.size()));
    reportTitleProgress(titleProgressStart, "Muxando MKV", 0);

    AVFormatContext* inCtx = avformat_alloc_context();
    if (!inCtx) {
        errorMsg = "Falha ao criar contexto de entrada.";
        return false;
    }
    // libavformat's GENPTS lookahead is unbounded. Across a DVD clock reset it can
    // retain the rest of the title while waiting for a raw DTS greater than the
    // pre-reset value. DVD input uses a bounded, SCR-aware lookahead below.
    const bool useBoundedDvdGenPts = !dvdClockTimeline.empty();
    if (!useBoundedDvdGenPts) {
        inCtx->flags |= AVFMT_FLAG_GENPTS;
    }
    int openRet = avformat_open_input(&inCtx, inputPath.c_str(), nullptr, nullptr);
    if (openRet < 0) {
        char errbuf[256];
        av_strerror(openRet, errbuf, sizeof(errbuf));
        errorMsg = "Falha ao abrir entrada: " + inputPath;
        LOG("remuxWithLibav: avformat_open_input failed: " + std::string(errbuf));
        avformat_close_input(&inCtx);
        return false;
    }
    int64_t inputSizeBytes = inCtx->pb ? avio_size(inCtx->pb) : -1;
    LOG("remuxWithLibav: input size bytes=" + std::to_string(inputSizeBytes));
    LOG("remuxWithLibav: PTS generation=" + std::string(
        useBoundedDvdGenPts ? "bounded SCR-aware lookahead" : "libavformat GENPTS"));

    if (avformat_find_stream_info(inCtx, nullptr) < 0) {
        errorMsg = "Falha ao obter informações de stream.";
        LOG("remuxWithLibav: avformat_find_stream_info failed");
        avformat_close_input(&inCtx);
        return false;
    }
    LOG("remuxWithLibav: initially found " + std::to_string(inCtx->nb_streams) +
        " streams");
    if (!prepareDvdSubtitleStreams(inCtx, title, errorMsg)) {
        avformat_close_input(&inCtx);
        return false;
    }
    LOG("remuxWithLibav: stream discovery complete, streams=" +
        std::to_string(inCtx->nb_streams));

    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", outputPath.c_str()) < 0 || !outCtx) {
        errorMsg = "Falha ao criar contexto de saída MKV.";
        LOG("remuxWithLibav: avformat_alloc_output_context2 failed");
        avformat_close_input(&inCtx);
        return false;
    }

    std::vector<int> inputToOutput(inCtx->nb_streams, -1);
    std::vector<bool> usedTrackMetadata(title.tracks.size(), false);
    int mappedStreamCount = 0;
    int referenceVideoInputStream = -1;
    std::vector<int> adaptiveAnchorInputStreams;
    for (unsigned i = 0; i < inCtx->nb_streams; i++) {
        AVStream* inStream = inCtx->streams[i];
        AVCodecParameters* inPar = inStream->codecpar;

        const AVCodecDescriptor* desc = avcodec_descriptor_get(inPar->codec_id);
        std::string codecName = desc ? desc->name : "unknown";
        LOG("remuxWithLibav: stream " + std::to_string(i) +
            " codec_id=" + std::to_string(inPar->codec_id) +
            " codec_type=" + std::to_string(inPar->codec_type) +
            " id=" + std::format("0x{:x}", inStream->id) +
            " name=" + codecName);

        // Ask the Matroska muxer if it supports this codec
        int supported = avformat_query_codec(outCtx->oformat, inPar->codec_id, FF_COMPLIANCE_NORMAL);
        if (supported != 1) {
            LOG("remuxWithLibav: skipping stream " + std::to_string(i) +
                " (" + codecName + " not supported by MKV muxer)");
            continue;
        }

        const TrackInfo* trackInfo = findTrackInfo(title, inStream, usedTrackMetadata);
        const char* mediaType = trackTypeForMediaType(inPar->codec_type);
        bool hasDeclaredTracksOfType = mediaType[0] && std::any_of(
            title.tracks.begin(), title.tracks.end(),
            [mediaType](const TrackInfo& track) {
                return track.type == mediaType && track.stream_id >= 0;
            });
        if (!trackInfo && hasDeclaredTracksOfType) {
            LOG("remuxWithLibav: skipping stream " + std::to_string(i) +
                " because id=" + std::format("0x{:x}", inStream->id) +
                " is not declared for this title's " + mediaType + " tracks");
            continue;
        }
        if (trackInfo && !trackInfo->output.selected) {
            LOG("remuxWithLibav: skipping stream " + std::to_string(i) +
                " because it was unchecked by the user, id=" +
                std::format("0x{:x}", inStream->id));
            continue;
        }
        if (inPar->codec_id == AV_CODEC_ID_DVD_SUBTITLE &&
            inPar->extradata_size == 0 &&
            (!trackInfo || !trackInfo->has_palette)) {
            LOG("remuxWithLibav: skipping DVD subtitle stream " + std::to_string(i) +
                " because no matching IFO palette was found");
            continue;
        }

        AVStream* outStream = avformat_new_stream(outCtx, nullptr);
        if (!outStream) {
            errorMsg = "Falha ao criar stream de saída.";
            LOG("remuxWithLibav: avformat_new_stream failed");
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            return false;
        }
        int copyRet = avcodec_parameters_copy(outStream->codecpar, inPar);
        if (copyRet < 0) {
            char errbuf[256];
            av_strerror(copyRet, errbuf, sizeof(errbuf));
            errorMsg = "Falha ao copiar parâmetros de stream.";
            LOG("remuxWithLibav: avcodec_parameters_copy failed: " + std::string(errbuf));
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            return false;
        }
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
        outStream->id = inStream->id;
        av_dict_copy(&outStream->metadata, inStream->metadata, 0);
        outStream->disposition = inStream->disposition;

        if (trackInfo) {
            outStream->disposition &=
                ~(AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED);
            if (trackInfo->output.mkv_default) {
                outStream->disposition |= AV_DISPOSITION_DEFAULT;
            }
            if (trackInfo->type == "subtitle" && trackInfo->output.mkv_forced) {
                outStream->disposition |= AV_DISPOSITION_FORCED;
            }

            const std::string language = trackInfo->language.empty()
                ? "und" : trackInfo->language;
            av_dict_set(&outStream->metadata, "language", language.c_str(), 0);
            const char* trackTitle = nullptr;
            if (trackInfo->type == "audio") {
                if (trackInfo->code_extension == 2) trackTitle = "Audio description";
                if (trackInfo->code_extension == 3 || trackInfo->code_extension == 4) {
                    trackTitle = "Commentary";
                }
            } else if (trackInfo->type == "subtitle") {
                if (trackInfo->output.mkv_forced) {
                    trackTitle = "Forced";
                } else if (trackInfo->code_extension >= 13 &&
                           trackInfo->code_extension <= 15) {
                    trackTitle = "Commentary";
                }
            }
            if (trackTitle) {
                av_dict_set(&outStream->metadata, "title", trackTitle, 0);
            }

            if (inPar->codec_id == AV_CODEC_ID_DVD_SUBTITLE &&
                !setVobSubCodecPrivate(outStream->codecpar, *trackInfo)) {
                errorMsg = "Falha ao criar paleta VobSub para o MKV.";
                LOG("remuxWithLibav: failed to create VobSub CodecPrivate for stream " +
                    std::to_string(i));
                avformat_free_context(outCtx);
                avformat_close_input(&inCtx);
                return false;
            }
            LOG("remuxWithLibav: metadata stream " + std::to_string(i) +
                ", id=" + std::format("0x{:x}", inStream->id) +
                ", type=" + trackInfo->type +
                ", language=" +
                (trackInfo->language.empty() ? "und" : trackInfo->language) +
                ", default=" +
                (trackInfo->output.mkv_default ? "yes" : "no") +
                ", forced=" +
                (trackInfo->output.mkv_forced ? "yes" : "no"));
        }
        inputToOutput[i] = mappedStreamCount;
        if (referenceVideoInputStream < 0 &&
            inPar->codec_type == AVMEDIA_TYPE_VIDEO) {
            referenceVideoInputStream = static_cast<int>(i);
        }
        if (inPar->codec_type == AVMEDIA_TYPE_VIDEO ||
            inPar->codec_type == AVMEDIA_TYPE_AUDIO) {
            adaptiveAnchorInputStreams.push_back(static_cast<int>(i));
        }
        LOG("remuxWithLibav: mapped stream " + std::to_string(i) + " -> " +
            std::to_string(mappedStreamCount) + " (" + codecName + ")");
        mappedStreamCount++;
    }

    if (mappedStreamCount == 0) {
        errorMsg = "Nenhum stream compatível com MKV encontrado.";
        LOG("remuxWithLibav: no compatible streams");
        avformat_free_context(outCtx);
        avformat_close_input(&inCtx);
        return false;
    }
    if (!dvdClockTimeline.empty() && referenceVideoInputStream < 0) {
        errorMsg = "O título DVD não possui uma faixa de vídeo selecionada para ancorar o relógio.";
        LOG("remuxWithLibav: no mapped video stream for adaptive SCR clock");
        avformat_free_context(outCtx);
        avformat_close_input(&inCtx);
        return false;
    }

    if (!addChaptersToOutput(outCtx, title, errorMsg)) {
        LOG("remuxWithLibav: failed to add chapters: " + errorMsg);
        avformat_free_context(outCtx);
        avformat_close_input(&inCtx);
        return false;
    }

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            errorMsg = "Falha ao abrir arquivo de saída: " + outputPath;
            LOG("remuxWithLibav: avio_open failed");
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            return false;
        }
    }

    constexpr int64_t kMaxInterleaveDeltaUs = 1000000;
    constexpr int64_t kForcedInterleaveFlushPackets = 1024;
    outCtx->max_interleave_delta = kMaxInterleaveDeltaUs;

    int headerRet = avformat_write_header(outCtx, nullptr);
    if (headerRet < 0) {
        char errbuf[256];
        av_strerror(headerRet, errbuf, sizeof(errbuf));
        errorMsg = "Falha ao escrever cabeçalho MKV.";
        LOG("remuxWithLibav: avformat_write_header failed: " + std::string(errbuf));
        avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
        avformat_close_input(&inCtx);
        return false;
    }
    LOG("remuxWithLibav: header written OK");

    const bool allowEqualDts = (outCtx->oformat->flags & AVFMT_TS_NONSTRICT) != 0;
    LOG("remuxWithLibav: DTS policy=" +
        std::string(allowEqualDts ? "nondecreasing" : "strictly increasing"));
    LOG("remuxWithLibav: interleave max delta us=" +
        std::to_string(outCtx->max_interleave_delta) + ", forced flush interval packets=" +
        std::to_string(kForcedInterleaveFlushPackets));

    struct StreamTimestampState {
        int64_t last_dts = AV_NOPTS_VALUE;
        int64_t last_positive_dts_step = 0;
        size_t last_clock_segment = static_cast<size_t>(-1);
    };
    std::vector<StreamTimestampState> timestampStates(mappedStreamCount);
    std::vector<bool> clockSegmentUsed(dvdClockTimeline.size(), false);

    AVPacket pkt{};
    int64_t packet_count = 0;
    int64_t skipped_count = 0;
    int64_t dropped_timestamp_count = 0;
    int64_t synthesized_timestamp_count = 0;
    int64_t adaptive_segment_anchor_count = 0;
    int64_t adaptive_segment_correction_count = 0;
    int64_t adaptive_large_deviation_count = 0;
    int64_t timeline_adjusted_count = 0;
    int64_t inherited_position_count = 0;
    int64_t terminal_boundary_inherited_count = 0;
    int64_t global_fallback_position_count = 0;
    int64_t max_muxed_time_ms = 0;
    int64_t max_input_position = 0;
    int64_t interleaver_flush_count = 0;
    int64_t bounded_genpts_count = 0;
    int64_t bounded_genpts_fallback_count = 0;
    int64_t read_ahead_bytes = 0;
    int64_t peak_read_ahead_bytes = 0;
    size_t peak_read_ahead_packets = 0;
    int lastMuxPermille = 0;
    constexpr size_t kNoTimelineSegment = static_cast<size_t>(-1);
    std::vector<size_t> lastPositionedTimelineSegmentByInput(
        inputToOutput.size(), kNoTimelineSegment);
    std::vector<size_t> terminalBoundarySegmentByInput(
        inputToOutput.size(), kNoTimelineSegment);
    size_t furthestPositionedTimelineSegment = kNoTimelineSegment;
    std::vector<int64_t> adaptiveSegmentCorrection90k(
        dvdClockTimeline.size(), 0);
    std::vector<bool> adaptiveSegmentResolved(
        dvdClockTimeline.size(), false);
    if (!adaptiveSegmentResolved.empty()) {
        adaptiveSegmentResolved[0] = true;
    }
    auto next_progress_log = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool ok = true;

    constexpr int64_t kMaxGenPtsLookaheadBytes = 4 * 1024 * 1024;
    constexpr size_t kMaxGenPtsLookaheadPackets = 4096;
    std::deque<AVPacket*> readAheadPackets;
    constexpr int64_t kMaxAdaptiveBoundaryBufferBytes = 16 * 1024 * 1024;
    constexpr size_t kMaxAdaptiveBoundaryBufferPackets = 8192;
    std::deque<AVPacket*> adaptiveBoundaryPackets;
    std::deque<AVPacket*> replayPackets;
    int64_t adaptive_boundary_buffer_bytes = 0;
    size_t adaptiveBoundarySegment = kNoTimelineSegment;
    std::vector<bool> adaptiveBoundaryAnchorRequired(
        inputToOutput.size(), false);
    std::vector<bool> adaptiveBoundaryAnchorSeen(
        inputToOutput.size(), false);
    size_t adaptiveBoundaryRequiredAnchorCount = 0;
    size_t adaptiveBoundarySeenAnchorCount = 0;
    int64_t adaptiveBoundaryMaxResidual90k = 0;
    int adaptiveBoundaryContributorInputStream = -1;
    int adaptiveBoundaryContributorOutputStream = -1;
    int64_t adaptiveBoundaryContributorPreviousDts = AV_NOPTS_VALUE;
    int64_t adaptiveBoundaryContributorObservedDts = AV_NOPTS_VALUE;
    int64_t adaptiveBoundaryContributorResidualOut = 0;
    int64_t adaptiveBoundaryContributorCadence = 0;
    int64_t adaptiveBoundaryContributorPosition = -1;
    bool rawInputEnded = false;
    int rawInputEndResult = AVERROR_EOF;

    auto clearReadAhead = [&]() {
        while (!readAheadPackets.empty()) {
            AVPacket* queued = readAheadPackets.front();
            readAheadPackets.pop_front();
            av_packet_free(&queued);
        }
        read_ahead_bytes = 0;
    };

    auto clearAdaptivePacketQueues = [&]() {
        while (!adaptiveBoundaryPackets.empty()) {
            AVPacket* queued = adaptiveBoundaryPackets.front();
            adaptiveBoundaryPackets.pop_front();
            av_packet_free(&queued);
        }
        while (!replayPackets.empty()) {
            AVPacket* queued = replayPackets.front();
            replayPackets.pop_front();
            av_packet_free(&queued);
        }
        adaptive_boundary_buffer_bytes = 0;
        adaptiveBoundarySegment = kNoTimelineSegment;
        std::fill(adaptiveBoundaryAnchorRequired.begin(),
                  adaptiveBoundaryAnchorRequired.end(), false);
        std::fill(adaptiveBoundaryAnchorSeen.begin(),
                  adaptiveBoundaryAnchorSeen.end(), false);
        adaptiveBoundaryRequiredAnchorCount = 0;
        adaptiveBoundarySeenAnchorCount = 0;
        adaptiveBoundaryMaxResidual90k = 0;
        adaptiveBoundaryContributorInputStream = -1;
        adaptiveBoundaryContributorOutputStream = -1;
        adaptiveBoundaryContributorPreviousDts = AV_NOPTS_VALUE;
        adaptiveBoundaryContributorObservedDts = AV_NOPTS_VALUE;
        adaptiveBoundaryContributorResidualOut = 0;
        adaptiveBoundaryContributorCadence = 0;
        adaptiveBoundaryContributorPosition = -1;
    };

    auto clockSegmentIndexForPosition = [&](int64_t position, size_t& segmentIndex) {
        if (position < 0 || dvdClockTimeline.empty()) return false;
        auto segmentIt = std::upper_bound(
            dvdClockTimeline.begin(), dvdClockTimeline.end(), position,
            [](int64_t packetPosition, const DvdClockSegment& segment) {
                return packetPosition < segment.byte_offset;
            });
        if (segmentIt == dvdClockTimeline.begin()) return false;
        segmentIndex = static_cast<size_t>(--segmentIt - dvdClockTimeline.begin());
        return true;
    };

    auto adaptiveCorrectionForSegment = [&](size_t segmentIndex) {
        if (segmentIndex >= adaptiveSegmentResolved.size()) return int64_t{0};
        for (size_t i = segmentIndex + 1; i > 0; --i) {
            const size_t candidate = i - 1;
            if (adaptiveSegmentResolved[candidate]) {
                return adaptiveSegmentCorrection90k[candidate];
            }
        }
        return int64_t{0};
    };

    auto effectiveClockOffsetForSegment = [&](size_t segmentIndex) {
        return dvdClockTimeline[segmentIndex].timestamp_offset_90k +
            adaptiveCorrectionForSegment(segmentIndex);
    };

    auto clockOffsetForPosition = [&](int64_t position, int64_t& offset90k) {
        size_t segmentIndex = 0;
        if (!clockSegmentIndexForPosition(position, segmentIndex)) return false;
        offset90k = effectiveClockOffsetForSegment(segmentIndex);
        return true;
    };

    auto adjustedDts90k = [&](const AVPacket* packet, int64_t& adjusted,
                              int64_t& offset90k) {
        if (!packet || packet->dts == AV_NOPTS_VALUE || packet->stream_index < 0 ||
            packet->stream_index >= static_cast<int>(inCtx->nb_streams) ||
            !clockOffsetForPosition(packet->pos, offset90k)) {
            return false;
        }
        AVStream* stream = inCtx->streams[packet->stream_index];
        adjusted = av_rescale_q(packet->dts, stream->time_base,
                                AVRational{1, 90000}) + offset90k;
        return true;
    };

    auto readRawPacket = [&]() {
        if (rawInputEnded) return rawInputEndResult;
        AVPacket* queued = av_packet_alloc();
        if (!queued) {
            rawInputEnded = true;
            rawInputEndResult = AVERROR(ENOMEM);
            return rawInputEndResult;
        }
        int ret = av_read_frame(inCtx, queued);
        if (ret < 0) {
            av_packet_free(&queued);
            rawInputEnded = true;
            rawInputEndResult = ret;
            return ret;
        }
        read_ahead_bytes += std::max<int64_t>(queued->size, 0);
        readAheadPackets.push_back(queued);
        peak_read_ahead_bytes = std::max(peak_read_ahead_bytes, read_ahead_bytes);
        peak_read_ahead_packets = std::max(
            peak_read_ahead_packets, readAheadPackets.size());
        return 0;
    };

    auto readNextPacket = [&](AVPacket* output) {
        if (!useBoundedDvdGenPts) return av_read_frame(inCtx, output);

        if (readAheadPackets.empty()) {
            int ret = readRawPacket();
            if (ret < 0) return ret;
        }

        AVPacket* front = readAheadPackets.front();
        AVStream* frontStream = inCtx->streams[front->stream_index];
        const bool needsVideoPts =
            frontStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            front->pts == AV_NOPTS_VALUE && front->dts != AV_NOPTS_VALUE;

        if (needsVideoPts) {
            bool resolved = false;
            while (!resolved) {
                int64_t frontAdjustedDts90k = 0;
                int64_t frontOffset90k = 0;
                const bool frontHasClock = adjustedDts90k(
                    front, frontAdjustedDts90k, frontOffset90k);

                if (frontHasClock) {
                    for (size_t i = 1; i < readAheadPackets.size(); ++i) {
                        AVPacket* future = readAheadPackets[i];
                        if (future->stream_index != front->stream_index ||
                            future->dts == AV_NOPTS_VALUE ||
                            future->pts == future->dts) {
                            continue;
                        }
                        int64_t futureAdjustedDts90k = 0;
                        int64_t futureOffset90k = 0;
                        if (!adjustedDts90k(future, futureAdjustedDts90k,
                                            futureOffset90k) ||
                            futureAdjustedDts90k <= frontAdjustedDts90k) {
                            continue;
                        }

                        const int64_t ptsInFrontClock90k =
                            futureAdjustedDts90k - frontOffset90k;
                        front->pts = av_rescale_q_rnd(
                            ptsInFrontClock90k, AVRational{1, 90000},
                            frontStream->time_base,
                            static_cast<AVRounding>(
                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                        bounded_genpts_count++;
                        resolved = true;
                        break;
                    }
                }

                if (resolved) break;

                const bool lookaheadLimitReached =
                    readAheadPackets.size() >= kMaxGenPtsLookaheadPackets ||
                    read_ahead_bytes >= kMaxGenPtsLookaheadBytes;
                if (rawInputEnded || lookaheadLimitReached) {
                    front->pts = front->dts;
                    bounded_genpts_fallback_count++;
                    if (bounded_genpts_fallback_count <= 10) {
                        LOG("remuxWithLibav: bounded PTS lookahead fallback, stream=" +
                            std::to_string(front->stream_index) +
                            ", dts=" + std::to_string(front->dts) +
                            ", pos=" + std::to_string(front->pos) +
                            ", buffered_packets=" +
                            std::to_string(readAheadPackets.size()) +
                            ", buffered_bytes=" +
                            std::to_string(read_ahead_bytes) +
                            ", reason=" +
                            (rawInputEnded ? "end-of-input" : "lookahead-limit"));
                    }
                    break;
                }

                readRawPacket();
            }
        }

        front = readAheadPackets.front();
        readAheadPackets.pop_front();
        read_ahead_bytes -= std::max<int64_t>(front->size, 0);
        av_packet_move_ref(output, front);
        av_packet_free(&front);
        return 0;
    };

    auto readPacketForMux = [&](AVPacket* output) {
        if (replayPackets.empty()) return readNextPacket(output);
        AVPacket* queued = replayPackets.front();
        replayPackets.pop_front();
        av_packet_move_ref(output, queued);
        av_packet_free(&queued);
        return 0;
    };

    auto stashAdaptiveBoundaryPacket = [&](AVPacket* packet) {
        AVPacket* queued = av_packet_alloc();
        if (!queued) {
            errorMsg = "Memória insuficiente ao alinhar um segmento SCR do DVD.";
            return false;
        }
        adaptive_boundary_buffer_bytes += std::max<int64_t>(packet->size, 0);
        av_packet_move_ref(queued, packet);
        adaptiveBoundaryPackets.push_back(queued);
        if (adaptiveBoundaryPackets.size() > kMaxAdaptiveBoundaryBufferPackets ||
            adaptive_boundary_buffer_bytes > kMaxAdaptiveBoundaryBufferBytes) {
            errorMsg = "As âncoras contínuas do novo segmento SCR não apareceram "
                "dentro do limite seguro de leitura.";
            LOG("remuxWithLibav: adaptive SCR boundary buffer limit exceeded, segment=" +
                std::to_string(adaptiveBoundarySegment) + ", packets=" +
                std::to_string(adaptiveBoundaryPackets.size()) + ", bytes=" +
                std::to_string(adaptive_boundary_buffer_bytes));
            return false;
        }
        return true;
    };

    auto beginAdaptiveBoundary = [&](size_t segmentIndex) {
        adaptiveBoundarySegment = segmentIndex;
        std::fill(adaptiveBoundaryAnchorRequired.begin(),
                  adaptiveBoundaryAnchorRequired.end(), false);
        std::fill(adaptiveBoundaryAnchorSeen.begin(),
                  adaptiveBoundaryAnchorSeen.end(), false);
        adaptiveBoundaryRequiredAnchorCount = 0;
        adaptiveBoundarySeenAnchorCount = 0;
        adaptiveBoundaryMaxResidual90k = 0;
        adaptiveBoundaryContributorInputStream = -1;
        adaptiveBoundaryContributorOutputStream = -1;
        adaptiveBoundaryContributorPreviousDts = AV_NOPTS_VALUE;
        adaptiveBoundaryContributorObservedDts = AV_NOPTS_VALUE;
        adaptiveBoundaryContributorResidualOut = 0;
        adaptiveBoundaryContributorCadence = 0;
        adaptiveBoundaryContributorPosition = -1;
        for (const int inputIndex : adaptiveAnchorInputStreams) {
            const int outputIndex = inputToOutput[inputIndex];
            if (outputIndex >= 0 &&
                timestampStates[outputIndex].last_dts != AV_NOPTS_VALUE) {
                adaptiveBoundaryAnchorRequired[inputIndex] = true;
                adaptiveBoundaryRequiredAnchorCount++;
            }
        }
        LOG("remuxWithLibav: buffering packets for adaptive SCR anchors, segment=" +
            std::to_string(segmentIndex) + ", byte=" +
            std::to_string(dvdClockTimeline[segmentIndex].byte_offset) +
            ", required_continuous_streams=" +
            std::to_string(adaptiveBoundaryRequiredAnchorCount));
    };

    auto finalizeAdaptiveSegment = [&](size_t segmentIndex) {
        const int64_t carriedCorrection90k =
            adaptiveCorrectionForSegment(segmentIndex);
        adaptiveSegmentCorrection90k[segmentIndex] =
            carriedCorrection90k + adaptiveBoundaryMaxResidual90k;
        adaptiveSegmentResolved[segmentIndex] = true;
        adaptive_segment_anchor_count++;
        if (adaptiveBoundaryMaxResidual90k != 0) {
            adaptive_segment_correction_count++;
        }
        const int64_t absoluteCorrection =
            std::max<int64_t>(adaptiveBoundaryContributorResidualOut, 0);
        if (adaptiveBoundaryContributorCadence > 0 &&
            absoluteCorrection > adaptiveBoundaryContributorCadence) {
            adaptive_large_deviation_count++;
        }
        LOG("remuxWithLibav: adaptive SCR segment anchored, segment=" +
            std::to_string(segmentIndex) + ", anchors=" +
            std::to_string(adaptiveBoundarySeenAnchorCount) + "/" +
            std::to_string(adaptiveBoundaryRequiredAnchorCount) +
            ", contributor_input_stream=" +
            std::to_string(adaptiveBoundaryContributorInputStream) +
            ", contributor_output_stream=" +
            std::to_string(adaptiveBoundaryContributorOutputStream) +
            ", previous_dts=" +
            std::to_string(adaptiveBoundaryContributorPreviousDts) +
            ", cadence=" +
            std::to_string(adaptiveBoundaryContributorCadence) +
            ", observed_dts=" +
            std::to_string(adaptiveBoundaryContributorObservedDts) +
            ", residual_out=" +
            std::to_string(adaptiveBoundaryContributorResidualOut) +
            ", residual_90k=" +
            std::to_string(adaptiveBoundaryMaxResidual90k) +
            ", carried_90k=" + std::to_string(carriedCorrection90k) +
            ", total_90k=" +
            std::to_string(adaptiveSegmentCorrection90k[segmentIndex]) +
            ", deviation_cadences=" +
            (adaptiveBoundaryContributorCadence > 0
                ? std::to_string(
                    static_cast<double>(absoluteCorrection) /
                    static_cast<double>(adaptiveBoundaryContributorCadence))
                : std::string("unknown")) +
            ", pos=" +
            std::to_string(adaptiveBoundaryContributorPosition));
    };

    auto observeAdaptiveBoundaryAnchor = [&](size_t segmentIndex,
                                             int inputIndex,
                                             const AVPacket* packet) {
        if (inputIndex < 0 ||
            static_cast<size_t>(inputIndex) >= adaptiveBoundaryAnchorRequired.size() ||
            !adaptiveBoundaryAnchorRequired[inputIndex] ||
            adaptiveBoundaryAnchorSeen[inputIndex]) {
            return true;
        }
        const int outputIndex = inputToOutput[inputIndex];
        AVStream* inputStream = inCtx->streams[inputIndex];
        AVStream* outputStream = outCtx->streams[outputIndex];
        StreamTimestampState& state = timestampStates[outputIndex];
        const int64_t rawDts = packet->dts != AV_NOPTS_VALUE
            ? packet->dts : packet->pts;
        if (rawDts == AV_NOPTS_VALUE) {
            errorMsg = "Não foi possível obter DTS para ancorar uma faixa contínua do segmento SCR.";
            LOG("remuxWithLibav: adaptive SCR anchor lacks DTS, segment=" +
                std::to_string(segmentIndex) + ", input_stream=" +
                std::to_string(inputIndex) + ", pos=" +
                std::to_string(packet->pos));
            return false;
        }

        int64_t expectedStep = state.last_positive_dts_step;
        if (expectedStep <= 0 && packet->duration > 0) {
            expectedStep = av_rescale_q(
                packet->duration, inputStream->time_base,
                outputStream->time_base);
        }
        if (expectedStep <= 0 && inputStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            const AVRational frameRate = av_guess_frame_rate(
                inCtx, inputStream, nullptr);
            if (frameRate.num > 0 && frameRate.den > 0) {
                expectedStep = av_rescale_q(
                    1, av_inv_q(frameRate), outputStream->time_base);
            }
        }
        const int64_t carriedCorrection90k =
            adaptiveCorrectionForSegment(segmentIndex);
        const int64_t effectiveOffset90k =
            dvdClockTimeline[segmentIndex].timestamp_offset_90k +
            carriedCorrection90k;
        const int64_t offsetInInputTimebase = av_rescale_q_rnd(
            effectiveOffset90k, AVRational{1, 90000}, inputStream->time_base,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        const int64_t observedDts = av_rescale_q_rnd(
            rawDts + offsetInInputTimebase, inputStream->time_base,
            outputStream->time_base,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        const int64_t correctionInOutputTimebase =
            adaptiveDtsCorrection(state.last_dts, observedDts, allowEqualDts);
        const int64_t residualCorrection90k = correctionInOutputTimebase > 0
            ? av_rescale_q_rnd(
                correctionInOutputTimebase, outputStream->time_base,
                AVRational{1, 90000}, AV_ROUND_UP)
            : 0;

        adaptiveBoundaryAnchorSeen[inputIndex] = true;
        adaptiveBoundarySeenAnchorCount++;
        LOG("remuxWithLibav: adaptive SCR anchor observed, segment=" +
            std::to_string(segmentIndex) + ", input_stream=" +
            std::to_string(inputIndex) + ", output_stream=" +
            std::to_string(outputIndex) + ", previous_dts=" +
            std::to_string(state.last_dts) + ", observed_dts=" +
            std::to_string(observedDts) + ", residual_out=" +
            std::to_string(correctionInOutputTimebase) + ", residual_90k=" +
            std::to_string(residualCorrection90k) + ", pos=" +
            std::to_string(packet->pos));
        if (adaptiveBoundaryContributorInputStream < 0 ||
            residualCorrection90k > adaptiveBoundaryMaxResidual90k) {
            adaptiveBoundaryMaxResidual90k = residualCorrection90k;
            adaptiveBoundaryContributorInputStream = inputIndex;
            adaptiveBoundaryContributorOutputStream = outputIndex;
            adaptiveBoundaryContributorPreviousDts = state.last_dts;
            adaptiveBoundaryContributorObservedDts = observedDts;
            adaptiveBoundaryContributorResidualOut = correctionInOutputTimebase;
            adaptiveBoundaryContributorCadence = expectedStep;
            adaptiveBoundaryContributorPosition = packet->pos;
        }
        if (adaptiveBoundarySeenAnchorCount ==
            adaptiveBoundaryRequiredAnchorCount) {
            finalizeAdaptiveSegment(segmentIndex);
        }
        return true;
    };

    auto validateEarlierSegmentAdaptiveBoundaryStream = [&](size_t segmentIndex,
                                                             int inputIndex) {
        if (segmentIndex >= dvdClockTimeline.size() || inputIndex < 0 ||
            static_cast<size_t>(inputIndex) >= inputToOutput.size()) {
            return false;
        }

        size_t positionedPackets = 0;
        size_t bufferedPackets = 0;
        for (const AVPacket* queued : adaptiveBoundaryPackets) {
            if (!queued || queued->stream_index != inputIndex) continue;
            bufferedPackets++;
            if (queued->pos < 0) continue;

            size_t packetSegment = kNoTimelineSegment;
            if (!clockSegmentIndexForPosition(queued->pos, packetSegment) ||
                !isEarlierClockSegment(packetSegment, segmentIndex)) {
                LOG("remuxWithLibav: terminal positioned stream is not confined to an "
                    "earlier SCR segment, boundary_segment=" +
                    std::to_string(segmentIndex) + ", input_stream=" +
                    std::to_string(inputIndex) + ", packet_segment=" +
                    (packetSegment == kNoTimelineSegment
                        ? std::string("unresolved")
                        : std::to_string(packetSegment)) +
                    ", pos=" + std::to_string(queued->pos));
                return false;
            }
            positionedPackets++;
        }

        if (positionedPackets == 0) return false;
        LOG("remuxWithLibav: terminal earlier-segment stream validated, "
            "boundary_segment=" + std::to_string(segmentIndex) +
            ", input_stream=" + std::to_string(inputIndex) +
            ", positioned_packets=" + std::to_string(positionedPackets) +
            ", buffered_packets=" + std::to_string(bufferedPackets));
        return true;
    };

    auto validatePositionlessAdaptiveBoundaryStream = [&](size_t segmentIndex,
                                                           int inputIndex) {
        if (segmentIndex >= dvdClockTimeline.size() || inputIndex < 0 ||
            static_cast<size_t>(inputIndex) >= inputToOutput.size()) {
            return false;
        }
        const int outputIndex = inputToOutput[inputIndex];
        if (outputIndex < 0 || outputIndex >= static_cast<int>(timestampStates.size())) {
            return false;
        }

        AVStream* inputStream = inCtx->streams[inputIndex];
        AVStream* outputStream = outCtx->streams[outputIndex];
        const int64_t candidateCorrection90k =
            adaptiveCorrectionForSegment(segmentIndex) +
            adaptiveBoundaryMaxResidual90k;
        const int64_t effectiveOffset90k =
            dvdClockTimeline[segmentIndex].timestamp_offset_90k +
            candidateCorrection90k;
        const int64_t offsetInInputTimebase = av_rescale_q_rnd(
            effectiveOffset90k, AVRational{1, 90000}, inputStream->time_base,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        int64_t previousDts = timestampStates[outputIndex].last_dts;
        size_t validatedPackets = 0;

        for (const AVPacket* queued : adaptiveBoundaryPackets) {
            if (!queued || queued->stream_index != inputIndex) continue;
            if (queued->pos >= 0) return false;

            const int64_t rawDts = queued->dts != AV_NOPTS_VALUE
                ? queued->dts : queued->pts;
            if (rawDts == AV_NOPTS_VALUE) {
                LOG("remuxWithLibav: terminal positionless packet lacks DTS, segment=" +
                    std::to_string(segmentIndex) + ", input_stream=" +
                    std::to_string(inputIndex));
                return false;
            }
            const int64_t candidateDts = av_rescale_q_rnd(
                rawDts + offsetInInputTimebase, inputStream->time_base,
                outputStream->time_base,
                static_cast<AVRounding>(
                    AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if (previousDts != AV_NOPTS_VALUE &&
                adaptiveDtsCorrection(previousDts, candidateDts, allowEqualDts) != 0) {
                LOG("remuxWithLibav: terminal positionless stream would regress, segment=" +
                    std::to_string(segmentIndex) + ", input_stream=" +
                    std::to_string(inputIndex) + ", output_stream=" +
                    std::to_string(outputIndex) + ", previous_dts=" +
                    std::to_string(previousDts) + ", candidate_dts=" +
                    std::to_string(candidateDts) + ", effective_offset_90k=" +
                    std::to_string(effectiveOffset90k));
                return false;
            }
            previousDts = candidateDts;
            validatedPackets++;
        }

        if (validatedPackets == 0) return false;
        LOG("remuxWithLibav: terminal positionless stream validated, segment=" +
            std::to_string(segmentIndex) + ", input_stream=" +
            std::to_string(inputIndex) + ", output_stream=" +
            std::to_string(outputIndex) + ", packets=" +
            std::to_string(validatedPackets) + ", final_candidate_dts=" +
            std::to_string(previousDts) + ", effective_offset_90k=" +
            std::to_string(effectiveOffset90k));
        return true;
    };

    auto replayAdaptiveBoundaryPackets = [&]() {
        while (!adaptiveBoundaryPackets.empty()) {
            replayPackets.push_back(adaptiveBoundaryPackets.front());
            adaptiveBoundaryPackets.pop_front();
        }
        adaptive_boundary_buffer_bytes = 0;
        adaptiveBoundarySegment = kNoTimelineSegment;
        std::fill(adaptiveBoundaryAnchorRequired.begin(),
                  adaptiveBoundaryAnchorRequired.end(), false);
        std::fill(adaptiveBoundaryAnchorSeen.begin(),
                  adaptiveBoundaryAnchorSeen.end(), false);
        adaptiveBoundaryRequiredAnchorCount = 0;
        adaptiveBoundarySeenAnchorCount = 0;
    };

    auto reportMuxProgress = [&](bool inputComplete) {
        int phasePermille = 0;
        if (inputComplete) {
            phasePermille = 999;
        } else if (inputSizeBytes > 0) {
            phasePermille = static_cast<int>(std::clamp<int64_t>(
                max_input_position * 1000 / inputSizeBytes, 0, 999));
        } else if (title.duration_ms > 0) {
            phasePermille = static_cast<int>(std::clamp<int64_t>(
                max_muxed_time_ms * 1000 / title.duration_ms, 0, 999));
        }
        if (phasePermille <= lastMuxPermille) return;
        lastMuxPermille = phasePermille;
        int titlePermille = titleProgressStart +
            (titleProgressEnd - titleProgressStart) * phasePermille / 1000;
        reportTitleProgress(titlePermille, "Muxando MKV", phasePermille);
    };

    auto flushInterleaver = [&](const std::string& reason, bool logSuccess) {
        if (logSuccess) {
            LOG("remuxWithLibav: interleaver flush starting (" + reason +
                "), packets=" + std::to_string(packet_count));
        }
        const auto flushStartedAt = std::chrono::steady_clock::now();
        int flushRet = av_interleaved_write_frame(outCtx, nullptr);
        const auto flushDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - flushStartedAt).count();
        if (flushRet < 0) {
            char errbuf[256];
            av_strerror(flushRet, errbuf, sizeof(errbuf));
            errorMsg = std::string("Erro ao descarregar fila do muxer: ") + errbuf;
            LOG("remuxWithLibav: interleaver flush failed (" + reason + "): " + errbuf);
            return false;
        }
        interleaver_flush_count++;
        if (logSuccess || flushDurationMs >= 1000) {
            LOG("remuxWithLibav: interleaver flushed (" + reason + "), packets=" +
                std::to_string(packet_count) + ", duration_ms=" +
                std::to_string(flushDurationMs));
        }
        return true;
    };

    while (true) {
        const int64_t readPositionBefore = inCtx->pb ? avio_tell(inCtx->pb) : -1;
        const auto readStartedAt = std::chrono::steady_clock::now();
        int ret = readPacketForMux(&pkt);
        const auto readDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - readStartedAt).count();
        if (readDurationMs >= 1000) {
            LOG("remuxWithLibav: slow packet read, duration_ms=" +
                std::to_string(readDurationMs) + ", input_before=" +
                std::to_string(readPositionBefore) + ", input_after=" +
                std::to_string(inCtx->pb ? avio_tell(inCtx->pb) : -1) +
                ", ret=" + std::to_string(ret));
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                errorMsg = std::string("Erro ao ler entrada: ") + errbuf;
                LOG("remuxWithLibav: av_read_frame failed: " + std::string(errbuf));
                ok = false;
                break;
            }
            if (!adaptiveBoundaryPackets.empty()) {
                std::vector<int> inactiveInputStreams;
                std::vector<int> earlierSegmentInputStreams;
                std::vector<int> invalidPositionedInputStreams;
                std::vector<int> positionlessInputStreams;
                std::vector<int> invalidPositionlessInputStreams;
                std::vector<size_t> bufferedPacketsByInput(
                    adaptiveBoundaryAnchorRequired.size(), 0);
                std::vector<bool> positionedPacketsByInput(
                    adaptiveBoundaryAnchorRequired.size(), false);
                for (const AVPacket* queued : adaptiveBoundaryPackets) {
                    if (queued && queued->stream_index >= 0 &&
                        static_cast<size_t>(queued->stream_index) <
                            bufferedPacketsByInput.size()) {
                        bufferedPacketsByInput[queued->stream_index]++;
                        if (queued->pos >= 0) {
                            positionedPacketsByInput[queued->stream_index] = true;
                        }
                    }
                }
                for (size_t inputIndex = 0;
                     inputIndex < adaptiveBoundaryAnchorRequired.size();
                     ++inputIndex) {
                    if (!adaptiveBoundaryAnchorRequired[inputIndex] ||
                        adaptiveBoundaryAnchorSeen[inputIndex]) {
                        continue;
                    }
                    if (bufferedPacketsByInput[inputIndex] == 0) {
                        inactiveInputStreams.push_back(
                            static_cast<int>(inputIndex));
                    } else if (positionedPacketsByInput[inputIndex]) {
                        const int streamIndex = static_cast<int>(inputIndex);
                        if (validateEarlierSegmentAdaptiveBoundaryStream(
                                adaptiveBoundarySegment, streamIndex)) {
                            earlierSegmentInputStreams.push_back(streamIndex);
                        } else {
                            invalidPositionedInputStreams.push_back(streamIndex);
                        }
                    } else {
                        positionlessInputStreams.push_back(
                            static_cast<int>(inputIndex));
                    }
                }

                for (const int inputIndex : positionlessInputStreams) {
                    if (!validatePositionlessAdaptiveBoundaryStream(
                            adaptiveBoundarySegment, inputIndex)) {
                        invalidPositionlessInputStreams.push_back(inputIndex);
                    }
                }

                if (canFinalizeTerminalAdaptiveBoundaryAtEof(
                        adaptiveBoundarySeenAnchorCount,
                        invalidPositionedInputStreams.size(),
                        invalidPositionlessInputStreams.size())) {
                    for (const int inputIndex : inactiveInputStreams) {
                        adaptiveBoundaryAnchorRequired[inputIndex] = false;
                    }
                    for (const int inputIndex : earlierSegmentInputStreams) {
                        adaptiveBoundaryAnchorRequired[inputIndex] = false;
                    }
                    for (const int inputIndex : positionlessInputStreams) {
                        adaptiveBoundaryAnchorRequired[inputIndex] = false;
                        terminalBoundarySegmentByInput[inputIndex] =
                            adaptiveBoundarySegment;
                    }
                    adaptiveBoundaryRequiredAnchorCount =
                        adaptiveBoundarySeenAnchorCount;
                    LOG("remuxWithLibav: final adaptive SCR segment accepted with " +
                        std::to_string(positionlessInputStreams.size()) +
                        " validated positionless stream(s) and " +
                        std::to_string(earlierSegmentInputStreams.size()) +
                        " earlier-segment stream(s) and " +
                        std::to_string(inactiveInputStreams.size()) +
                        " inactive stream(s); finalizing from " +
                        std::to_string(adaptiveBoundarySeenAnchorCount) +
                        " observed anchor(s), segment=" +
                        std::to_string(adaptiveBoundarySegment) +
                        ", buffered_packets=" +
                        std::to_string(adaptiveBoundaryPackets.size()) +
                        ", buffered_bytes=" +
                        std::to_string(adaptive_boundary_buffer_bytes));
                    finalizeAdaptiveSegment(adaptiveBoundarySegment);
                    replayAdaptiveBoundaryPackets();
                    continue;
                }

                errorMsg = "Fim da entrada antes das âncoras contínuas do segmento SCR.";
                LOG("remuxWithLibav: input ended with unresolved adaptive SCR boundary, "
                    "segment=" + std::to_string(adaptiveBoundarySegment) +
                    ", observed_anchors=" +
                    std::to_string(adaptiveBoundarySeenAnchorCount) +
                    ", earlier_segment_streams=" +
                    std::to_string(earlierSegmentInputStreams.size()) +
                    ", invalid_positioned_streams=" +
                    std::to_string(invalidPositionedInputStreams.size()) +
                    ", positionless_streams=" +
                    std::to_string(positionlessInputStreams.size()) +
                    ", invalid_positionless_streams=" +
                    std::to_string(invalidPositionlessInputStreams.size()) +
                    ", inactive_streams=" +
                    std::to_string(inactiveInputStreams.size()) +
                    ", buffered_packets=" +
                    std::to_string(adaptiveBoundaryPackets.size()) +
                    ", buffered_bytes=" +
                    std::to_string(adaptive_boundary_buffer_bytes));
                ok = false;
                break;
            }
            reportMuxProgress(true);
            LOG("remuxWithLibav: end of input");
            break;
        }

        if (pkt.pos >= 0) {
            max_input_position = std::max(
                max_input_position, pkt.pos + std::max<int64_t>(pkt.size, 0));
            reportMuxProgress(false);
        }

        int input_idx = pkt.stream_index;
        if (input_idx < 0 || input_idx >= static_cast<int>(inputToOutput.size()) ||
            inputToOutput[input_idx] < 0) {
            skipped_count++;
            av_packet_unref(&pkt);
            continue;
        }

        int out_idx = inputToOutput[input_idx];
        AVStream* inStream = inCtx->streams[input_idx];
        AVStream* outStream = outCtx->streams[out_idx];
        int64_t inputPosition = pkt.pos;
        const DvdClockSegment* dvdClockSegment = nullptr;
        size_t dvdClockSegmentIndex = 0;
        const char* dvdClockSegmentSource = "none";
        StreamTimestampState& timestampState = timestampStates[out_idx];

        size_t positionedSegmentIndex = kNoTimelineSegment;
        const bool hasPositionedSegment = inputPosition >= 0 &&
            clockSegmentIndexForPosition(inputPosition, positionedSegmentIndex);
        if (adaptiveBoundarySegment != kNoTimelineSegment ||
            (hasPositionedSegment && positionedSegmentIndex > 0 &&
             !adaptiveSegmentResolved[positionedSegmentIndex])) {
            if (adaptiveBoundarySegment == kNoTimelineSegment) {
                beginAdaptiveBoundary(positionedSegmentIndex);
                if (adaptiveBoundaryRequiredAnchorCount == 0) {
                    finalizeAdaptiveSegment(positionedSegmentIndex);
                }
            }

            const bool isPositionedBoundaryPacket = hasPositionedSegment &&
                positionedSegmentIndex == adaptiveBoundarySegment;
            if (isPositionedBoundaryPacket &&
                !observeAdaptiveBoundaryAnchor(
                    adaptiveBoundarySegment, input_idx, &pkt)) {
                av_packet_unref(&pkt);
                ok = false;
                break;
            }
            if (!stashAdaptiveBoundaryPacket(&pkt)) {
                av_packet_unref(&pkt);
                ok = false;
                break;
            }
            if (adaptiveSegmentResolved[adaptiveBoundarySegment]) {
                replayAdaptiveBoundaryPackets();
            }
            continue;
        }

        if (!dvdClockTimeline.empty()) {
            if (inputPosition >= 0) {
                auto segmentIt = std::upper_bound(
                    dvdClockTimeline.begin(), dvdClockTimeline.end(), inputPosition,
                    [](int64_t position, const DvdClockSegment& segment) {
                        return position < segment.byte_offset;
                    });
                if (segmentIt == dvdClockTimeline.begin()) {
                    errorMsg = "Pacote DVD anterior ao primeiro segmento do relógio SCR.";
                    LOG("remuxWithLibav: no SCR segment for packet position=" +
                        std::to_string(inputPosition));
                    av_packet_unref(&pkt);
                    ok = false;
                    break;
                }

                --segmentIt;
                dvdClockSegmentIndex = static_cast<size_t>(
                    segmentIt - dvdClockTimeline.begin());
                dvdClockSegmentSource = "packet-position";

                if (furthestPositionedTimelineSegment != kNoTimelineSegment &&
                    dvdClockSegmentIndex > furthestPositionedTimelineSegment &&
                    effectiveClockOffsetForSegment(dvdClockSegmentIndex) !=
                        effectiveClockOffsetForSegment(
                            furthestPositionedTimelineSegment)) {
                    if (!flushInterleaver(
                            "effective SCR offset change at byte " +
                                std::to_string(
                                    dvdClockTimeline[dvdClockSegmentIndex].byte_offset),
                            true)) {
                        av_packet_unref(&pkt);
                        ok = false;
                        break;
                    }
                }

                lastPositionedTimelineSegmentByInput[input_idx] = dvdClockSegmentIndex;
                if (furthestPositionedTimelineSegment == kNoTimelineSegment ||
                    dvdClockSegmentIndex > furthestPositionedTimelineSegment) {
                    furthestPositionedTimelineSegment = dvdClockSegmentIndex;
                }
            } else if (terminalBoundarySegmentByInput[input_idx] != kNoTimelineSegment) {
                dvdClockSegmentIndex = terminalBoundarySegmentByInput[input_idx];
                dvdClockSegmentSource = "validated-terminal-boundary-inheritance";
                inherited_position_count++;
                terminal_boundary_inherited_count++;
            } else if (lastPositionedTimelineSegmentByInput[input_idx] != kNoTimelineSegment) {
                dvdClockSegmentIndex = lastPositionedTimelineSegmentByInput[input_idx];
                dvdClockSegmentSource = "per-stream-inheritance";
                inherited_position_count++;
            } else if (furthestPositionedTimelineSegment != kNoTimelineSegment) {
                // A parser may omit pkt.pos for the first packet it emits for a stream. Until
                // that stream provides its own positioned packet, retain the old global
                // fallback for compatibility, but make it explicit and measurable. Once a
                // stream has a position, all of its positionless packets inherit only from
                // that stream so another stream cannot move it across an SCR discontinuity.
                dvdClockSegmentIndex = furthestPositionedTimelineSegment;
                dvdClockSegmentSource = "global-fallback-before-stream-anchor";
                inherited_position_count++;
                global_fallback_position_count++;
                if (global_fallback_position_count <= 10) {
                    LOG("remuxWithLibav: positionless packet before stream SCR anchor, "
                        "using global fallback, input_stream=" + std::to_string(input_idx) +
                        ", SCR_byte_offset=" +
                        std::to_string(
                            dvdClockTimeline[dvdClockSegmentIndex].byte_offset) +
                        ", pts=" + std::to_string(pkt.pts) +
                        ", dts=" + std::to_string(pkt.dts));
                }
            } else {
                errorMsg = "O primeiro pacote de um stream DVD não informou posição; "
                    "não é seguro escolher um segmento SCR.";
                LOG("remuxWithLibav: first stream packet without byte position, input_stream=" +
                    std::to_string(input_idx) + ", pts=" + std::to_string(pkt.pts) +
                    ", dts=" + std::to_string(pkt.dts));
                av_packet_unref(&pkt);
                ok = false;
                break;
            }

            dvdClockSegment = &dvdClockTimeline[dvdClockSegmentIndex];
            clockSegmentUsed[dvdClockSegmentIndex] = true;

            int64_t offsetInInputTimebase = av_rescale_q_rnd(
                effectiveClockOffsetForSegment(dvdClockSegmentIndex),
                AVRational{1, 90000},
                inStream->time_base,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if (pkt.pts != AV_NOPTS_VALUE) pkt.pts += offsetInInputTimebase;
            if (pkt.dts != AV_NOPTS_VALUE) pkt.dts += offsetInInputTimebase;
            timeline_adjusted_count++;
        }

        if (pkt.pts == AV_NOPTS_VALUE && pkt.dts == AV_NOPTS_VALUE) {
            dropped_timestamp_count++;
            if (dropped_timestamp_count <= 10) {
                LOG("remuxWithLibav: dropping packet without PTS and DTS, input_stream=" +
                    std::to_string(input_idx) + ", pos=" + std::to_string(inputPosition));
            }
            av_packet_unref(&pkt);
            continue;
        }
        if (pkt.pts == AV_NOPTS_VALUE) {
            pkt.pts = pkt.dts;
            synthesized_timestamp_count++;
        }
        if (pkt.dts == AV_NOPTS_VALUE) {
            pkt.dts = pkt.pts;
            synthesized_timestamp_count++;
        }

        pkt.stream_index = out_idx;
        av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);

        if (timestampState.last_dts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE &&
            (pkt.dts < timestampState.last_dts ||
             (!allowEqualDts && pkt.dts == timestampState.last_dts))) {
            errorMsg = "DTS não monotônico mesmo após aplicar o relógio SCR do DVD.";
            std::string segmentInfo = dvdClockSegment
                ? ", VOBU=" + std::to_string(dvdClockSegment->vobu_number) +
                  ", cell=" + std::to_string(dvdClockSegment->cell_number) +
                  ", SCR_offset_90k=" +
                      std::to_string(dvdClockSegment->timestamp_offset_90k) +
                  ", adaptive_correction_90k=" +
                      std::to_string(
                          adaptiveCorrectionForSegment(dvdClockSegmentIndex)) +
                  ", segment_source=" + dvdClockSegmentSource
                : "";
            LOG("remuxWithLibav: non-monotonic DTS after SCR timeline, input_stream=" +
                std::to_string(input_idx) + ", output_stream=" + std::to_string(out_idx) +
                ", previous_dts=" + std::to_string(timestampState.last_dts) +
                ", current_dts=" + std::to_string(pkt.dts) +
                ", pos=" + std::to_string(inputPosition) + segmentInfo);
            av_packet_unref(&pkt);
            ok = false;
            break;
        }
        if (timestampState.last_dts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE &&
            pkt.dts > timestampState.last_dts) {
            timestampState.last_positive_dts_step =
                pkt.dts - timestampState.last_dts;
        }
        timestampState.last_dts = pkt.dts;
        if (dvdClockSegment) {
            timestampState.last_clock_segment = dvdClockSegmentIndex;
        }

        int64_t pts = pkt.pts;
        int64_t dts = pkt.dts;
        int64_t duration = pkt.duration;

        if (packet_count > 0 &&
            packet_count % kForcedInterleaveFlushPackets == 0) {
            if (!flushInterleaver("periodic bound", false)) {
                av_packet_unref(&pkt);
                ok = false;
                break;
            }
        }

        if (packet_count == 0) {
            LOG("remuxWithLibav: writing first mapped packet, input_stream=" +
                std::to_string(input_idx) + ", output_stream=" + std::to_string(out_idx) +
                ", pts=" + std::to_string(pts) + ", dts=" + std::to_string(dts) +
                ", pos=" + std::to_string(inputPosition));
        }

        ret = av_interleaved_write_frame(outCtx, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            errorMsg = std::string("Erro durante escrita de frames: ") + errbuf;
            LOG("remuxWithLibav: write failed at packet " + std::to_string(packet_count + 1) +
                ": " + std::string(errbuf) +
                ", input_stream=" + std::to_string(input_idx) +
                ", output_stream=" + std::to_string(out_idx) +
                ", pts=" + std::to_string(pts) +
                ", dts=" + std::to_string(dts) +
                ", duration=" + std::to_string(duration) +
                ", time_base=" + std::to_string(outStream->time_base.num) +
                "/" + std::to_string(outStream->time_base.den) +
                ", pos=" + std::to_string(inputPosition) +
                ", skipped=" + std::to_string(skipped_count) +
                (dvdClockSegment
                    ? ", VOBU=" + std::to_string(dvdClockSegment->vobu_number) +
                      ", cell=" + std::to_string(dvdClockSegment->cell_number)
                    : ""));
            ok = false;
            break;
        }
        packet_count++;

        if (packet_count == 1) {
            LOG("remuxWithLibav: first mapped packet written");
        }

        if (dts != AV_NOPTS_VALUE) {
            int64_t packet_time_ms = av_rescale_q(
                dts, outStream->time_base, AVRational{1, 1000});
            max_muxed_time_ms = std::max(max_muxed_time_ms, packet_time_ms);
            if (inputSizeBytes <= 0) reportMuxProgress(false);
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= next_progress_log) {
            std::string progress = "remuxWithLibav: progress, packets=" +
                std::to_string(packet_count) + ", muxed_time_ms=" +
                std::to_string(max_muxed_time_ms) + ", input_bytes=" +
                std::to_string(max_input_position) + "/" +
                std::to_string(inputSizeBytes) + ", phase_permille=" +
                std::to_string(lastMuxPermille);
            LOG(progress);
            next_progress_log = now + std::chrono::seconds(5);
        }
    }

    clearReadAhead();
    clearAdaptivePacketQueues();

    int64_t usedClockSegments = static_cast<int64_t>(
        std::count(clockSegmentUsed.begin(), clockSegmentUsed.end(), true));
    LOG("remuxWithLibav: copied " + std::to_string(packet_count) + " packets, skipped " +
        std::to_string(skipped_count) + " unmapped packets, dropped " +
        std::to_string(dropped_timestamp_count) + " packets without timestamps, synthesized " +
        std::to_string(synthesized_timestamp_count) + " timestamps, adaptive SCR anchors " +
        std::to_string(adaptive_segment_anchor_count) + ", nonzero segment corrections " +
        std::to_string(adaptive_segment_correction_count) +
        ", corrections above one observed cadence " +
        std::to_string(adaptive_large_deviation_count) + ", SCR-adjusted " +
        std::to_string(timeline_adjusted_count) + " packets across " +
        std::to_string(usedClockSegments) + "/" +
        std::to_string(dvdClockTimeline.size()) +
        " clock segments, inherited segment for " +
        std::to_string(inherited_position_count) + " packets without byte position (" +
        std::to_string(terminal_boundary_inherited_count) +
        " validated terminal-boundary inheritances, " +
        std::to_string(global_fallback_position_count) +
        " global fallbacks before per-stream anchor), bounded PTS generated " +
        std::to_string(bounded_genpts_count) + ", bounded PTS fallbacks " +
        std::to_string(bounded_genpts_fallback_count) +
        ", peak PTS lookahead " + std::to_string(peak_read_ahead_packets) +
        " packets/" + std::to_string(peak_read_ahead_bytes) + " bytes, forced " +
        std::to_string(interleaver_flush_count) + " interleaver flushes");

    int trailer_ret = av_write_trailer(outCtx);
    if (trailer_ret < 0) {
        char errbuf[256];
        av_strerror(trailer_ret, errbuf, sizeof(errbuf));
        LOG("remuxWithLibav: av_write_trailer failed: " + std::string(errbuf));
        if (ok) errorMsg = std::string("Erro ao finalizar MKV: ") + errbuf;
        ok = false;
    } else {
        LOG("remuxWithLibav: trailer written");
    }

    int close_ret = avio_closep(&outCtx->pb);
    if (close_ret < 0) {
        char errbuf[256];
        av_strerror(close_ret, errbuf, sizeof(errbuf));
        LOG("remuxWithLibav: avio_closep failed: " + std::string(errbuf));
        if (ok) errorMsg = std::string("Erro ao fechar MKV: ") + errbuf;
        ok = false;
    }
    avformat_free_context(outCtx);
    avformat_close_input(&inCtx);

    if (ok) {
        reportTitleProgress(titleProgressEnd, "Muxando MKV", 1000);
    }

    if (!ok) {
        std::error_code ec;
        if (fs::remove(outputPath, ec)) {
            LOG("remuxWithLibav: partial output removed");
        } else if (ec) {
            LOG("remuxWithLibav: failed to remove partial output: " + ec.message());
        }
    }

    if (ok && (dropped_timestamp_count > 0 || synthesized_timestamp_count > 0)) {
        if (!errorMsg.empty()) errorMsg += "\n";
        errorMsg += "Aviso: " + std::to_string(dropped_timestamp_count) +
            " pacote(s) sem timestamp foram ignorados e " +
            std::to_string(synthesized_timestamp_count) +
            " timestamp(s) ausente(s) foram derivados. Verifique a reprodução do MKV.";
    }
    return ok;
}

bool MkvRemuxer::remuxTitle(const DiscInfo& disc, const TitleInfo& title,
                            const std::string& outputPath, std::string& errorMsg) {
    LOG("remuxTitle: disc_type=" + discTypeLabel(disc.disc_kind, disc.source_kind) +
        ", source_kind=" + sourceKindName(disc.source_kind) +
        ", title=" + std::to_string(title.index) +
        ", output=" + outputPath);

    if (disc.disc_kind == DiscKind::DvdVideo) {
        return remuxDvdTitle(disc, title, outputPath, errorMsg);
    }

    if (disc.disc_kind == DiscKind::BluRay &&
        disc.source_kind == SourceKind::IsoImage) {
        LOG("remuxTitle: using remuxWithLibav for Blu-ray ISO");
        return remuxWithLibav(disc.source_path, outputPath, title, errorMsg);
    }

    if (disc.disc_kind == DiscKind::BluRay) {
        fs::path streamDir = fs::path(disc.source_path) / "STREAM";
        if (!fs::exists(streamDir)) {
            streamDir = fs::path(disc.source_path) / "BDMV" / "STREAM";
        }
        std::string m2tsFile = std::format("{}/{:05d}.m2ts", streamDir.string(), title.index);
        if (!fs::exists(m2tsFile)) {
            errorMsg = "Arquivo M2TS não encontrado: " + m2tsFile;
            LOG("remuxTitle: M2TS not found: " + m2tsFile);
            return false;
        }
        return remuxWithLibav(m2tsFile, outputPath, title, errorMsg);
    }
    else {
        return remuxWithLibav(disc.source_path, outputPath, title, errorMsg);
    }
}

int MkvRemuxer::remuxSelected(const DiscInfo& disc, const std::string& outputFolder,
                               std::string& errorMsg) {
    LOG("remuxSelected: folder=" + outputFolder);

    m_completedWork = 0;
    m_totalWork = 0;
    m_selectedTitleCount = 0;
    for (const auto& title : disc.titles) {
        if (!title.output.selected) continue;
        m_totalWork += std::max<int64_t>(title.duration_ms, 1);
        m_selectedTitleCount++;
    }
    m_totalWork = std::max<int64_t>(m_totalWork, 1);
    m_currentTitleOrdinal = 0;

    if (!fs::exists(outputFolder)) {
        fs::create_directories(outputFolder);
    }

    int successCount = 0;
    for (const auto& title : disc.titles) {
        if (!title.output.selected) continue;

        m_currentTitleOrdinal++;
        m_currentTitleWork = std::max<int64_t>(title.duration_ms, 1);
        reportTitleProgress(0, "Preparando", 0);

        std::string filename = buildOutputFileName(disc, title);
        std::string outputPath = (fs::path(outputFolder) / filename).string();

        LOG("remuxSelected: processing title " + std::to_string(title.index) +
            " -> " + outputPath);

        if (remuxTitle(disc, title, outputPath, errorMsg)) {
            successCount++;
            reportTitleProgress(1000, "Concluído", 1000);
            m_completedWork += m_currentTitleWork;
            LOG("remuxSelected: title " + std::to_string(title.index) + " OK");
        } else {
            LOG("remuxSelected: title " + std::to_string(title.index) + " FAILED: " + errorMsg);
            return successCount;
        }
    }

    LOG("remuxSelected: completed, " + std::to_string(successCount) + " titles saved");
    return successCount;
}

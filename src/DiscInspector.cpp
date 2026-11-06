#include "DiscInspector.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

static bool dirExists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

static bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

extern "C" {
#include <libbluray/bluray.h>
#include <libbluray/meta_data.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>
}

namespace fs = std::filesystem;

static pgc_t* findTitlePgc(ifo_handle_t* vts, int vtsTitleNumber) {
    if (!vts || !vts->vts_ptt_srpt || !vts->vts_pgcit || vtsTitleNumber < 1 ||
        vtsTitleNumber > vts->vts_ptt_srpt->nr_of_srpts) {
        return nullptr;
    }

    const ttu_t& titleUnit = vts->vts_ptt_srpt->title[vtsTitleNumber - 1];
    if (titleUnit.nr_of_ptts < 1 || !titleUnit.ptt) {
        return nullptr;
    }

    int pgcNumber = titleUnit.ptt[0].pgcn;
    if (pgcNumber < 1 || pgcNumber > vts->vts_pgcit->nr_of_pgci_srp) {
        return nullptr;
    }
    return vts->vts_pgcit->pgci_srp[pgcNumber - 1].pgc;
}

static int64_t dvdTimeToMilliseconds(const dvd_time_t& time) {
    int64_t ticks = 0;
    ticks += static_cast<int64_t>(time.hour >> 4) * 10 * 60 * 60 * 90000;
    ticks += static_cast<int64_t>(time.hour & 0x0f) * 60 * 60 * 90000;
    ticks += static_cast<int64_t>(time.minute >> 4) * 10 * 60 * 90000;
    ticks += static_cast<int64_t>(time.minute & 0x0f) * 60 * 90000;
    ticks += static_cast<int64_t>(time.second >> 4) * 10 * 90000;
    ticks += static_cast<int64_t>(time.second & 0x0f) * 90000;

    int64_t frames = static_cast<int64_t>((time.frame_u & 0x30) >> 4) * 10 +
        (time.frame_u & 0x0f);
    ticks += frames * ((time.frame_u & 0x80) ? 3000 : 3600);
    return ticks / 90;
}

static std::string dvdLanguageToIso6392(uint16_t langCode) {
    if (langCode == 0) return "und";

    std::string code;
    code.push_back(static_cast<char>((langCode >> 8) & 0xff));
    code.push_back(static_cast<char>(langCode & 0xff));
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!std::isalpha(static_cast<unsigned char>(code[0])) ||
        !std::isalpha(static_cast<unsigned char>(code[1]))) {
        return "und";
    }

    // Matroska's legacy Language element uses ISO 639-2. DVD stores ISO 639-1.
    static const std::unordered_map<std::string, std::string> iso6391To6392 = {
        {"ar", "ara"}, {"bg", "bul"}, {"ca", "cat"}, {"cs", "ces"},
        {"cy", "cym"}, {"da", "dan"}, {"de", "deu"}, {"el", "ell"},
        {"en", "eng"}, {"es", "spa"}, {"et", "est"}, {"eu", "eus"},
        {"fa", "fas"}, {"fi", "fin"}, {"fr", "fra"}, {"ga", "gle"},
        {"gl", "glg"}, {"he", "heb"}, {"hi", "hin"}, {"hr", "hrv"},
        {"hu", "hun"}, {"hy", "hye"}, {"id", "ind"}, {"is", "isl"},
        {"it", "ita"}, {"ja", "jpn"}, {"ka", "kat"}, {"ko", "kor"},
        {"lt", "lit"}, {"lv", "lav"}, {"mk", "mkd"}, {"ms", "msa"},
        {"mt", "mlt"}, {"nl", "nld"}, {"no", "nor"}, {"pl", "pol"},
        {"pt", "por"}, {"ro", "ron"}, {"ru", "rus"}, {"sk", "slk"},
        {"sl", "slv"}, {"sq", "sqi"}, {"sr", "srp"}, {"sv", "swe"},
        {"th", "tha"}, {"tr", "tur"}, {"uk", "ukr"}, {"ur", "urd"},
        {"vi", "vie"}, {"zh", "zho"}
    };
    auto it = iso6391To6392.find(code);
    return it != iso6391To6392.end() ? it->second : code;
}

static int nextPlayableCell(const pgc_t* pgc, int cell) {
    if (!pgc || cell < 0 || cell >= pgc->nr_of_cells) return cell + 1;
    if (pgc->cell_playback[cell].block_type != BLOCK_TYPE_ANGLE_BLOCK) {
        return cell + 1;
    }

    int next = cell;
    while (next < pgc->nr_of_cells &&
           pgc->cell_playback[next].block_mode != BLOCK_MODE_LAST_CELL) {
        ++next;
    }
    return next + 1;
}

static void addDvdChapters(ifo_handle_t* vts, int vtsTitleNumber, TitleInfo& title) {
    if (!vts || !vts->vts_ptt_srpt || !vts->vts_pgcit || vtsTitleNumber < 1 ||
        vtsTitleNumber > vts->vts_ptt_srpt->nr_of_srpts) {
        return;
    }

    const ttu_t& unit = vts->vts_ptt_srpt->title[vtsTitleNumber - 1];
    int64_t chapterStartMs = 0;
    for (int i = 0; i < unit.nr_of_ptts; ++i) {
        const ptt_info_t& ptt = unit.ptt[i];
        if (ptt.pgcn < 1 || ptt.pgcn > vts->vts_pgcit->nr_of_pgci_srp) continue;

        pgc_t* pgc = vts->vts_pgcit->pgci_srp[ptt.pgcn - 1].pgc;
        if (!pgc || !pgc->program_map || !pgc->cell_playback ||
            ptt.pgn < 1 || ptt.pgn > pgc->nr_of_programs) {
            continue;
        }

        int cellStart = pgc->program_map[ptt.pgn - 1] - 1;
        int cellEnd = pgc->nr_of_cells - 1;
        if (i + 1 < unit.nr_of_ptts) {
            const ptt_info_t& nextPtt = unit.ptt[i + 1];
            if (nextPtt.pgcn == ptt.pgcn && nextPtt.pgn > ptt.pgn &&
                nextPtt.pgn <= pgc->nr_of_programs) {
                cellEnd = pgc->program_map[nextPtt.pgn - 1] - 2;
            }
        }

        int64_t durationMs = 0;
        for (int cell = cellStart; cell <= cellEnd; cell = nextPlayableCell(pgc, cell)) {
            durationMs += dvdTimeToMilliseconds(pgc->cell_playback[cell].playback_time);
        }
        if (durationMs <= 0) continue;

        ChapterInfo chapter;
        chapter.index = static_cast<int>(title.chapters.size()) + 1;
        chapter.name = "Capítulo " + std::to_string(chapter.index);
        chapter.start_ms = chapterStartMs;
        chapter.end_ms = chapterStartMs + durationMs;
        title.chapters.push_back(chapter);
        chapterStartMs = chapter.end_ms;
    }
    if (!title.chapters.empty()) {
        title.chapter_count = static_cast<int>(title.chapters.size());
    }
}

static std::pair<int, int> dvdVideoSize(const video_attr_t& attr) {
    int height = attr.video_format == 0 ? 480 : 576;
    int width = 720;
    switch (attr.picture_size) {
    case 1: width = 704; break;
    case 2: width = 352; break;
    case 3:
        width = 352;
        height /= 2;
        break;
    default: break;
    }
    return {width, height};
}

static void populateDvdTitle(ifo_handle_t* vts, int vtsTitleNumber, TitleInfo& title) {
    pgc_t* pgc = findTitlePgc(vts, vtsTitleNumber);
    if (!pgc || !vts->vtsi_mat) return;

    title.duration_ms = dvdTimeToMilliseconds(pgc->playback_time);
    addDvdChapters(vts, vtsTitleNumber, title);

    const auto [videoWidth, videoHeight] = dvdVideoSize(vts->vtsi_mat->vts_video_attr);
    TrackInfo video;
    video.index = 0;
    video.type = "video";
    video.codec = "MPEG-2";
    video.width = videoWidth;
    video.height = videoHeight;
    video.stream_id = 0x1e0;
    video.selected = true;
    title.tracks.push_back(video);

    int audioCount = std::min<int>(8, vts->vtsi_mat->nr_of_vts_audio_streams);
    for (int i = 0; i < audioCount; ++i) {
        uint16_t control = pgc->audio_control[i];
        if (!(control & 0x8000)) continue;

        const audio_attr_t& attr = vts->vtsi_mat->vts_audio_attr[i];
        int position = (control & 0x7f00) >> 8;
        TrackInfo track;
        track.index = i;
        track.type = "audio";
        track.language = dvdLanguageToIso6392(attr.lang_code);
        track.channels = attr.channels + 1;
        track.code_extension = attr.code_extension;
        track.selected = true;

        switch (attr.audio_format) {
        case 0:
            track.codec = "AC3";
            track.stream_id = 0x80 + position;
            break;
        case 2:
        case 3:
            track.codec = "MPEG Audio";
            track.stream_id = 0x1c0 + position;
            break;
        case 4:
            track.codec = "LPCM";
            track.stream_id = 0xa0 + position;
            break;
        case 6:
            track.codec = "DTS";
            track.stream_id = 0x88 + position;
            break;
        default:
            track.codec = "DVD Audio";
            break;
        }
        title.tracks.push_back(track);
    }

    std::set<int> subtitleStreamIds;
    int subtitleCount = std::min<int>(32, vts->vtsi_mat->nr_of_vts_subp_streams);
    bool widescreen = vts->vtsi_mat->vts_video_attr.display_aspect_ratio == 3;
    for (int i = 0; i < subtitleCount; ++i) {
        uint32_t control = pgc->subp_control[i];
        if (!(control & 0x80000000U)) continue;

        int position = widescreen ? ((control >> 16) & 0x1f) : ((control >> 24) & 0x1f);
        int streamId = 0x20 + position;
        if (!subtitleStreamIds.insert(streamId).second) continue;

        const subp_attr_t& attr = vts->vtsi_mat->vts_subp_attr[i];
        TrackInfo track;
        track.index = i;
        track.type = "subtitle";
        track.codec = "DVD-VOBSub";
        track.language = dvdLanguageToIso6392(attr.lang_code);
        track.width = videoWidth;
        track.height = videoHeight;
        track.stream_id = streamId;
        track.code_extension = attr.code_extension;
        for (size_t paletteIndex = 0; paletteIndex < track.palette.size(); ++paletteIndex) {
            track.palette[paletteIndex] = pgc->palette[paletteIndex];
        }
        track.has_palette = true;
        track.selected = true;
        title.tracks.push_back(track);
    }
}

static std::string blurayLanguage(const uint8_t lang[4]) {
    std::string code;
    for (int i = 0; i < 3 && lang[i] != 0; ++i) {
        code.push_back(static_cast<char>(std::tolower(lang[i])));
    }
    return code.size() == 3 ? code : "und";
}

static void addBlurayChapters(const BLURAY_TITLE_INFO* info, TitleInfo& title) {
    if (!info || !info->chapters) return;
    for (uint32_t i = 0; i < info->chapter_count; ++i) {
        ChapterInfo chapter;
        chapter.index = static_cast<int>(i + 1);
        chapter.name = "Capítulo " + std::to_string(chapter.index);
        chapter.start_ms = static_cast<int64_t>(info->chapters[i].start / 90);
        chapter.end_ms = static_cast<int64_t>(
            (info->chapters[i].start + info->chapters[i].duration) / 90);
        if (chapter.end_ms > chapter.start_ms) {
            title.chapters.push_back(chapter);
        }
    }
    if (!title.chapters.empty()) {
        title.chapter_count = static_cast<int>(title.chapters.size());
    }
}

static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}

// Using fs::is_directory / fs::is_regular_file defined above

std::string DiscInspector::detectVolumeLabel(const std::string& path) {
    // Try to read label from filesystem; fallback to directory name
    fs::path p(path);
    return p.filename().string();
}

DiscInfo DiscInspector::inspectDirectory(const std::string& path) {
    DiscInfo info;
    info.source_path = path;
    info.valid = false;

    if (!dirExists(path)) {
        info.error = "Pasta não existe.";
        return info;
    }

    // Look for VIDEO_TS (case-insensitive)
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_directory() && toLower(entry.path().filename().string()) == "video_ts") {
            return inspectDvd(entry.path().string());
        }
    }

    // Look for BDMV
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_directory() && toLower(entry.path().filename().string()) == "bdmv") {
            return inspectBluRay(entry.path().string());
        }
    }

    info.error = "Estrutura DVD-Video ou Blu-ray não encontrada.";
    return info;
}

DiscInfo DiscInspector::inspectImage(const std::string& isoPath) {
    DiscInfo info;
    info.source_path = isoPath;
    info.valid = false;

    if (!fileExists(isoPath)) {
        info.error = "Arquivo de imagem não existe: " + isoPath;
        return info;
    }

    info.volume_label = fs::path(isoPath).stem().string();

    // Try opening as DVD first (libdvdread supports ISO directly)
    dvd_reader_t* dvd = DVDOpen(isoPath.c_str());
    if (dvd) {
        ifo_handle_t* vmg = ifoOpen(dvd, 0);
        if (vmg) {
            DVDClose(dvd);
            ifoClose(vmg);
            // It's a DVD ISO — inspect it
            return inspectDvdIso(isoPath);
        }
        DVDClose(dvd);
    }

    // Try opening as Blu-ray (libbluray supports ISO directly)
    BLURAY* bd = bd_open(isoPath.c_str(), nullptr);
    if (bd) {
        uint32_t num_pl = bd_get_titles(bd, TITLES_ALL, 0);
        if (num_pl > 0) {
            bd_close(bd);
            return inspectBluRayIso(isoPath);
        }
        bd_close(bd);
    }

    info.disc_type = "ISO Image";
    info.error = "A imagem ISO não contém estrutura DVD-Video ou Blu-ray reconhecida.";
    return info;
}

DiscInfo DiscInspector::inspectDvdIso(const std::string& isoPath) {
    DiscInfo info;
    info.source_path = isoPath;
    info.disc_type = "DVD-Video (ISO)";
    info.valid = false;

    dvd_reader_t* dvd = DVDOpen(isoPath.c_str());
    if (!dvd) {
        info.error = "Não foi possível abrir a imagem ISO como DVD.";
        return info;
    }

    ifo_handle_t* vmg = ifoOpen(dvd, 0);
    if (!vmg) {
        info.error = "Não foi possível ler o VMG (Video Manager) da imagem.";
        DVDClose(dvd);
        return info;
    }

    int numTitles = vmg->tt_srpt->nr_of_srpts;
    info.volume_label = fs::path(isoPath).stem().string();

    for (int t = 0; t < numTitles; t++) {
        TitleInfo title;
        title.index = t + 1;
        title.name = "Título " + std::to_string(t + 1);
        title.duration_ms = 0;
        title.chapter_count = vmg->tt_srpt->title[t].nr_of_ptts;
        title.selected = (t == 0);

        ifo_handle_t* vts = ifoOpen(dvd, vmg->tt_srpt->title[t].title_set_nr);
        if (vts) {
            populateDvdTitle(vts, vmg->tt_srpt->title[t].vts_ttn, title);
        }
        if (vts) ifoClose(vts);
        info.titles.push_back(title);
    }

    ifoClose(vmg);
    DVDClose(dvd);
    info.valid = true;
    return info;
}

DiscInfo DiscInspector::inspectBluRayIso(const std::string& isoPath) {
    DiscInfo info;
    info.source_path = isoPath;
    info.disc_type = "Blu-ray (ISO)";
    info.valid = false;

    BLURAY* bd = bd_open(isoPath.c_str(), nullptr);
    if (!bd) {
        info.error = "Não foi possível abrir a imagem ISO como Blu-ray.";
        return info;
    }

    const META_DL* meta = bd_get_meta(bd);
    if (meta && meta->di_name) {
        info.volume_label = meta->di_name;
    } else {
        info.volume_label = fs::path(isoPath).stem().string();
    }

    uint32_t num_pl = bd_get_titles(bd, TITLES_ALL, 0);
    for (uint32_t p = 0; p < num_pl; p++) {
        BLURAY_TITLE_INFO* ti = bd_get_title_info(bd, p, 0);
        if (!ti) continue;

        TitleInfo title;
        title.index = p + 1;
        title.name = "Playlist " + std::to_string(ti->playlist);
        title.duration_ms = static_cast<int64_t>(ti->duration / 90);
        title.chapter_count = ti->chapter_count;
        title.selected = (p == 0);
        addBlurayChapters(ti, title);

        for (uint32_t c = 0; c < ti->clip_count; c++) {
            BLURAY_CLIP_INFO* clip = &ti->clips[c];

            for (uint32_t v = 0; v < clip->video_stream_count; v++) {
                TrackInfo track;
                track.index = v;
                track.type = "video";
                track.codec = "H.264";
                track.width = clip->video_streams[v].format;
                track.height = 0;
                track.stream_id = clip->video_streams[v].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }

            for (uint32_t a = 0; a < clip->audio_stream_count; a++) {
                TrackInfo track;
                track.index = a;
                track.type = "audio";
                track.codec = "AC3";
                track.channels = clip->audio_streams[a].format;
                track.language = blurayLanguage(clip->audio_streams[a].lang);
                track.stream_id = clip->audio_streams[a].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }

            for (uint32_t s = 0; s < clip->pg_stream_count; s++) {
                TrackInfo track;
                track.index = s;
                track.type = "subtitle";
                track.codec = "PGS";
                track.language = blurayLanguage(clip->pg_streams[s].lang);
                track.stream_id = clip->pg_streams[s].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }
        }

        bd_free_title_info(ti);
        info.titles.push_back(title);
    }

    bd_close(bd);
    info.valid = true;
    return info;
}

DiscInfo DiscInspector::inspectDvd(const std::string& videoTsPath) {
    DiscInfo info;
    info.source_path = videoTsPath;
    info.disc_type = "DVD-Video";
    info.valid = false;

    dvd_reader_t* dvd = DVDOpen(videoTsPath.c_str());
    if (!dvd) {
        // Try parent directory
        fs::path parent = fs::path(videoTsPath).parent_path();
        dvd = DVDOpen(parent.string().c_str());
        if (!dvd) {
            info.error = "Não foi possível abrir a estrutura DVD-Video com libdvdread.";
            return info;
        }
        info.source_path = parent.string();
    }

    ifo_handle_t* vmg = ifoOpen(dvd, 0);
    if (!vmg) {
        info.error = "Não foi possível ler o VMG (Video Manager).";
        DVDClose(dvd);
        return info;
    }

    int numTitles = vmg->tt_srpt->nr_of_srpts;
    info.volume_label = detectVolumeLabel(info.source_path);

    for (int t = 0; t < numTitles; t++) {
        TitleInfo title;
        title.index = t + 1;
        title.name = "Título " + std::to_string(t + 1);
        title.duration_ms = 0;
        title.chapter_count = vmg->tt_srpt->title[t].nr_of_ptts;
        title.selected = (t == 0); // Select first title by default

        ifo_handle_t* vts = ifoOpen(dvd, vmg->tt_srpt->title[t].title_set_nr);
        if (vts) {
            populateDvdTitle(vts, vmg->tt_srpt->title[t].vts_ttn, title);
        }
        if (vts) ifoClose(vts);
        info.titles.push_back(title);
    }

    ifoClose(vmg);
    DVDClose(dvd);
    info.valid = true;
    return info;
}

DiscInfo DiscInspector::inspectBluRay(const std::string& bdmvPath) {
    DiscInfo info;
    info.source_path = bdmvPath;
    info.disc_type = "Blu-ray";
    info.valid = false;

    // libbluray expects the root containing BDMV/
    fs::path root = fs::path(bdmvPath).parent_path();

    BLURAY* bd = bd_open(root.string().c_str(), nullptr);
    if (!bd) {
        info.error = "Não foi possível abrir a estrutura Blu-ray com libbluray.";
        return info;
    }

    // Get disc title
    const META_DL* meta = bd_get_meta(bd);
    if (meta && meta->di_name) {
        info.volume_label = meta->di_name;
    } else {
        info.volume_label = detectVolumeLabel(root.string());
    }

    // Get playlists
    uint32_t num_pl = bd_get_titles(bd, TITLES_ALL, 0);
    for (uint32_t p = 0; p < num_pl; p++) {
        BLURAY_TITLE_INFO* ti = bd_get_title_info(bd, p, 0);
        if (!ti) continue;

        TitleInfo title;
        title.index = p + 1;
        title.name = "Playlist " + std::to_string(ti->playlist);
        title.duration_ms = static_cast<int64_t>(ti->duration / 90);
        title.chapter_count = ti->chapter_count;
        title.selected = (p == 0);
        addBlurayChapters(ti, title);

        // Video, audio, subtitle clips
        for (uint32_t c = 0; c < ti->clip_count; c++) {
            BLURAY_CLIP_INFO* clip = &ti->clips[c];

            // Video streams
            for (uint32_t v = 0; v < clip->video_stream_count; v++) {
                TrackInfo track;
                track.index = v;
                track.type = "video";
                track.codec = "H.264";
                track.width = clip->video_streams[v].format;
                track.height = 0;
                track.stream_id = clip->video_streams[v].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }

            // Audio streams
            for (uint32_t a = 0; a < clip->audio_stream_count; a++) {
                TrackInfo track;
                track.index = a;
                track.type = "audio";
                track.codec = "AC3";
                track.channels = clip->audio_streams[a].format;
                track.language = blurayLanguage(clip->audio_streams[a].lang);
                track.stream_id = clip->audio_streams[a].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }

            // Subtitle streams (PGS)
            for (uint32_t s = 0; s < clip->pg_stream_count; s++) {
                TrackInfo track;
                track.index = s;
                track.type = "subtitle";
                track.codec = "PGS";
                track.language = blurayLanguage(clip->pg_streams[s].lang);
                track.stream_id = clip->pg_streams[s].pid;
                track.selected = true;
                title.tracks.push_back(track);
            }
        }

        bd_free_title_info(ti);
        info.titles.push_back(title);
    }

    bd_close(bd);
    info.valid = true;
    return info;
}

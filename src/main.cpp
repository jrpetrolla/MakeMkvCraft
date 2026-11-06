#include "MainWindow.h"
#include "MkvRemuxer.h"
#include <QApplication>
#include <QStandardPaths>
#include <cstdarg>
#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

static void ffmpegLog(void*, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char message[2048];
    vsnprintf(message, sizeof(message), format, args);
    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!text.empty()) Logger::instance().log("FFmpeg: " + text);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("MakeMkvCraft");
    app.setApplicationVersion("0.1.0");

    // Initialize logger — writes to log.txt next to the exe
    QString exePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (exePath.isEmpty()) {
        exePath = QApplication::applicationDirPath();
    }
    // Use the exe's directory for log.txt
    Logger::instance().init(QApplication::applicationFilePath().toStdString());
    av_log_set_callback(ffmpegLog);

    MainWindow window;
    window.show();

    return app.exec();
}

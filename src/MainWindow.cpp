#include "MainWindow.h"
#include "MkvRemuxer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QGroupBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QApplication>
#include <QCloseEvent>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QThread>
#include <QDialog>
#include <QDialogButtonBox>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QString formatDuration(int64_t ms) {
    int totalSec = static_cast<int>(ms / 1000);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

namespace {
constexpr int kItemKindRole = Qt::UserRole;
constexpr int kItemPositionRole = Qt::UserRole + 1;
constexpr int kTrackTypeRole = Qt::UserRole + 2;
constexpr int kForcedFromSourceRole = Qt::UserRole + 3;

enum TreeItemKind {
    TitleItem = 1,
    TrackItem = 2,
    ChaptersItem = 3,
    ChapterDetailItem = 4
};

enum TreeColumn {
    SelectionColumn = 0,
    NameColumn = 1,
    EncodingColumn = 2,
    DetailsColumn = 3,
    ChaptersColumn = 4,
    TracksColumn = 5,
    DefaultColumn = 6,
    ForcedColumn = 7
};

#ifdef _WIN32
constexpr int kDriveReadyRole = Qt::UserRole + 10;

struct OpticalDriveInfo {
    QString root;
    QString volumeLabel;
    QString state;
    bool ready = false;
};

static std::vector<OpticalDriveInfo> enumerateOpticalDrives() {
    std::vector<OpticalDriveInfo> drives;
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) return drives;

    std::vector<wchar_t> roots(required + 1, L'\0');
    if (GetLogicalDriveStringsW(required, roots.data()) == 0) return drives;

    for (const wchar_t* root = roots.data(); *root != L'\0';
         root += std::wcslen(root) + 1) {
        if (GetDriveTypeW(root) != DRIVE_CDROM) continue;

        OpticalDriveInfo drive;
        drive.root = QString::fromWCharArray(root);

        wchar_t volumeName[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(root, volumeName, MAX_PATH + 1, nullptr,
                                  nullptr, nullptr, nullptr, 0)) {
            drive.ready = true;
            drive.volumeLabel = QString::fromWCharArray(volumeName);
            drive.state = "pronta";
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) {
                drive.state = "sem mídia";
            } else if (error == ERROR_ACCESS_DENIED) {
                drive.state = "acesso negado";
            } else if (error == ERROR_UNRECOGNIZED_VOLUME) {
                drive.state = "mídia ilegível";
            } else {
                drive.state = QString("erro %1").arg(error);
            }
        }
        drives.push_back(std::move(drive));
    }
    return drives;
}

static QString selectOpticalDrive(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Selecionar unidade óptica");
    dialog.setMinimumWidth(460);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        "Selecione uma unidade com mídia pronta para leitura.", &dialog));

    auto* driveRow = new QHBoxLayout;
    auto* combo = new QComboBox(&dialog);
    auto* refreshButton = new QPushButton("Atualizar", &dialog);
    driveRow->addWidget(combo, 1);
    driveRow->addWidget(refreshButton);
    layout->addLayout(driveRow);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    auto updateOkState = [combo, buttons]() {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            combo->currentData(kDriveReadyRole).toBool());
    };
    auto reload = [combo, updateOkState]() {
        combo->clear();
        const auto drives = enumerateOpticalDrives();
        if (drives.empty()) {
            combo->addItem("Nenhuma unidade óptica encontrada");
            combo->setItemData(0, false, kDriveReadyRole);
            updateOkState();
            return;
        }

        for (const auto& drive : drives) {
            const QString label = drive.volumeLabel.isEmpty()
                ? "sem rótulo" : drive.volumeLabel;
            combo->addItem(
                QString("%1 — %2 — %3")
                    .arg(drive.root.left(2), label, drive.state),
                drive.root);
            const int index = combo->count() - 1;
            combo->setItemData(index, drive.ready, kDriveReadyRole);
        }
        updateOkState();
    };

    QObject::connect(refreshButton, &QPushButton::clicked,
                     &dialog, [reload]() { reload(); });
    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, [updateOkState](int) { updateOkState(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);

    reload();
    if (dialog.exec() != QDialog::Accepted) return {};
    return combo->currentData().toString();
}
#endif

}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUi();
    setWindowTitle("MakeMkvCraft");
    resize(900, 500);
    setMinimumSize(700, 360);
}

MainWindow::~MainWindow() {
    if (m_remuxThread && m_remuxThread->isRunning()) {
        LOG("MainWindow: waiting for active remux thread during shutdown");
        m_remuxThread->wait();
    }
    Logger::instance().close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_remuxThread && m_remuxThread->isRunning()) {
        QMessageBox::information(this, "Remux em andamento",
            "Aguarde o remux terminar antes de fechar o aplicativo.");
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::setupUi() {
    auto central = new QWidget;
    auto outerLayout = new QVBoxLayout(central);

    // --- Toolbar buttons ---
    auto btnBar = new QHBoxLayout;
    m_btnOpenFolder = new QPushButton("Abrir pasta");
    m_btnOpenImage = new QPushButton("Abrir ISO");
    m_btnOpenDrive = new QPushButton("Abrir disco");
    m_btnSaveMkv = new QPushButton("Salvar MKV");

    for (auto* btn : {m_btnOpenFolder, m_btnOpenImage, m_btnOpenDrive,
                      m_btnSaveMkv}) {
        btn->setMinimumHeight(28);
        btnBar->addWidget(btn);
    }
    btnBar->addStretch();
    outerLayout->addLayout(btnBar);

    // --- Source info bar ---
    auto infoBar = new QHBoxLayout;
    m_lblSource = new QLabel("Origem: —");
    m_lblType = new QLabel("Tipo: —");
    m_lblStatus = new QLabel("Pronto.");
    infoBar->addWidget(m_lblSource);
    infoBar->addSpacing(20);
    infoBar->addWidget(m_lblType);
    infoBar->addSpacing(20);
    infoBar->addWidget(m_lblStatus);
    infoBar->addStretch();
    outerLayout->addLayout(infoBar);

    // --- Tree only (no log panel) ---
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(8);
    m_tree->setHeaderLabels(
        {"Sel", "Título", "Encoding", "Duração", "Capítulos", "Faixas",
         "Default", "Forced"});
    m_tree->header()->setSectionResizeMode(SelectionColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int column = EncodingColumn; column <= ForcedColumn; ++column) {
        m_tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    outerLayout->addWidget(m_tree, 1);

    // --- Progress bar ---
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    outerLayout->addWidget(m_progress);

    setCentralWidget(central);

    // --- Connections ---
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &MainWindow::onOpenFolder);
    connect(m_btnOpenImage, &QPushButton::clicked, this, &MainWindow::onOpenImage);
    connect(m_btnOpenDrive, &QPushButton::clicked, this, &MainWindow::onOpenDrive);
    connect(m_btnSaveMkv, &QPushButton::clicked, this, &MainWindow::onSaveMkv);
    connect(m_tree, &QTreeWidget::itemChanged,
            this, &MainWindow::onTreeItemChanged);

    setControlsEnabled(false);
}

void MainWindow::populateTree(const DiscInfo& disc) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clear();

    for (size_t titlePosition = 0; titlePosition < disc.titles.size(); ++titlePosition) {
        const auto& title = disc.titles[titlePosition];
        const bool selectedByDefault = title.output.selected;
        auto* item = new QTreeWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(SelectionColumn,
            selectedByDefault ? Qt::Checked : Qt::Unchecked);
        item->setData(SelectionColumn, kItemKindRole, TitleItem);
        item->setData(SelectionColumn, kItemPositionRole,
            static_cast<int>(titlePosition));
        item->setText(NameColumn, QString::fromStdString(title.name));
        const auto videoTrack = std::find_if(
            title.tracks.begin(), title.tracks.end(),
            [](const TrackInfo& track) { return track.type == "video"; });
        if (videoTrack != title.tracks.end()) {
            item->setText(EncodingColumn, QString::fromStdString(videoTrack->codec));
        }
        item->setText(DetailsColumn, formatDuration(title.duration_ms));
        item->setText(ChaptersColumn, QString::number(title.chapter_count));
        item->setText(TracksColumn, QString::number(title.tracks.size()));

        LOG("populateTree: title=" + std::to_string(title.index) +
            ", tracks=" + std::to_string(title.tracks.size()) +
            ", chapters=" + std::to_string(title.chapters.size()) +
            ", selected=" + (selectedByDefault ? "yes" : "no"));

        for (size_t trackPosition = 0; trackPosition < title.tracks.size(); ++trackPosition) {
            const auto& track = title.tracks[trackPosition];
            auto* child = new QTreeWidgetItem;
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(SelectionColumn,
                track.output.selected ? Qt::Checked : Qt::Unchecked);
            child->setData(SelectionColumn, kItemKindRole, TrackItem);
            child->setData(SelectionColumn, kItemPositionRole,
                static_cast<int>(trackPosition));
            child->setData(SelectionColumn, kTrackTypeRole,
                QString::fromStdString(track.type));
            child->setData(SelectionColumn, kForcedFromSourceRole,
                track.forced_from_source);
            child->setText(NameColumn, QString("%1 #%2")
                .arg(QString::fromStdString(track.type))
                .arg(track.index));
            child->setText(EncodingColumn, QString::fromStdString(track.codec));
            if (track.type == "video")
                child->setText(DetailsColumn,
                    QString("%1x%2").arg(track.width).arg(track.height));
            else if (track.type == "audio") {
                QString details = QString("%1ch").arg(track.channels);
                if (!track.language.empty()) {
                    details += " · " + QString::fromStdString(track.language);
                }
                child->setText(DetailsColumn, details);
            }
            else
                child->setText(DetailsColumn, QString::fromStdString(track.language));

            child->setCheckState(DefaultColumn,
                track.output.mkv_default ? Qt::Checked : Qt::Unchecked);
            child->setToolTip(DefaultColumn,
                "Define a flag DefaultTrack do Matroska; apenas uma por tipo.");
            if (track.type == "subtitle") {
                child->setCheckState(ForcedColumn,
                    track.output.mkv_forced ? Qt::Checked : Qt::Unchecked);
                child->setToolTip(ForcedColumn,
                    "Define a flag ForcedTrack da faixa inteira no Matroska.");
            } else {
                child->setText(ForcedColumn, "—");
            }
            item->addChild(child);

            LOG("populateTree: track position=" + std::to_string(trackPosition) +
                ", type=" + track.type + ", codec=" + track.codec +
                ", id=" + std::to_string(track.stream_id) +
                ", language=" + (track.language.empty() ? "und" : track.language) +
                ", selected=" + (track.output.selected ? "yes" : "no") +
                ", default=" + (track.output.mkv_default ? "yes" : "no") +
                ", forced=" + (track.output.mkv_forced ? "yes" : "no"));
        }

        if (!title.chapters.empty()) {
            auto* chaptersItem = new QTreeWidgetItem;
            chaptersItem->setFlags(chaptersItem->flags() | Qt::ItemIsUserCheckable);
            chaptersItem->setCheckState(SelectionColumn,
                title.output.include_chapters ? Qt::Checked : Qt::Unchecked);
            chaptersItem->setData(SelectionColumn, kItemKindRole, ChaptersItem);
            chaptersItem->setText(NameColumn,
                QString("Capítulos (%1)").arg(title.chapters.size()));
            chaptersItem->setText(DetailsColumn, formatDuration(title.duration_ms));
            chaptersItem->setText(ChaptersColumn,
                QString::number(title.chapters.size()));

            for (const auto& chapter : title.chapters) {
                auto* chapterItem = new QTreeWidgetItem;
                chapterItem->setFlags(
                    chapterItem->flags() & ~Qt::ItemIsUserCheckable);
                chapterItem->setData(SelectionColumn, kItemKindRole,
                    ChapterDetailItem);
                chapterItem->setText(NameColumn,
                    QString::fromStdString(chapter.name));
                chapterItem->setText(DetailsColumn,
                    formatDuration(chapter.start_ms));
                chapterItem->setText(ChaptersColumn,
                    formatDuration(chapter.end_ms - chapter.start_ms));
                chaptersItem->addChild(chapterItem);
            }
            item->addChild(chaptersItem);
        }

        m_tree->addTopLevelItem(item);
    }
    m_tree->collapseAll();
}

void MainWindow::logMessage(const QString& msg) {
    LOG(msg.toStdString());
}

void MainWindow::setControlsEnabled(bool enabled) {
    m_btnSaveMkv->setEnabled(enabled);
}

void MainWindow::setRemuxBusy(bool busy) {
    m_btnOpenFolder->setEnabled(!busy);
    m_btnOpenImage->setEnabled(!busy);
    m_btnOpenDrive->setEnabled(!busy);
    m_tree->setEnabled(!busy);
    setControlsEnabled(!busy && m_currentDisc.valid);
}

void MainWindow::onOpenFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Selecionar pasta do disco");
    if (dir.isEmpty()) return;

    logMessage("Inspecionando: " + dir);
    m_currentDisc = m_inspector.inspectDirectory(dir.toStdString());

    if (!m_currentDisc.valid) {
        QMessageBox::warning(this, "Erro", QString::fromStdString(m_currentDisc.error));
        logMessage("Erro: " + QString::fromStdString(m_currentDisc.error));
        return;
    }

    m_lblSource->setText("Origem: " + QString::fromStdString(m_currentDisc.source_path));
    const std::string discType = discTypeLabel(
        m_currentDisc.disc_kind, m_currentDisc.source_kind);
    m_lblType->setText("Tipo: " + QString::fromStdString(discType));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(discType))
        .arg(m_currentDisc.titles.size()));

    populateTree(m_currentDisc);
    setControlsEnabled(true);
}

void MainWindow::onOpenImage() {
    QString file = QFileDialog::getOpenFileName(this, "Selecionar imagem ISO",
        QString(), "Imagens ISO (*.iso);;Todos os arquivos (*.*)");
    if (file.isEmpty()) return;

    logMessage("Imagem ISO: " + file);
    m_currentDisc = m_inspector.inspectImage(file.toStdString());

    if (!m_currentDisc.valid) {
        QMessageBox::information(this, "Imagem ISO",
            QString::fromStdString(m_currentDisc.error));
        logMessage(QString::fromStdString(m_currentDisc.error));
        return;
    }

    m_lblSource->setText("Origem: " + QString::fromStdString(m_currentDisc.source_path));
    const std::string discType = discTypeLabel(
        m_currentDisc.disc_kind, m_currentDisc.source_kind);
    m_lblType->setText("Tipo: " + QString::fromStdString(discType));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(discType))
        .arg(m_currentDisc.titles.size()));

    populateTree(m_currentDisc);
    setControlsEnabled(true);
}

void MainWindow::onOpenDrive() {
#ifdef _WIN32
    QString drive = selectOpticalDrive(this);
#else
    QString drive = QFileDialog::getExistingDirectory(this, "Selecionar unidade de disco",
        "/dev");
#endif
    if (drive.isEmpty()) return;

    logMessage("Abrindo disco: " + drive);
    m_currentDisc = m_inspector.inspectOpticalDrive(drive.toStdString());

    if (!m_currentDisc.valid) {
        QMessageBox::warning(this, "Erro", QString::fromStdString(m_currentDisc.error));
        logMessage("Erro: " + QString::fromStdString(m_currentDisc.error));
        return;
    }

    m_lblSource->setText("Origem: " + QString::fromStdString(m_currentDisc.source_path));
    const std::string discType = discTypeLabel(
        m_currentDisc.disc_kind, m_currentDisc.source_kind);
    m_lblType->setText("Tipo: " + QString::fromStdString(discType));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(discType))
        .arg(m_currentDisc.titles.size()));

    populateTree(m_currentDisc);
    setControlsEnabled(true);
}

void MainWindow::onRefreshTree() {
    if (m_currentDisc.valid)
        populateTree(m_currentDisc);
}

void MainWindow::onTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (!item) return;

    const int kind = item->data(SelectionColumn, kItemKindRole).toInt();
    const QSignalBlocker blocker(m_tree);

    auto trackCount = [](QTreeWidgetItem* titleItem, const QString& type) {
        int count = 0;
        if (!titleItem) return count;
        for (int i = 0; i < titleItem->childCount(); ++i) {
            QTreeWidgetItem* child = titleItem->child(i);
            if (child->data(SelectionColumn, kItemKindRole).toInt() == TrackItem &&
                child->data(SelectionColumn, kTrackTypeRole).toString() == type) {
                ++count;
            }
        }
        return count;
    };

    auto initializeSelectedTrackFlags = [&](QTreeWidgetItem* trackItem) {
        const QString type =
            trackItem->data(SelectionColumn, kTrackTypeRole).toString();
        if (type == "video" && trackCount(trackItem->parent(), type) == 1) {
            trackItem->setCheckState(DefaultColumn, Qt::Checked);
        }
        if (type == "subtitle" &&
            trackItem->data(SelectionColumn, kForcedFromSourceRole).toBool()) {
            trackItem->setCheckState(ForcedColumn, Qt::Checked);
        }
    };

    if (kind == TitleItem && column == SelectionColumn) {
        const Qt::CheckState state = item->checkState(SelectionColumn);
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem* child = item->child(i);
            const int childKind =
                child->data(SelectionColumn, kItemKindRole).toInt();
            if (childKind != TrackItem && childKind != ChaptersItem) continue;

            child->setCheckState(SelectionColumn, state);
            if (childKind == TrackItem) {
                if (state == Qt::Unchecked) {
                    child->setCheckState(DefaultColumn, Qt::Unchecked);
                    if (child->data(SelectionColumn, kTrackTypeRole).toString() ==
                        "subtitle") {
                        child->setCheckState(ForcedColumn, Qt::Unchecked);
                    }
                } else {
                    initializeSelectedTrackFlags(child);
                }
            }
        }
        return;
    }

    if (kind != TrackItem) return;

    const QString type = item->data(SelectionColumn, kTrackTypeRole).toString();
    if (column == SelectionColumn) {
        if (item->checkState(SelectionColumn) == Qt::Unchecked) {
            item->setCheckState(DefaultColumn, Qt::Unchecked);
            if (type == "subtitle") {
                item->setCheckState(ForcedColumn, Qt::Unchecked);
            }
        } else {
            initializeSelectedTrackFlags(item);
        }
        return;
    }

    if (column == DefaultColumn) {
        if (item->checkState(DefaultColumn) == Qt::Checked) {
            item->setCheckState(SelectionColumn, Qt::Checked);
            QTreeWidgetItem* titleItem = item->parent();
            for (int i = 0; titleItem && i < titleItem->childCount(); ++i) {
                QTreeWidgetItem* sibling = titleItem->child(i);
                if (sibling != item &&
                    sibling->data(SelectionColumn, kItemKindRole).toInt() == TrackItem &&
                    sibling->data(SelectionColumn, kTrackTypeRole).toString() == type) {
                    sibling->setCheckState(DefaultColumn, Qt::Unchecked);
                }
            }
        } else if (type == "video" &&
                   item->checkState(SelectionColumn) == Qt::Checked &&
                   trackCount(item->parent(), type) == 1) {
            item->setCheckState(DefaultColumn, Qt::Checked);
        }
        return;
    }

    if (column == ForcedColumn && type == "subtitle" &&
        item->checkState(ForcedColumn) == Qt::Checked) {
        item->setCheckState(SelectionColumn, Qt::Checked);
    }
}

void MainWindow::onSaveMkv() {
    if (!m_currentDisc.valid || m_remuxThread) return;

    for (int i = 0; i < m_tree->topLevelItemCount() && i < (int)m_currentDisc.titles.size(); i++) {
        QTreeWidgetItem* titleItem = m_tree->topLevelItem(i);
        TitleInfo& title = m_currentDisc.titles[i];
        title.output.selected =
            titleItem->checkState(SelectionColumn) == Qt::Checked;
        std::set<std::string> defaultTrackTypes;

        for (int childIndex = 0; childIndex < titleItem->childCount(); ++childIndex) {
            QTreeWidgetItem* child = titleItem->child(childIndex);
            int kind = child->data(SelectionColumn, kItemKindRole).toInt();
            if (kind == TrackItem) {
                int trackPosition =
                    child->data(SelectionColumn, kItemPositionRole).toInt();
                if (trackPosition >= 0 &&
                    trackPosition < static_cast<int>(title.tracks.size())) {
                    TrackInfo& track = title.tracks[trackPosition];
                    track.output.selected =
                        child->checkState(SelectionColumn) == Qt::Checked;
                    track.output.mkv_default = track.output.selected &&
                        child->checkState(DefaultColumn) == Qt::Checked;
                    track.output.mkv_forced = track.output.selected &&
                        track.type == "subtitle" &&
                        child->checkState(ForcedColumn) == Qt::Checked;

                    if (track.output.mkv_default &&
                        !defaultTrackTypes.insert(track.type).second) {
                        QMessageBox::information(this, "Salvar MKV",
                            QString("O título %1 possui mais de uma faixa %2 "
                                    "marcada como Default.")
                                .arg(title.index)
                                .arg(QString::fromStdString(track.type)));
                        return;
                    }
                }
            } else if (kind == ChaptersItem) {
                title.output.include_chapters =
                    child->checkState(SelectionColumn) == Qt::Checked;
            }
        }

        int selectedTracks = static_cast<int>(std::count_if(
            title.tracks.begin(), title.tracks.end(),
            [](const TrackInfo& track) { return track.output.selected; }));
        LOG("selection: title=" + std::to_string(title.index) +
            ", selected=" + (title.output.selected ? "yes" : "no") +
            ", selected_tracks=" + std::to_string(selectedTracks) +
            ", include_chapters=" +
            (title.output.include_chapters ? "yes" : "no"));

        if (title.output.selected && selectedTracks == 0) {
            QMessageBox::information(this, "Salvar MKV",
                QString("O título %1 não possui nenhuma faixa selecionada.")
                    .arg(title.index));
            return;
        }
    }

    int selCount = 0;
    for (const auto& t : m_currentDisc.titles)
        if (t.output.selected) selCount++;

    if (selCount == 0) {
        QMessageBox::information(this, "Salvar MKV", "Nenhum título selecionado.");
        return;
    }

    QString outDir = QFileDialog::getExistingDirectory(this, "Pasta de destino",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (outDir.isEmpty()) return;

    logMessage(QString("Iniciando remux de %1 título(s) para %2").arg(selCount).arg(outDir));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFormat("%p% — Preparando");
    setRemuxBusy(true);

    struct RemuxResult {
        int success = 0;
        std::string error;
    };

    auto result = std::make_shared<RemuxResult>();
    DiscInfo discSnapshot = m_currentDisc;
    std::string outputFolder = outDir.toStdString();

    QThread* worker = QThread::create(
        [this, disc = std::move(discSnapshot), outputFolder, result]() mutable {
            try {
                auto progressCallback = [this](int value, const std::string& status) {
                    const QString statusText = QString::fromStdString(status);
                    QMetaObject::invokeMethod(this,
                        [this, value, statusText]() {
                            if (!m_remuxThread) return;
                            m_progress->setValue(std::clamp(value, 0, 1000));
                            m_progress->setFormat("%p% — " + statusText);
                        },
                        Qt::QueuedConnection);
                };
                MkvRemuxer remuxer(progressCallback);
                result->success = remuxer.remuxSelected(disc, outputFolder, result->error);
            } catch (const std::exception& ex) {
                result->error = std::string("Exceção durante o remux: ") + ex.what();
                LOG("remux worker exception: " + result->error);
            } catch (...) {
                result->error = "Exceção desconhecida durante o remux.";
                LOG("remux worker exception: unknown exception");
            }
        });
    worker->setObjectName("MkvRemuxWorker");
    worker->setParent(this);
    m_remuxThread = worker;

    connect(worker, &QThread::finished, this,
        [this, worker, result, selCount, outDir]() {
            if (m_remuxThread == worker) m_remuxThread = nullptr;
            worker->deleteLater();

            const int success = result->success;
            const QString errorText = QString::fromStdString(result->error);
            m_progress->setRange(0, 1000);
            setRemuxBusy(false);

            if (success == selCount) {
                m_progress->setValue(1000);
                m_progress->setFormat("%p% — Concluído");
                logMessage(QString("Remux concluído: %1/%2 título(s) salvos.")
                    .arg(success).arg(selCount));
                QString message = QString("%1 arquivo(s) MKV criados em:\n%2")
                    .arg(success).arg(outDir);
                if (!result->error.empty()) {
                    logMessage(errorText);
                    QMessageBox::warning(this, "Salvar MKV", message + "\n\n" + errorText);
                } else {
                    QMessageBox::information(this, "Salvar MKV", message);
                }
            } else if (success > 0) {
                m_progress->setFormat("%p% — Falha após conclusão parcial");
                logMessage(QString("Remux parcial: %1/%2. Erro: %3")
                    .arg(success).arg(selCount).arg(errorText));
                QMessageBox::warning(this, "Salvar MKV",
                    QString("%1 de %2 concluídos.\nErro: %3\n\nVeja log.txt para detalhes.")
                        .arg(success).arg(selCount).arg(errorText));
            } else {
                m_progress->setFormat("%p% — Falha");
                logMessage("Erro no remux: " + errorText);
                QMessageBox::critical(this, "Salvar MKV",
                    errorText + "\n\nVeja log.txt para detalhes.");
            }
        });

    worker->start();
}

void MainWindow::onClearLog() {
    // No-op — log now goes to log.txt
}

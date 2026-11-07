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
#include <algorithm>
#include <memory>

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

enum TreeItemKind {
    TitleItem = 1,
    TrackItem = 2,
    ChaptersItem = 3,
    ChapterDetailItem = 4
};

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
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels(
        {"Sel", "Título", "Encoding", "Duração", "Capítulos", "Faixas"});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
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

    size_t longestTitlePosition = disc.titles.size();
    if (!disc.titles.empty()) {
        const auto longestTitle = std::max_element(
            disc.titles.begin(), disc.titles.end(),
            [](const TitleInfo& left, const TitleInfo& right) {
                return left.duration_ms < right.duration_ms;
            });
        longestTitlePosition = static_cast<size_t>(
            std::distance(disc.titles.begin(), longestTitle));
    }

    for (size_t titlePosition = 0; titlePosition < disc.titles.size(); ++titlePosition) {
        const auto& title = disc.titles[titlePosition];
        const bool selectedByDefault = titlePosition == longestTitlePosition;
        auto* item = new QTreeWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, selectedByDefault ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kItemKindRole, TitleItem);
        item->setData(0, kItemPositionRole, static_cast<int>(titlePosition));
        item->setText(1, QString::fromStdString(title.name));
        const auto videoTrack = std::find_if(
            title.tracks.begin(), title.tracks.end(),
            [](const TrackInfo& track) { return track.type == "video"; });
        if (videoTrack != title.tracks.end()) {
            item->setText(2, QString::fromStdString(videoTrack->codec));
        }
        item->setText(3, formatDuration(title.duration_ms));
        item->setText(4, QString::number(title.chapter_count));
        item->setText(5, QString::number(title.tracks.size()));

        LOG("populateTree: title=" + std::to_string(title.index) +
            ", tracks=" + std::to_string(title.tracks.size()) +
            ", chapters=" + std::to_string(title.chapters.size()) +
            ", selected=" + (selectedByDefault ? "yes" : "no"));

        for (size_t trackPosition = 0; trackPosition < title.tracks.size(); ++trackPosition) {
            const auto& track = title.tracks[trackPosition];
            auto* child = new QTreeWidgetItem;
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0,
                selectedByDefault ? Qt::Checked : Qt::Unchecked);
            child->setData(0, kItemKindRole, TrackItem);
            child->setData(0, kItemPositionRole, static_cast<int>(trackPosition));
            child->setText(1, QString("%1 #%2")
                .arg(QString::fromStdString(track.type))
                .arg(track.index));
            child->setText(2, QString::fromStdString(track.codec));
            if (track.type == "video")
                child->setText(3, QString("%1x%2").arg(track.width).arg(track.height));
            else if (track.type == "audio") {
                QString details = QString("%1ch").arg(track.channels);
                if (!track.language.empty()) {
                    details += " · " + QString::fromStdString(track.language);
                }
                child->setText(3, details);
            }
            else
                child->setText(3, QString::fromStdString(track.language));
            item->addChild(child);

            LOG("populateTree: track position=" + std::to_string(trackPosition) +
                ", type=" + track.type + ", codec=" + track.codec +
                ", id=" + std::to_string(track.stream_id) +
                ", language=" + (track.language.empty() ? "und" : track.language) +
                ", selected=" + (selectedByDefault ? "yes" : "no"));
        }

        if (!title.chapters.empty()) {
            auto* chaptersItem = new QTreeWidgetItem;
            chaptersItem->setFlags(chaptersItem->flags() | Qt::ItemIsUserCheckable);
            chaptersItem->setCheckState(0,
                selectedByDefault ? Qt::Checked : Qt::Unchecked);
            chaptersItem->setData(0, kItemKindRole, ChaptersItem);
            chaptersItem->setText(1,
                QString("Capítulos (%1)").arg(title.chapters.size()));
            chaptersItem->setText(3, formatDuration(title.duration_ms));
            chaptersItem->setText(4, QString::number(title.chapters.size()));

            for (const auto& chapter : title.chapters) {
                auto* chapterItem = new QTreeWidgetItem;
                chapterItem->setFlags(
                    chapterItem->flags() & ~Qt::ItemIsUserCheckable);
                chapterItem->setData(0, kItemKindRole, ChapterDetailItem);
                chapterItem->setText(1, QString::fromStdString(chapter.name));
                chapterItem->setText(3, formatDuration(chapter.start_ms));
                chapterItem->setText(4,
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
    m_lblType->setText("Tipo: " + QString::fromStdString(m_currentDisc.disc_type));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(m_currentDisc.disc_type))
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
    m_lblType->setText("Tipo: " + QString::fromStdString(m_currentDisc.disc_type));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(m_currentDisc.disc_type))
        .arg(m_currentDisc.titles.size()));

    populateTree(m_currentDisc);
    setControlsEnabled(true);
}

void MainWindow::onOpenDrive() {
    QString drive = QFileDialog::getExistingDirectory(this, "Selecionar unidade de disco",
        "/dev");
    if (drive.isEmpty()) return;

    logMessage("Abrindo disco: " + drive);
    m_currentDisc = m_inspector.inspectDirectory(drive.toStdString());

    if (!m_currentDisc.valid) {
        QMessageBox::warning(this, "Erro", QString::fromStdString(m_currentDisc.error));
        logMessage("Erro: " + QString::fromStdString(m_currentDisc.error));
        return;
    }

    m_lblSource->setText("Origem: " + QString::fromStdString(m_currentDisc.source_path));
    m_lblType->setText("Tipo: " + QString::fromStdString(m_currentDisc.disc_type));
    m_lblStatus->setText(QString("%1 título(s) encontrado(s).").arg(m_currentDisc.titles.size()));
    logMessage(QString("Detectado: %1, %2 título(s)")
        .arg(QString::fromStdString(m_currentDisc.disc_type))
        .arg(m_currentDisc.titles.size()));

    populateTree(m_currentDisc);
    setControlsEnabled(true);
}

void MainWindow::onRefreshTree() {
    if (m_currentDisc.valid)
        populateTree(m_currentDisc);
}

void MainWindow::onTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (!item || column != 0 ||
        item->data(0, kItemKindRole).toInt() != TitleItem) {
        return;
    }

    const QSignalBlocker blocker(m_tree);
    const Qt::CheckState state = item->checkState(0);
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        if (child->flags() & Qt::ItemIsUserCheckable) {
            child->setCheckState(0, state);
        }
    }
}

void MainWindow::onSaveMkv() {
    if (!m_currentDisc.valid || m_remuxThread) return;

    for (int i = 0; i < m_tree->topLevelItemCount() && i < (int)m_currentDisc.titles.size(); i++) {
        QTreeWidgetItem* titleItem = m_tree->topLevelItem(i);
        TitleInfo& title = m_currentDisc.titles[i];
        title.selected = titleItem->checkState(0) == Qt::Checked;

        for (int childIndex = 0; childIndex < titleItem->childCount(); ++childIndex) {
            QTreeWidgetItem* child = titleItem->child(childIndex);
            int kind = child->data(0, kItemKindRole).toInt();
            if (kind == TrackItem) {
                int trackPosition = child->data(0, kItemPositionRole).toInt();
                if (trackPosition >= 0 &&
                    trackPosition < static_cast<int>(title.tracks.size())) {
                    title.tracks[trackPosition].selected =
                        child->checkState(0) == Qt::Checked;
                }
            } else if (kind == ChaptersItem) {
                title.include_chapters = child->checkState(0) == Qt::Checked;
            }
        }

        int selectedTracks = static_cast<int>(std::count_if(
            title.tracks.begin(), title.tracks.end(),
            [](const TrackInfo& track) { return track.selected; }));
        LOG("selection: title=" + std::to_string(title.index) +
            ", selected=" + (title.selected ? "yes" : "no") +
            ", selected_tracks=" + std::to_string(selectedTracks) +
            ", include_chapters=" + (title.include_chapters ? "yes" : "no"));

        if (title.selected && selectedTracks == 0) {
            QMessageBox::information(this, "Salvar MKV",
                QString("O título %1 não possui nenhuma faixa selecionada.")
                    .arg(title.index));
            return;
        }
    }

    int selCount = 0;
    for (const auto& t : m_currentDisc.titles)
        if (t.selected) selCount++;

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

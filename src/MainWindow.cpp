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

void setCheckStateRecursively(QTreeWidgetItem* item, Qt::CheckState state) {
    if (item->flags() & Qt::ItemIsUserCheckable) {
        item->setCheckState(0, state);
    }
    for (int i = 0; i < item->childCount(); ++i) {
        setCheckStateRecursively(item->child(i), state);
    }
}
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
    m_btnEject = new QPushButton("Ejetar");
    m_btnSaveMkv = new QPushButton("Salvar MKV");
    m_btnCancel = new QPushButton("Cancelar");
    m_btnSelectAll = new QPushButton("Selecionar tudo");
    m_btnSelectNone = new QPushButton("Desmarcar");
    m_btnSettings = new QPushButton("Configurações");
    m_btnAbout = new QPushButton("Sobre");

    for (auto* btn : {m_btnOpenFolder, m_btnOpenImage, m_btnOpenDrive, m_btnEject,
                      m_btnSaveMkv, m_btnCancel,
                      m_btnSelectAll, m_btnSelectNone, m_btnSettings, m_btnAbout}) {
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
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({"Sel", "Título", "Duração", "Capítulos", "Faixas"});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
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
    connect(m_btnEject, &QPushButton::clicked, this, &MainWindow::onEjectDisc);
    connect(m_btnSaveMkv, &QPushButton::clicked, this, &MainWindow::onSaveMkv);
    connect(m_btnCancel, &QPushButton::clicked, this, &MainWindow::onCancelJob);
    connect(m_btnSelectAll, &QPushButton::clicked, this, &MainWindow::onSelectAll);
    connect(m_btnSelectNone, &QPushButton::clicked, this, &MainWindow::onSelectNone);
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::onSettings);
    connect(m_btnAbout, &QPushButton::clicked, this, &MainWindow::onAbout);

    m_btnCancel->setEnabled(false);
    setControlsEnabled(false);
}

void MainWindow::populateTree(const DiscInfo& disc) {
    m_tree->clear();

    for (size_t titlePosition = 0; titlePosition < disc.titles.size(); ++titlePosition) {
        const auto& title = disc.titles[titlePosition];
        auto* item = new QTreeWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, title.selected ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kItemKindRole, TitleItem);
        item->setData(0, kItemPositionRole, static_cast<int>(titlePosition));
        item->setText(1, QString::fromStdString(title.name));
        item->setText(2, formatDuration(title.duration_ms));
        item->setText(3, QString::number(title.chapter_count));
        item->setText(4, QString::number(title.tracks.size()));

        LOG("populateTree: title=" + std::to_string(title.index) +
            ", tracks=" + std::to_string(title.tracks.size()) +
            ", chapters=" + std::to_string(title.chapters.size()));

        for (size_t trackPosition = 0; trackPosition < title.tracks.size(); ++trackPosition) {
            const auto& track = title.tracks[trackPosition];
            auto* child = new QTreeWidgetItem;
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, track.selected ? Qt::Checked : Qt::Unchecked);
            child->setData(0, kItemKindRole, TrackItem);
            child->setData(0, kItemPositionRole, static_cast<int>(trackPosition));
            child->setText(1, QString("%1 #%2 — %3")
                .arg(QString::fromStdString(track.type))
                .arg(track.index)
                .arg(QString::fromStdString(track.codec)));
            if (track.type == "video")
                child->setText(2, QString("%1x%2").arg(track.width).arg(track.height));
            else if (track.type == "audio") {
                QString details = QString("%1ch").arg(track.channels);
                if (!track.language.empty()) {
                    details += " · " + QString::fromStdString(track.language);
                }
                child->setText(2, details);
            }
            else
                child->setText(2, QString::fromStdString(track.language));
            item->addChild(child);

            LOG("populateTree: track position=" + std::to_string(trackPosition) +
                ", type=" + track.type + ", codec=" + track.codec +
                ", id=" + std::to_string(track.stream_id) +
                ", language=" + (track.language.empty() ? "und" : track.language) +
                ", selected=" + (track.selected ? "yes" : "no"));
        }

        if (!title.chapters.empty()) {
            auto* chaptersItem = new QTreeWidgetItem;
            chaptersItem->setFlags(chaptersItem->flags() | Qt::ItemIsUserCheckable);
            chaptersItem->setCheckState(0,
                title.include_chapters ? Qt::Checked : Qt::Unchecked);
            chaptersItem->setData(0, kItemKindRole, ChaptersItem);
            chaptersItem->setText(1,
                QString("Capítulos (%1)").arg(title.chapters.size()));
            chaptersItem->setText(2, formatDuration(title.duration_ms));
            chaptersItem->setText(3, QString::number(title.chapters.size()));

            for (const auto& chapter : title.chapters) {
                auto* chapterItem = new QTreeWidgetItem;
                chapterItem->setFlags(
                    chapterItem->flags() & ~Qt::ItemIsUserCheckable);
                chapterItem->setData(0, kItemKindRole, ChapterDetailItem);
                chapterItem->setText(1, QString::fromStdString(chapter.name));
                chapterItem->setText(2, formatDuration(chapter.start_ms));
                chapterItem->setText(3,
                    formatDuration(chapter.end_ms - chapter.start_ms));
                chaptersItem->addChild(chapterItem);
            }
            item->addChild(chaptersItem);
        }

        m_tree->addTopLevelItem(item);
    }
    m_tree->expandAll();
}

void MainWindow::logMessage(const QString& msg) {
    LOG(msg.toStdString());
}

void MainWindow::setControlsEnabled(bool enabled) {
    m_btnSaveMkv->setEnabled(enabled);
    m_btnSelectAll->setEnabled(enabled);
    m_btnSelectNone->setEnabled(enabled);
    m_btnEject->setEnabled(enabled);
}

void MainWindow::setRemuxBusy(bool busy) {
    m_btnOpenFolder->setEnabled(!busy);
    m_btnOpenImage->setEnabled(!busy);
    m_btnOpenDrive->setEnabled(!busy);
    m_btnSettings->setEnabled(!busy);
    m_tree->setEnabled(!busy);
    m_btnCancel->setEnabled(false);
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

void MainWindow::onEjectDisc() {
    logMessage("Ejetar disco — não implementado neste sistema.");
}

void MainWindow::onRefreshTree() {
    if (m_currentDisc.valid)
        populateTree(m_currentDisc);
}

void MainWindow::onSelectAll() {
    for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
        setCheckStateRecursively(m_tree->topLevelItem(i), Qt::Checked);
    }
}

void MainWindow::onSelectNone() {
    for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
        setCheckStateRecursively(m_tree->topLevelItem(i), Qt::Unchecked);
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
    m_lblStatus->setText("Preparando remux...");
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
                            m_lblStatus->setText(statusText);
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
                m_lblStatus->setText(QString("Concluído: %1 arquivo(s) criados.").arg(success));
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
                m_lblStatus->setText(QString("Parcial: %1 de %2 concluídos.")
                    .arg(success).arg(selCount));
                logMessage(QString("Remux parcial: %1/%2. Erro: %3")
                    .arg(success).arg(selCount).arg(errorText));
                QMessageBox::warning(this, "Salvar MKV",
                    QString("%1 de %2 concluídos.\nErro: %3\n\nVeja log.txt para detalhes.")
                        .arg(success).arg(selCount).arg(errorText));
            } else {
                m_progress->setFormat("%p% — Falha");
                m_lblStatus->setText("Falha no remux.");
                logMessage("Erro no remux: " + errorText);
                QMessageBox::critical(this, "Salvar MKV",
                    errorText + "\n\nVeja log.txt para detalhes.");
            }
        });

    worker->start();
}

void MainWindow::onCancelJob() {
    logMessage("Cancelar — não implementado nesta versão.");
}

void MainWindow::onClearLog() {
    // No-op — log now goes to log.txt
}

void MainWindow::onSettings() {
    QMessageBox::information(this, "Configurações",
        "Diálogo de configurações — será implementado.\n\n"
        "Incluíra: pasta de saída padrão, preferência de idioma,\n"
        "seleção automática de faixas, e configurações de mux.");
}

void MainWindow::onAbout() {
    QMessageBox::about(this, "Sobre MakeMkvCraft",
        "<h3>MakeMkvCraft</h3>"
        "<p>Aplicativo livre e independente para remux de DVD/Blu-ray → MKV.</p>"
        "<p>Baseado em Qt 5, libbluray, libdvdread, libdvdnav e FFmpeg.</p>"
        "<p>Versão 0.1.0 — GPL-3.0-or-later.</p>"
        "<p><small>Projeto não afiliado nem endossado pelo MakeMKV ou pela GuinpinSoft.</small></p>");
}

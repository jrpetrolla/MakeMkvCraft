#pragma once

#include <QMainWindow>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QCheckBox>
#include <QString>
#include "DiscInspector.h"
#include "MkvRemuxer.h"

class QCloseEvent;
class QThread;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onOpenFolder();
    void onOpenImage();
    void onOpenDrive();
    void onEjectDisc();
    void onRefreshTree();
    void onSelectAll();
    void onSelectNone();
    void onSaveMkv();
    void onCancelJob();
    void onClearLog();
    void onSettings();
    void onAbout();

private:
    void setupUi();
    void populateTree(const DiscInfo& disc);
    void logMessage(const QString& msg);
    void setControlsEnabled(bool enabled);
    void setRemuxBusy(bool busy);

    QPushButton* m_btnOpenFolder;
    QPushButton* m_btnOpenImage;
    QPushButton* m_btnOpenDrive;
    QPushButton* m_btnEject;
    QPushButton* m_btnSaveMkv;
    QPushButton* m_btnCancel;
    QPushButton* m_btnSelectAll;
    QPushButton* m_btnSelectNone;
    QPushButton* m_btnSettings;
    QPushButton* m_btnAbout;

    QTreeWidget* m_tree;

    QLabel* m_lblSource;
    QLabel* m_lblType;
    QLabel* m_lblStatus;
    QProgressBar* m_progress;

    DiscInspector m_inspector;
    DiscInfo m_currentDisc;
    QThread* m_remuxThread = nullptr;
};

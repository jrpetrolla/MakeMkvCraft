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
    void onRefreshTree();
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onSaveMkv();
    void onClearLog();

private:
    void setupUi();
    void populateTree(const DiscInfo& disc);
    void logMessage(const QString& msg);
    void setControlsEnabled(bool enabled);
    void setRemuxBusy(bool busy);

    QPushButton* m_btnOpenFolder;
    QPushButton* m_btnOpenImage;
    QPushButton* m_btnOpenDrive;
    QPushButton* m_btnSaveMkv;

    QTreeWidget* m_tree;

    QLabel* m_lblSource;
    QLabel* m_lblType;
    QLabel* m_lblStatus;
    QProgressBar* m_progress;

    DiscInspector m_inspector;
    DiscInfo m_currentDisc;
    QThread* m_remuxThread = nullptr;
};

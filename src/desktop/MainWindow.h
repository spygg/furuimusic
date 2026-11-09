#pragma once

#include <QMainWindow>
#include <QVector>
#include <QPair>

#include "../core/AppController.h"

class QListWidget;
class QStackedWidget;
class QLineEdit;
class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QSlider;
class QCheckBox;
class QSpinBox;
class QCloseEvent;
class QResizeEvent;
class QPoint;
class LyricsWindow;

namespace Furui {
class CoverArt;
class SeekSlider;
class ValueSlider;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    // 断点：宽屏三栏（导航+内容+队列抽屉可开），中屏收图标栏，窄屏竖屏（底部标签+抽屉全屏）
    enum LayoutMode { Wide, Medium, Portrait };

    QWidget *createSearchPage();
    QWidget *createRankPage();
    QWidget *createMyPlaylistsPage();
    QWidget *createFavoritesPage();
    QWidget *createHistoryPage();
    QWidget *createAccountPage();
    QWidget *createSettingsPage();
    QWidget *createPlayBar();
    QWidget *createQueuePanel();

    void fillSongTable(QTreeWidget *table, const QVector<Furui::Song> &songs);
    void fillPlaylistTable(QTreeWidget *table);
    void refreshFromResults();
    void refreshPlaylistTables();
    void refreshAccountsTable();
    void refreshQueue();
    void updatePlayerInfo();
    void updatePlayButton();
    void updatePosition();
    void updateSettingsControls();
    void connectSignals();
    void showMessage(const QString &text);
    void onSearchContextMenu(const QPoint &pos);
    void songContextMenu(QTreeWidget *table, const QPoint &pos);
    void onQueueContextMenu(const QPoint &pos);
    void toggleLyrics();
    void openLoginDialog();
    void openSettingsDialog();

    void applyLayoutMode();
    void rebuildPlayBar();
    void rebuildSearchToolbar();
    void tuneTableColumns();
    void setQueueDrawerVisible(bool visible);
    void rethemeIcons();

    Furui::AppController *app;

    QWidget *m_shell;
    QListWidget *m_nav;
    QListWidget *m_tabBar;
    QStackedWidget *m_stack;

    QLineEdit *m_searchEdit;
    QComboBox *m_sourceCombo;
    QComboBox *m_searchTypeCombo;
    QTreeWidget *m_searchTable;
    QWidget *m_searchBar = nullptr;
    QPushButton *m_searchBtn = nullptr;

    QTreeWidget *m_rankTable;
    QTreeWidget *m_myPlaylistTable;
    QTreeWidget *m_favTable;
    QTreeWidget *m_historyTable;
    QTreeWidget *m_accountTable;

    QComboBox *m_quickQuality;
    QCheckBox *m_quickVip;
    Furui::ValueSlider *m_quickVolume;
    QComboBox *m_quickMode;
    QComboBox *m_quickTheme;

    QWidget *m_queuePanel;
    QListWidget *m_queueList;
    QPushButton *m_queueBtn;

    Furui::CoverArt *m_cover;
    QWidget *m_barInfo;
    QWidget *m_barControls;
    QWidget *m_barMeta;
    QLabel *m_title;
    QLabel *m_artist;
    QLabel *m_vipBadge;
    QPushButton *m_prevBtn;
    QPushButton *m_playBtn;
    QPushButton *m_nextBtn;
    Furui::SeekSlider *m_progress;
    QSlider *m_volume;
    QPushButton *m_volumeBtn;
    QPushButton *m_modeBtn;
    QPushButton *m_lyricsBtn;
    QPushButton *m_favCurrentBtn;
    QLabel *m_time;
    QLabel *m_statusLabel;
    QWidget *m_playBar;

    LyricsWindow *m_lyricsWindow;
    LayoutMode m_mode;
    bool m_queueOpen;
    // 主题切换后要按新配色重画矢量图标：记录 (按钮, 图标种类, 是否强调色)
    QVector<QPair<QPushButton *, int>> m_iconButtons;
};

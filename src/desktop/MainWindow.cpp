#include "MainWindow.h"
#include "LoginDialog.h"
#include "SettingsDialog.h"
#include "LyricsWindow.h"

#include <QApplication>
#include <QtGlobal>
#include <QListWidget>
#include <QListWidgetItem>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QCheckBox>
#include <QSpinBox>
#include <QMenu>
#include <QAction>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QStatusBar>
#include <QSettings>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QListView>
#include <QGraphicsDropShadowEffect>
#include <QStyle>

#include "Theme.h"
#include "PlayerWidgets.h"

using namespace Furui;

namespace {

QString formatTime(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 total = ms / 1000;
    const int m = int(total / 60);
    const int s = int(total % 60);
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QTreeWidget *makeSongTable(QWidget *parent)
{
    QTreeWidget *t = new QTreeWidget(parent);
    t->setColumnCount(6);
    t->setHeaderLabels(QStringList()
                       << QStringLiteral("歌名") << QStringLiteral("歌手")
                       << QStringLiteral("专辑") << QStringLiteral("时长")
                       << QStringLiteral("来源") << QStringLiteral("VIP"));
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return t;
}

QTreeWidget *makePlaylistTable(QWidget *parent)
{
    QTreeWidget *t = new QTreeWidget(parent);
    t->setColumnCount(5);
    t->setHeaderLabels(QStringList()
                       << QStringLiteral("标题") << QStringLiteral("所有者")
                       << QStringLiteral("歌曲数") << QStringLiteral("类型")
                       << QStringLiteral("来源"));
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return t;
}


// 导航项：文字 + 矢量图标，横屏显示文字，窄屏只留图标
struct NavEntry {
    const char *label;
    Furui::Icons::Kind kind;
};
const NavEntry kNavEntries[] = {
    {"搜索", Furui::Icons::Search},
    {"榜单", Furui::Icons::Rank},
    {"我的歌单", Furui::Icons::Playlist},
    {"收藏", Furui::Icons::Heart},
    {"播放历史", Furui::Icons::History},
    {"账号", Furui::Icons::Account},
    {"设置", Furui::Icons::Settings},
};
const int kNavCount = int(sizeof(kNavEntries) / sizeof(kNavEntries[0]));

// 换布局：删掉旧的原地重建，控件本身留在宿主里，所以同一套控件能横竖两用
void installLayout(QWidget *host, QLayout *next)
{
    if (!host) return;
    if (QLayout *old = host->layout()) delete old;
    next->setParent(host);
    host->setLayout(next);
}

QVBoxLayout *vbox(int spacing, int margin)
{
    QVBoxLayout *l = new QVBoxLayout();
    l->setSpacing(spacing);
    l->setContentsMargins(margin, margin, margin, margin);
    return l;
}

QHBoxLayout *hbox(int spacing, int margin)
{
    QHBoxLayout *l = new QHBoxLayout();
    l->setSpacing(spacing);
    l->setContentsMargins(margin, margin, margin, margin);
    return l;
}

// 列表为空时给出中央提示；覆盖层以列表为父对象，随列表一起销毁
void setEmptyState(QTreeWidget *table, const QString &text)
{
    if (!table) return;
    EmptyOverlay *overlay = table->findChild<EmptyOverlay *>();
    if (!overlay) overlay = new EmptyOverlay(table, text);
    overlay->setText(text);
    overlay->showFor(table->topLevelItemCount());
}

QColor navIconColor(const Furui::Tokens &t, bool selected)
{
    return selected ? t.accent : t.subtext;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      app(Furui::AppController::instance()),
      m_shell(nullptr), m_nav(nullptr), m_tabBar(nullptr), m_stack(nullptr),
      m_searchEdit(nullptr), m_sourceCombo(nullptr), m_searchTypeCombo(nullptr), m_searchTable(nullptr),
      m_rankTable(nullptr), m_myPlaylistTable(nullptr), m_favTable(nullptr), m_historyTable(nullptr),
      m_accountTable(nullptr),
      m_quickQuality(nullptr), m_quickVip(nullptr), m_quickVolume(nullptr),
      m_quickMode(nullptr), m_quickTheme(nullptr),
      m_queuePanel(nullptr), m_queueList(nullptr), m_queueBtn(nullptr),
      m_cover(nullptr), m_barInfo(nullptr), m_barControls(nullptr), m_barMeta(nullptr),
      m_title(nullptr), m_artist(nullptr), m_vipBadge(nullptr),
      m_prevBtn(nullptr), m_playBtn(nullptr), m_nextBtn(nullptr),
      m_progress(nullptr), m_volume(nullptr), m_volumeBtn(nullptr),
      m_modeBtn(nullptr), m_lyricsBtn(nullptr), m_favCurrentBtn(nullptr),
      m_time(nullptr), m_statusLabel(nullptr), m_playBar(nullptr),
      m_lyricsWindow(nullptr),
      m_mode(Wide), m_queueOpen(false)
{
    setWindowTitle(app->appName() + QStringLiteral(" - 桌面版"));
    // 最小尺寸要足够小，否则竖屏/窄窗口根本缩不进去（改造前被顶到 1067px 宽）
    setMinimumSize(360, 480);
    resize(1180, 760);

    QSettings st(QStringLiteral("Furui"), QStringLiteral("FuruiMusic"));
    const QVariant savedSize = st.value(QStringLiteral("window/size"));
    if (savedSize.isValid() && savedSize.toSize().isValid()) resize(savedSize.toSize());
    const QVariant savedPos = st.value(QStringLiteral("window/pos"));
    if (savedPos.isValid()) move(savedPos.toPoint());

    // 主题必须在建页之前应用：Theme::apply 会算出 DPI 缩放系数，
    // 而 createPlayBar 之类的 C++ 固定尺寸直接读 pxScale()，
    // 晚一步就全按 1.0 建好（高分屏上播放条控件偏小、圆角还被 QSS 的半径吃掉）。
    SettingsDialog::applyTheme(app->setting(QStringLiteral("theme"), QStringLiteral("light")).toString());

    m_shell = new QWidget(this);
    QVBoxLayout *outer = vbox(12, 0);
    outer->setContentsMargins(16, 16, 16, 12);
    installLayout(m_shell, outer);

    QHBoxLayout *body = hbox(12, 0);

    m_nav = new QListWidget(m_shell);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setFocusPolicy(Qt::NoFocus);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    for (int i = 0; i < kNavCount; ++i) {
        QListWidgetItem *it = new QListWidgetItem(QString::fromUtf8(kNavEntries[i].label), m_nav);
        it->setData(Qt::UserRole, int(kNavEntries[i].kind));
    }

    m_stack = new QStackedWidget(m_shell);
    m_stack->setFrameShape(QFrame::NoFrame);
    m_stack->addWidget(createSearchPage());
    m_stack->addWidget(createRankPage());
    m_stack->addWidget(createMyPlaylistsPage());
    m_stack->addWidget(createFavoritesPage());
    m_stack->addWidget(createHistoryPage());
    m_stack->addWidget(createAccountPage());
    m_stack->addWidget(createSettingsPage());

    body->addWidget(m_nav);
    body->addWidget(m_stack, 1);
    outer->addLayout(body, 1);

    // 竖屏用的底部标签栏：和 m_nav 指向同一个 stack，切换时互相同步
    m_tabBar = new QListWidget(m_shell);
    m_tabBar->setObjectName(QStringLiteral("tabBar"));
    m_tabBar->setFrameShape(QFrame::NoFrame);
    m_tabBar->setFocusPolicy(Qt::NoFocus);
    m_tabBar->setViewMode(QListView::IconMode);
    m_tabBar->setFlow(QListView::LeftToRight);
    m_tabBar->setMovement(QListView::Static);
    m_tabBar->setResizeMode(QListView::Adjust);
    m_tabBar->setWrapping(false);
    m_tabBar->setSpacing(2);
    m_tabBar->setIconSize(QSize(22, 22));
    m_tabBar->setFixedHeight(60);
    m_tabBar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tabBar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int i = 0; i < kNavCount; ++i) {
        QListWidgetItem *it = new QListWidgetItem(QString(), m_tabBar);
        it->setToolTip(QString::fromUtf8(kNavEntries[i].label));
        it->setData(Qt::UserRole, int(kNavEntries[i].kind));
        it->setTextAlignment(Qt::AlignCenter);
    }
    m_tabBar->hide();
    outer->addWidget(m_tabBar);

    m_playBar = createPlayBar();
    outer->addWidget(m_playBar);
    setCentralWidget(m_shell);

    // 播放队列做成浮层抽屉：不进布局，所以不会把窗口最小宽度顶大
    m_queuePanel = createQueuePanel();
    m_queuePanel->hide();

    auto showPage = [this](int row) {
        if (!app || !m_stack || row < 0 || row >= m_stack->count()) return;
        m_stack->setCurrentIndex(row);
        if (row == 2) app->refreshPlaylists();
        else if (row == 3) app->loadFavorites();
        else if (row == 4) app->loadHistory();
        else if (row == 5) app->refreshAccounts();
        applyLayoutMode();
    };
    connect(m_nav, QOverload<int>::of(&QListWidget::currentRowChanged), this, showPage);
    connect(m_tabBar, QOverload<int>::of(&QListWidget::currentRowChanged), this, [this, showPage](int row) {
        if (m_nav && m_nav->currentRow() != row) {
            QSignalBlocker blocker(m_nav);
            m_nav->setCurrentRow(row);
        }
        showPage(row);
    });
    connect(m_nav, &QListWidget::itemSelectionChanged, this, [this]() {
        if (!m_tabBar || m_nav->currentRow() == m_tabBar->currentRow()) return;
        QSignalBlocker blocker(m_tabBar);
        m_tabBar->setCurrentRow(m_nav->currentRow());
    });

    connectSignals();

    refreshFromResults();
    refreshPlaylistTables();
    refreshAccountsTable();
    refreshQueue();
    updatePlayerInfo();
    updatePlayButton();
    updateSettingsControls();
    if (m_statusLabel) m_statusLabel->setText(app->statusMessage());

    app->refreshPlaylists();
    app->refreshAccounts();
    m_nav->setCurrentRow(0);
    applyLayoutMode();
}

MainWindow::~MainWindow()
{
    delete m_lyricsWindow;
    m_lyricsWindow = nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings st(QStringLiteral("Furui"), QStringLiteral("FuruiMusic"));
    st.setValue(QStringLiteral("window/size"), size());
    st.setValue(QStringLiteral("window/pos"), pos());
    QMainWindow::closeEvent(event);
}

QWidget *MainWindow::createSearchPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);

    m_searchBar = new QWidget(page);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setPlaceholderText(QStringLiteral("输入歌名、歌手或专辑，回车搜索"));
    m_sourceCombo = new QComboBox(m_searchBar);
    m_sourceCombo->addItem(QStringLiteral("全部来源"), QString());
    const QStringList ids = app->sourceIds();
    const QStringList names = app->sourceNames();
    for (int i = 0; i < qMin(ids.size(), names.size()); ++i)
        m_sourceCombo->addItem(names.at(i), ids.at(i));
    m_searchTypeCombo = new QComboBox(m_searchBar);
    m_searchTypeCombo->addItem(QStringLiteral("歌曲"), QStringLiteral("song"));
    m_searchTypeCombo->addItem(QStringLiteral("歌单"), QStringLiteral("songlist"));
    m_searchTypeCombo->setCurrentIndex(app->searchType() == QLatin1String("songlist") ? 1 : 0);
    connect(m_searchTypeCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (app && m_searchTypeCombo)
            app->setSearchType(m_searchTypeCombo->itemData(i).toString());
    });
    m_searchBtn = new QPushButton(QStringLiteral("搜索"), m_searchBar);
    QPushButton *searchBtn = m_searchBtn;
    lay->addWidget(m_searchBar);
    rebuildSearchToolbar();

    m_searchTable = makeSongTable(page);
    lay->addWidget(m_searchTable, 1);
    QLabel *tip = new QLabel(QStringLiteral("双击播放；右键可加入队列、收藏或下载"), page);
    tip->setObjectName(QStringLiteral("hintLabel"));
    lay->addWidget(tip);

    auto doSearch = [this]() {
        if (!app || !m_searchEdit) return;
        const QString keyword = m_searchEdit->text().trimmed();
        if (keyword.isEmpty()) {
            showMessage(QStringLiteral("请输入搜索关键词"));
            return;
        }
        const QString sourceId = m_sourceCombo ? m_sourceCombo->currentData().toString() : QString();
        app->search(keyword, sourceId);
    };
    connect(searchBtn, &QPushButton::clicked, this, doSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, doSearch);

    connect(m_searchTable, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!app || !item || !m_searchTable) return;
                const int row = m_searchTable->indexOfTopLevelItem(item);
                if (row >= 0) app->playResult(row);
            });
    m_searchTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_searchTable, &QWidget::customContextMenuRequested, this, &MainWindow::onSearchContextMenu);
    return page;
}

QWidget *MainWindow::createRankPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);

    QHBoxLayout *btns = new QHBoxLayout();
    const QStringList ids = app->sourceIds();
    const QStringList names = app->sourceNames();
    for (int i = 0; i < qMin(ids.size(), names.size()); ++i) {
        const QString sid = ids.at(i);
        QPushButton *b = new QPushButton(names.at(i), page);
        connect(b, &QPushButton::clicked, this, [this, sid]() {
            if (!app) return;
            app->loadRank(sid);
            showMessage(QStringLiteral("正在加载榜单：") + app->sourceName(sid));
        });
        btns->addWidget(b);
    }
    btns->addStretch(1);
    lay->addLayout(btns);

    m_rankTable = makePlaylistTable(page);
    lay->addWidget(m_rankTable, 1);
    connect(m_rankTable, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!app || !item || !m_rankTable) return;
                const QString uid = item->data(0, Qt::UserRole).toString();
                if (!uid.isEmpty()) app->loadPlaylist(uid);
            });
    return page;
}

QWidget *MainWindow::createMyPlaylistsPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);

    QHBoxLayout *top = new QHBoxLayout();
    QPushButton *refresh = new QPushButton(QStringLiteral("刷新歌单"), page);
    QPushButton *syncAll = new QPushButton(QStringLiteral("同步全部账号歌单"), page);
    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (app) app->refreshPlaylists();
    });
    connect(syncAll, &QPushButton::clicked, this, [this]() {
        if (app) app->syncAllUserLibraries();
    });
    top->addWidget(refresh);
    top->addWidget(syncAll);
    top->addStretch(1);
    lay->addLayout(top);

    m_myPlaylistTable = makePlaylistTable(page);
    lay->addWidget(m_myPlaylistTable, 1);
    connect(m_myPlaylistTable, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!app || !item || !m_myPlaylistTable) return;
                const QString uid = item->data(0, Qt::UserRole).toString();
                if (!uid.isEmpty()) app->loadPlaylist(uid);
            });
    return page;
}

QWidget *MainWindow::createFavoritesPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    QHBoxLayout *top = new QHBoxLayout();
    QPushButton *load = new QPushButton(QStringLiteral("加载我的收藏"), page);
    connect(load, &QPushButton::clicked, this, [this]() {
        if (app) app->loadFavorites();
    });
    top->addWidget(load);
    top->addStretch(1);
    lay->addLayout(top);
    m_favTable = makeSongTable(page);
    lay->addWidget(m_favTable, 1);
    QLabel *favTip = new QLabel(QStringLiteral("双击播放；右键可加入队列、收藏或下载"), page);
    favTip->setObjectName(QStringLiteral("hintLabel"));
    lay->addWidget(favTip);
    // 收藏表与搜索表映射同一份 results，不接双击就只能在这里看，不能听
    connect(m_favTable, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!app || !item || !m_favTable) return;
        const int row = m_favTable->indexOfTopLevelItem(item);
        if (row >= 0) app->playResult(row);
    });
    m_favTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_favTable, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { songContextMenu(m_favTable, pos); });
    return page;
}

QWidget *MainWindow::createHistoryPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    QHBoxLayout *top = new QHBoxLayout();
    QPushButton *load = new QPushButton(QStringLiteral("加载播放历史"), page);
    connect(load, &QPushButton::clicked, this, [this]() {
        if (app) app->loadHistory();
    });
    top->addWidget(load);
    top->addStretch(1);
    lay->addLayout(top);
    m_historyTable = makeSongTable(page);
    lay->addWidget(m_historyTable, 1);
    QLabel *hisTip = new QLabel(QStringLiteral("双击播放；右键可加入队列、收藏或下载"), page);
    hisTip->setObjectName(QStringLiteral("hintLabel"));
    lay->addWidget(hisTip);
    connect(m_historyTable, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!app || !item || !m_historyTable) return;
        const int row = m_historyTable->indexOfTopLevelItem(item);
        if (row >= 0) app->playResult(row);
    });
    m_historyTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_historyTable, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { songContextMenu(m_historyTable, pos); });
    return page;
}

QWidget *MainWindow::createAccountPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);

    QHBoxLayout *top = new QHBoxLayout();
    QPushButton *openLogin = new QPushButton(QStringLiteral("打开登录"), page);
    QPushButton *refresh = new QPushButton(QStringLiteral("刷新账号状态"), page);
    connect(openLogin, &QPushButton::clicked, this, &MainWindow::openLoginDialog);
    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (app) app->refreshAccounts();
    });
    top->addWidget(openLogin);
    top->addWidget(refresh);
    top->addStretch(1);
    lay->addLayout(top);

    m_accountTable = new QTreeWidget(page);
    m_accountTable->setColumnCount(4);
    m_accountTable->setHeaderLabels(QStringList()
                                    << QStringLiteral("平台") << QStringLiteral("昵称")
                                    << QStringLiteral("状态") << QStringLiteral("登录"));
    m_accountTable->setRootIsDecorated(false);
    m_accountTable->setAlternatingRowColors(true);
    m_accountTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(m_accountTable, 1);
    return page;
}

QWidget *MainWindow::createSettingsPage()
{
    QWidget *page = new QWidget(this);
    QFormLayout *form = new QFormLayout(page);

    m_quickQuality = new QComboBox(page);
    m_quickQuality->addItem(QStringLiteral("标准"), QStringLiteral("standard"));
    m_quickQuality->addItem(QStringLiteral("高品"), QStringLiteral("high"));
    m_quickQuality->addItem(QStringLiteral("无损"), QStringLiteral("lossless"));
    m_quickQuality->addItem(QStringLiteral("Hi-Res"), QStringLiteral("hires"));
    form->addRow(QStringLiteral("音质"), m_quickQuality);

    m_quickVip = new Furui::AccentCheckBox(QStringLiteral("VIP 歌曲使用替代音源"), page);
    form->addRow(QString(), m_quickVip);

    m_quickVolume = new Furui::ValueSlider(0, 100, page);
    m_quickVolume->setSuffix(QStringLiteral(" %"));
    form->addRow(QStringLiteral("默认音量"), m_quickVolume);

    m_quickMode = new QComboBox(page);
    m_quickMode->addItem(QStringLiteral("顺序播放"), 0);
    m_quickMode->addItem(QStringLiteral("列表循环"), 1);
    m_quickMode->addItem(QStringLiteral("单曲循环"), 2);
    m_quickMode->addItem(QStringLiteral("随机播放"), 3);
    form->addRow(QStringLiteral("播放模式"), m_quickMode);

    m_quickTheme = new QComboBox(page);
    m_quickTheme->addItem(QStringLiteral("亮色"), QStringLiteral("light"));
    m_quickTheme->addItem(QStringLiteral("暗色"), QStringLiteral("dark"));
    form->addRow(QStringLiteral("主题"), m_quickTheme);

    QPushButton *openSettings = new QPushButton(QStringLiteral("打开设置"), page);
    form->addRow(QString(), openSettings);
    connect(openSettings, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);

    connect(m_quickQuality, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_quickQuality) return;
        app->setSetting(QStringLiteral("quality"), m_quickQuality->itemData(i).toString());
    });
    connect(m_quickVip, &QCheckBox::toggled, this, [this](bool on) {
        if (app) app->setSetting(QStringLiteral("vipSubstitute"), on);
    });
    connect(m_quickVolume, &Furui::ValueSlider::valueChanged, this, [this](int v) {
        if (!app) return;
        app->setSetting(QStringLiteral("defaultVolume"), v);
        app->setVolume(v);
    });
    connect(m_quickMode, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_quickMode) return;
        app->setPlayMode(m_quickMode->itemData(i).toInt());
    });
    connect(m_quickTheme, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_quickTheme) return;
        const QString key = m_quickTheme->itemData(i).toString();
        app->setSetting(QStringLiteral("theme"), key);
        SettingsDialog::applyTheme(key);
    });
    return page;
}

QWidget *MainWindow::createPlayBar()
{
    using namespace Furui;
    const Tokens &t = Theme::current();

    QWidget *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("playBar"));
    // 初始装横屏布局；竖屏切换由 applyLayoutMode() 重排同一批控件
    QVBoxLayout *stub = vbox(8, 12);
    installLayout(bar, stub);

    // ---- 信息组：封面 + 歌名/歌手/角标 ----
    m_barInfo = new QWidget(bar);
    m_cover = new CoverArt(m_barInfo);
    m_cover->setObjectName(QStringLiteral("coverArt"));
    m_cover->setFixedSize(56, 56);
    m_title = new QLabel(QStringLiteral("未在播放"), m_barInfo);
    m_title->setObjectName(QStringLiteral("titleLabel"));
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_artist = new QLabel(QStringLiteral("--"), m_barInfo);
    m_artist->setObjectName(QStringLiteral("subLabel"));
    m_artist->setMinimumWidth(0);
    m_vipBadge = new QLabel(m_barInfo);
    m_vipBadge->setObjectName(QStringLiteral("chip"));
    m_vipBadge->hide();
    {
        QHBoxLayout *l = hbox(10, 0);
        QVBoxLayout *txt = vbox(2, 0);
        l->addWidget(m_cover);
        l->addLayout(txt, 1);
        txt->addWidget(m_title);
        txt->addWidget(m_artist);
        txt->addWidget(m_vipBadge, 0, Qt::AlignLeft);
        installLayout(m_barInfo, l);
    }

    // ---- 控制组：上一首/播放/下一首 + 进度 ----
    m_barControls = new QWidget(bar);
    const qreal k = Furui::Theme::pxScale();
    const int side = 2 * qRound(22 * k);   // 与 QSS 里 px(22) 的圆角严格成两倍关系，才是正圆
    m_prevBtn = makeIconButton(Icons::Prev, QStringLiteral("上一首"), qRound(34 * k), t.subtext,
                               m_barControls, qRound(20 * k));
    m_nextBtn = makeIconButton(Icons::Next, QStringLiteral("下一首"), qRound(34 * k), t.subtext,
                               m_barControls, qRound(20 * k));
    m_playBtn = new QPushButton(m_barControls);
    m_playBtn->setObjectName(QStringLiteral("playBtn"));
    // 播放键要画成正圆：QSS 圆角按 DPI 放大过，盒子边长必须同步放大，否则半径超过半边长就不圆角
    m_playBtn->setFixedSize(side, side);
    m_playBtn->setFocusPolicy(Qt::NoFocus);
    m_playBtn->setCursor(Qt::PointingHandCursor);
    m_playBtn->setProperty("iconKind", int(Icons::Play));
    m_playBtn->setProperty("iconPx", qRound(20 * k));
    m_playBtn->setProperty("onAccent", true);
    m_progress = new SeekSlider(m_barControls);
    m_time = new QLabel(QStringLiteral("0:00/0:00"), m_barControls);
    m_time->setObjectName(QStringLiteral("subLabel"));
    m_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    {
        QVBoxLayout *l = vbox(4, 0);
        QHBoxLayout *ctrls = hbox(10, 0);
        QHBoxLayout *prog = hbox(10, 0);
        ctrls->addStretch(1);
        ctrls->addWidget(m_prevBtn);
        ctrls->addWidget(m_playBtn);
        ctrls->addWidget(m_nextBtn);
        ctrls->addStretch(1);
        prog->addWidget(m_progress, 1);
        prog->addWidget(m_time);
        l->addLayout(ctrls);
        l->addLayout(prog);
        installLayout(m_barControls, l);
    }

    // ---- 功能组：音量/模式/歌词/收藏/队列/状态 ----
    m_barMeta = new QWidget(bar);
    m_volumeBtn = makeIconButton(Icons::Volume, QStringLiteral("音量"), 30, t.subtext, m_barMeta, 18);
    m_volume = new QSlider(Qt::Horizontal, m_barMeta);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(qRound(96 * k));
    m_volume->setFixedHeight(qRound(20 * k));
    m_volume->setCursor(Qt::PointingHandCursor);
    m_modeBtn = makeIconButton(Icons::Repeat, QStringLiteral("播放模式"), 30, t.subtext, m_barMeta, 18);
    m_modeBtn->setProperty("activeWhenChecked", true);
    m_lyricsBtn = makeIconButton(Icons::Lyric, QStringLiteral("歌词"), 30, t.subtext, m_barMeta, 18);
    m_lyricsBtn->setProperty("activeWhenChecked", true);
    m_favCurrentBtn = makeIconButton(Icons::Heart, QStringLiteral("收藏当前歌曲"), 30, t.subtext,
                                     m_barMeta, 18);
    m_favCurrentBtn->setProperty("activeWhenChecked", true);
    m_queueBtn = makeIconButton(Icons::Queue, QStringLiteral("播放队列"), 30, t.subtext, m_barMeta, 18);
    m_queueBtn->setObjectName(QStringLiteral("queueToggle"));
    m_queueBtn->setCheckable(true);
    m_queueBtn->setProperty("activeWhenChecked", true);
    // 状态文字交给底部状态栏，播放条只放控件，窄窗口不会被挤掉
    m_statusLabel = new QLabel(bar);
    m_statusLabel->setObjectName(QStringLiteral("hintLabel"));
    m_statusLabel->hide();
    {
        QHBoxLayout *l = hbox(8, 0);
        QHBoxLayout *vol = hbox(6, 0);
        l->addLayout(vol);
        vol->addWidget(m_volumeBtn);
        vol->addWidget(m_volume);
        l->addWidget(m_modeBtn);
        l->addWidget(m_lyricsBtn);
        l->addWidget(m_favCurrentBtn);
        l->addWidget(m_queueBtn);
        installLayout(m_barMeta, l);
    }

    {
        QHBoxLayout *l = hbox(14, 0);
        l->addWidget(m_barInfo, 0);
        l->addWidget(m_barControls, 1);
        l->addWidget(m_barMeta, 0);
        installLayout(bar, l);
    }

    connect(m_prevBtn, &QPushButton::clicked, this, [this]() { if (app) app->previous(); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this]() { if (app) app->next(); });
    connect(m_playBtn, &QPushButton::clicked, this, [this]() { if (app) app->togglePause(); });
    connect(m_modeBtn, &QPushButton::clicked, this, [this]() {
        if (app) app->cyclePlayMode();
        updateSettingsControls();
    });
    connect(m_lyricsBtn, &QPushButton::clicked, this, &MainWindow::toggleLyrics);
    connect(m_favCurrentBtn, &QPushButton::clicked, this, [this]() {
        if (!app) return;
        app->toggleFavoriteCurrent();
        m_favCurrentBtn->setChecked(app->isFavoriteCurrent());
    });
    connect(m_queueBtn, &QPushButton::clicked, this, [this]() {
        setQueueDrawerVisible(m_queueBtn->isChecked());
    });

    connect(m_progress, &SeekSlider::seekRequested, this, [this](qint64 ms) {
        if (app) app->seek(ms);
    });
    connect(m_progress, &SeekSlider::hoverPreview, this, [this](const QString &text) {
        if (!m_time) return;
        if (text.isEmpty()) updatePosition();
        else m_time->setText(text + QStringLiteral("/") + formatTime(app ? app->duration() : 0));
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        if (app) app->setVolume(v);
        if (m_volumeBtn) {
            m_volumeBtn->setProperty("iconKind", v == 0 ? int(Icons::Mute) : int(Icons::Volume));
            rethemeIcons();
        }
    });

    return bar;
}

QWidget *MainWindow::createQueuePanel()
{
    QWidget *panel = new QWidget(m_shell);
    panel->setObjectName(QStringLiteral("drawer"));
    QVBoxLayout *lay = vbox(10, 14);
    installLayout(panel, lay);

    QHBoxLayout *head = hbox(8, 0);
    QLabel *title = new QLabel(QStringLiteral("播放队列"), panel);
    title->setObjectName(QStringLiteral("drawerTitle"));
    QPushButton *close = makeIconButton(Icons::Close, QStringLiteral("关闭队列"), 28,
                                       Theme::current().subtext, panel, 16);
    connect(close, &QPushButton::clicked, this, [this]() { setQueueDrawerVisible(false); });
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(close);
    lay->addLayout(head);

    m_queueList = new QListWidget(panel);
    m_queueList->setFocusPolicy(Qt::NoFocus);
    m_queueList->setAlternatingRowColors(true);
    lay->addWidget(m_queueList, 1);

    QHBoxLayout *ops = hbox(8, 0);
    QPushButton *remove = new QPushButton(QStringLiteral("移除选中"), panel);
    QPushButton *clear = new QPushButton(QStringLiteral("清空队列"), panel);
    remove->setProperty("role", "pill");
    clear->setProperty("role", "pill");
    connect(remove, &QPushButton::clicked, this, [this]() {
        if (!app || !m_queueList) return;
        const int row = m_queueList->currentRow();
        if (row >= 0) app->removeQueue(row);
    });
    connect(clear, &QPushButton::clicked, this, [this]() { if (app) app->clearQueue(); });
    ops->addWidget(remove);
    ops->addWidget(clear);
    ops->addStretch(1);
    lay->addLayout(ops);

    QLabel *tip = new QLabel(QStringLiteral("双击播放，右键可移除或收藏"), panel);
    tip->setObjectName(QStringLiteral("hintLabel"));
    lay->addWidget(tip);

    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(36);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    panel->setGraphicsEffect(shadow);

    connect(m_queueList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!app || !item || !m_queueList) return;
        const int row = m_queueList->row(item);
        if (row >= 0) app->playQueue(row);
    });
    m_queueList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_queueList, &QWidget::customContextMenuRequested, this, &MainWindow::onQueueContextMenu);
    return panel;
}

void MainWindow::fillSongTable(QTreeWidget *table, const QVector<Furui::Song> &songs)
{
    if (!table || !app) return;
    table->clear();
    for (const Furui::Song &s : songs) {
        QTreeWidgetItem *it = new QTreeWidgetItem(table);
        it->setText(0, s.title);
        it->setText(1, s.artist);
        it->setText(2, s.album);
        it->setText(3, formatTime(s.duration));
        it->setText(4, app->sourceName(s.source));
        it->setText(5, s.vip ? QStringLiteral("VIP") : QString());
    }
    setEmptyState(table, songs.isEmpty() ? QStringLiteral("没有匹配的歌曲，换个关键词试试")
                                         : QStringLiteral("歌曲列表"));
}

void MainWindow::fillPlaylistTable(QTreeWidget *table)
{
    if (!table || !app) return;
    table->clear();
    const QVector<Furui::Playlist> &list = app->playlists()->playlists();
    for (const Furui::Playlist &p : list) {
        QTreeWidgetItem *it = new QTreeWidgetItem(table);
        it->setText(0, p.title);
        it->setText(1, p.owner);
        it->setText(2, QString::number(p.count));
        it->setText(3, p.type);
        it->setText(4, app->sourceName(p.source));
        it->setData(0, Qt::UserRole, p.uid);
    }
    setEmptyState(table, list.isEmpty() ? QStringLiteral("暂无歌单，登录后同步你的歌单")
                                        : QStringLiteral("歌单列表"));
}

void MainWindow::refreshFromResults()
{
    if (!app) return;
    const QVector<Furui::Song> &songs = app->results()->songs();
    fillSongTable(m_searchTable, songs);
    fillSongTable(m_favTable, songs);
    fillSongTable(m_historyTable, songs);
}

void MainWindow::refreshPlaylistTables()
{
    fillPlaylistTable(m_rankTable);
    fillPlaylistTable(m_myPlaylistTable);
}

void MainWindow::refreshAccountsTable()
{
    if (!app || !m_accountTable) return;
    m_accountTable->clear();
    const QVector<Furui::Account> &list = app->accounts()->accounts();
    for (const Furui::Account &a : list) {
        QTreeWidgetItem *it = new QTreeWidgetItem(m_accountTable);
        it->setText(0, app->sourceName(a.source));
        it->setText(1, a.nickname);
        it->setText(2, a.status);
        it->setText(3, a.logged ? QStringLiteral("已登录") : QStringLiteral("未登录"));
    }
}

void MainWindow::refreshQueue()
{
    if (!app || !m_queueList) return;
    m_queueList->clear();
    const QVector<Furui::Song> &songs = app->queue()->songs();
    for (const Furui::Song &s : songs) {
        QListWidgetItem *it = new QListWidgetItem(m_queueList);
        it->setText(s.title + QStringLiteral(" - ") + s.artist);
    }
    EmptyOverlay *overlay = m_queueList->findChild<EmptyOverlay *>();
    if (!overlay) overlay = new EmptyOverlay(m_queueList, QStringLiteral("队列为空"));
    overlay->showFor(m_queueList->count());
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // 窗口映射之后才拿得到真实 DPR，图标按屏幕密度重新光栅化一次
    rethemeIcons();
}

void MainWindow::updatePlayerInfo()
{
    if (!app) return;
    const QString title = app->currentTitle();
    if (m_title) m_title->setText(title.isEmpty() ? QStringLiteral("未在播放") : title);
    if (m_artist) {
        const QString artist = app->currentArtist();
        m_artist->setText(artist.isEmpty() ? QStringLiteral("--") : artist);
    }
    if (m_vipBadge) {
        const QString badge = app->substituteBadge();
        m_vipBadge->setText(badge);
        m_vipBadge->setVisible(!badge.isEmpty());
    }
    if (m_cover) m_cover->setPixmap(QPixmap(app->currentCover()));
    if (m_favCurrentBtn) m_favCurrentBtn->setChecked(app->isFavoriteCurrent());
    if (m_progress) m_progress->setRange(app->position(), app->duration());
    updatePosition();
    updatePlayButton();
}

void MainWindow::updatePlayButton()
{
    if (!app || !m_playBtn) return;
    const bool playing = app->playing();
    m_playBtn->setProperty("iconKind",
                           int(playing ? Icons::Pause : Icons::Play));
    m_playBtn->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
    rethemeIcons();
}

void MainWindow::updatePosition()
{
    if (!app) return;
    if (m_progress) m_progress->setRange(app->position(), app->duration());
    if (m_time)
        m_time->setText(formatTime(app->position()) + QStringLiteral("/")
                        + formatTime(app->duration()));
}

void MainWindow::updateSettingsControls()
{
    if (!app) return;
    if (m_quickQuality) {
        QSignalBlocker blocker(m_quickQuality);
        const int i = m_quickQuality->findData(app->quality());
        m_quickQuality->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_quickVip) {
        QSignalBlocker blocker(m_quickVip);
        m_quickVip->setChecked(app->vipSubstituteEnabled());
    }
    if (m_searchTypeCombo) {
        QSignalBlocker blocker(m_searchTypeCombo);
        m_searchTypeCombo->setCurrentIndex(app->searchType() == QLatin1String("songlist") ? 1 : 0);
    }
    if (m_quickVolume) {
        QSignalBlocker blocker(m_quickVolume);
        m_quickVolume->setValue(app->setting(QStringLiteral("defaultVolume"), app->volume()).toInt());
    }
    if (m_quickMode) {
        QSignalBlocker blocker(m_quickMode);
        const int i = m_quickMode->findData(app->playMode());
        m_quickMode->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_quickTheme) {
        QSignalBlocker blocker(m_quickTheme);
        const int i = m_quickTheme->findData(app->setting(QStringLiteral("theme"), QStringLiteral("light")).toString());
        m_quickTheme->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_volume) {
        QSignalBlocker blocker(m_volume);
        m_volume->setValue(app->volume());
    }
    if (m_modeBtn) {
        const int mode = app->playMode();
        const int kind = mode == 1 ? int(Icons::RepeatOne)
                                   : (mode == 3 ? int(Icons::Shuffle) : int(Icons::Repeat));
        m_modeBtn->setProperty("iconKind", kind);
        m_modeBtn->setToolTip(app->playModeName());
        m_modeBtn->setChecked(mode != 0);
        rethemeIcons();
    }
}

void MainWindow::showMessage(const QString &text)
{
    if (m_statusLabel) m_statusLabel->setText(text);
    statusBar()->showMessage(text, 4000);
}

void MainWindow::onSearchContextMenu(const QPoint &pos)
{
    songContextMenu(m_searchTable, pos);
}

// 搜索/收藏/历史三张表都直接映射 app->results() 的行号，右键菜单共用一套动作
void MainWindow::songContextMenu(QTreeWidget *table, const QPoint &pos)
{
    if (!app || !table) return;
    QTreeWidgetItem *item = table->itemAt(pos);
    if (!item) return;
    const int row = table->indexOfTopLevelItem(item);

    QMenu menu(table);
    QAction *aPlay = menu.addAction(QStringLiteral("播放"));
    QAction *aQueue = menu.addAction(QStringLiteral("加入播放队列"));
    QAction *aFav = menu.addAction(QStringLiteral("收藏/取消收藏"));
    QAction *aDown = menu.addAction(QStringLiteral("下载"));
    QAction *chosen = menu.exec(table->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == aPlay) app->playResult(row);
    else if (chosen == aQueue) app->addResultToQueue(row);
    else if (chosen == aFav) app->toggleFavoriteResult(row);
    else if (chosen == aDown) app->downloadResult(row);
}

void MainWindow::onQueueContextMenu(const QPoint &pos)
{
    if (!app || !m_queueList) return;
    QListWidgetItem *item = m_queueList->itemAt(pos);
    if (!item) return;
    const int row = m_queueList->row(item);

    QMenu menu(m_queueList);
    QAction *aPlay = menu.addAction(QStringLiteral("播放"));
    QAction *aFav = menu.addAction(QStringLiteral("收藏/取消收藏"));
    QAction *aDel = menu.addAction(QStringLiteral("从队列移除"));
    QAction *chosen = menu.exec(m_queueList->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == aPlay) app->playQueue(row);
    else if (chosen == aFav) app->toggleFavoriteQueue(row);
    else if (chosen == aDel) app->removeQueue(row);
}

void MainWindow::toggleLyrics()
{
    if (!m_lyricsWindow) m_lyricsWindow = new LyricsWindow(nullptr);
    if (m_lyricsWindow->isVisible()) {
        m_lyricsWindow->hide();
    } else {
        m_lyricsWindow->show();
        m_lyricsWindow->raise();
    }
}

void MainWindow::openLoginDialog()
{
    LoginDialog dlg(this);
    dlg.exec();
}

void MainWindow::openSettingsDialog()
{
    SettingsDialog dlg(this);
    dlg.exec();
}


void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    applyLayoutMode();
}

void MainWindow::applyLayoutMode()
{
    if (!m_shell || !m_nav || !m_stack) return;
    const Tokens &t = Theme::current();
    const int w = m_shell->width();
    const LayoutMode mode = w >= 1080 ? Wide : (w >= 780 ? Medium : Portrait);
    const bool portrait = mode == Portrait;

    // 侧栏：宽屏图标+文字，中屏纯图标，竖屏交给底部标签栏
    m_nav->setVisible(!portrait);
    m_tabBar->setVisible(portrait);
    const int navWidth = portrait ? 0 : (mode == Wide ? 190 : 62);
    if (!portrait && m_nav->width() != navWidth) m_nav->setFixedWidth(navWidth);

    const int row = m_nav->currentRow();
    const int iconPx = mode == Wide ? 20 : 22;
    for (int i = 0; i < m_nav->count(); ++i) {
        QListWidgetItem *it = m_nav->item(i);
        const Icons::Kind k = Icons::Kind(it->data(Qt::UserRole).toInt());
        it->setIcon(QIcon(Icons::pixmap(k, navIconColor(t, i == row), iconPx,
                                       m_nav->devicePixelRatioF())));
        it->setText(mode == Wide ? QString::fromUtf8(kNavEntries[i].label) : QString());
        it->setToolTip(QString::fromUtf8(kNavEntries[i].label));
        it->setSizeHint(QSize(navWidth - 12, iconPx + 26));
    }
    for (int i = 0; i < m_tabBar->count(); ++i) {
        QListWidgetItem *it = m_tabBar->item(i);
        const Icons::Kind k = Icons::Kind(it->data(Qt::UserRole).toInt());
        it->setIcon(QIcon(Icons::pixmap(k, navIconColor(t, i == row), 22,
                                        m_tabBar->devicePixelRatioF())));
        it->setSizeHint(QSize(qMax(48, (m_tabBar->width() - 16) / m_tabBar->count()), 52));
    }

    if (mode != m_mode) {
        m_mode = mode;
        rebuildPlayBar();
        rebuildSearchToolbar();
        tuneTableColumns();

        // 竖屏时底部标签栏是主导航，播放条要排在它上面；横屏标签栏整体隐藏
        QBoxLayout *outer = qobject_cast<QBoxLayout *>(m_shell->layout());
        if (outer) {
            const int idx = outer->indexOf(m_tabBar);
            const int want = portrait ? outer->count() - 1 : 1;
            if (idx >= 0 && idx != want) outer->insertItem(want, outer->takeAt(idx));
        }
    }

    if (m_queuePanel && m_queuePanel->isVisible()) {
        const QRect area = m_stack->geometry();
        const int pw = portrait ? area.width() : qMin(340, int(area.width() * 0.46));
        m_queuePanel->setGeometry(area.right() - pw + 1, area.top(), pw, area.height());
        m_queuePanel->raise();
    }
}

void MainWindow::rebuildSearchToolbar()
{
    if (!m_searchBar || !m_searchEdit) return;

    if (m_mode == Portrait) {
        // 竖屏宽度不够放一行四件套，输入框独占一行才看得清关键词
        QHBoxLayout *row1 = hbox(8, 0);
        QHBoxLayout *row2 = hbox(8, 0);
        row1->addWidget(m_searchEdit, 1);
        row2->addWidget(m_sourceCombo);
        row2->addWidget(m_searchTypeCombo);
        row2->addWidget(m_searchBtn, 1);
        QVBoxLayout *v = vbox(8, 0);
        v->addLayout(row1);
        v->addLayout(row2);
        installLayout(m_searchBar, v);
        return;
    }

    QHBoxLayout *row = hbox(8, 0);
    row->addWidget(m_searchEdit, 1);
    row->addWidget(m_sourceCombo);
    row->addWidget(m_searchTypeCombo);
    row->addWidget(m_searchBtn);
    installLayout(m_searchBar, row);
}

void MainWindow::rebuildPlayBar()
{
    const bool portrait = m_mode == Portrait;
    const int coverSide = portrait ? qBound(72, m_shell->width() - 120, 168) : 56;

    m_cover->setFixedSize(coverSide, coverSide);

    if (portrait) {
        QVBoxLayout *info = vbox(6, 0);
        info->setAlignment(Qt::AlignHCenter);
        info->addWidget(m_cover, 0, Qt::AlignHCenter);
        m_title->setAlignment(Qt::AlignHCenter);
        m_artist->setAlignment(Qt::AlignHCenter);
        info->addWidget(m_title);
        info->addWidget(m_artist);
        info->addWidget(m_vipBadge, 0, Qt::AlignHCenter);
        installLayout(m_barInfo, info);

        QVBoxLayout *ctrls = vbox(6, 0);
        QHBoxLayout *buttons = hbox(16, 0);
        buttons->addStretch(1);
        buttons->addWidget(m_prevBtn);
        buttons->addWidget(m_playBtn);
        buttons->addWidget(m_nextBtn);
        buttons->addStretch(1);
        ctrls->addLayout(buttons);
        ctrls->addWidget(m_progress);
        m_time->setAlignment(Qt::AlignCenter);
        ctrls->addWidget(m_time);
        installLayout(m_barControls, ctrls);

        QHBoxLayout *meta = hbox(6, 0);
        meta->addStretch(1);
        meta->addWidget(m_volumeBtn);
        meta->addWidget(m_volume);
        meta->addSpacing(6);
        meta->addWidget(m_modeBtn);
        meta->addWidget(m_lyricsBtn);
        meta->addWidget(m_favCurrentBtn);
        meta->addWidget(m_queueBtn);
        meta->addStretch(1);
        installLayout(m_barMeta, meta);

        QVBoxLayout *bar = vbox(10, 12);
        bar->addWidget(m_barInfo, 0, Qt::AlignHCenter);
        bar->addWidget(m_barControls, 1);
        bar->addWidget(m_barMeta);
        installLayout(m_playBar, bar);
        if (m_statusLabel) m_statusLabel->hide();
    } else {
        QHBoxLayout *info = hbox(10, 0);
        QVBoxLayout *txt = vbox(2, 0);
        m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_artist->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        info->addWidget(m_cover);
        info->addLayout(txt, 1);
        txt->addWidget(m_title);
        txt->addWidget(m_artist);
        txt->addWidget(m_vipBadge, 0, Qt::AlignLeft);
        installLayout(m_barInfo, info);

        QVBoxLayout *ctrls = vbox(4, 0);
        QHBoxLayout *buttons = hbox(10, 0);
        QHBoxLayout *seek = hbox(10, 0);
        buttons->addStretch(1);
        buttons->addWidget(m_prevBtn);
        buttons->addWidget(m_playBtn);
        buttons->addWidget(m_nextBtn);
        buttons->addStretch(1);
        seek->addWidget(m_progress, 1);
        m_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        seek->addWidget(m_time);
        ctrls->addLayout(buttons);
        ctrls->addLayout(seek);
        installLayout(m_barControls, ctrls);

        QHBoxLayout *meta = hbox(6, 0);
        QHBoxLayout *vol = hbox(4, 0);
        vol->addWidget(m_volumeBtn);
        vol->addWidget(m_volume);
        meta->addLayout(vol);
        meta->addWidget(m_modeBtn);
        meta->addWidget(m_lyricsBtn);
        meta->addWidget(m_favCurrentBtn);
        meta->addWidget(m_queueBtn);
        meta->addWidget(m_statusLabel);
        installLayout(m_barMeta, meta);

        QHBoxLayout *bar = hbox(14, 0);
        bar->setAlignment(Qt::AlignVCenter);
        bar->addWidget(m_barInfo, 0);
        bar->addWidget(m_barControls, 1);
        bar->addWidget(m_barMeta, 0);
        installLayout(m_playBar, bar);
    }
    // 横屏固定最小高度，否则窄窗口下布局会把歌手那一行裁掉半截
    m_playBar->setMinimumHeight(portrait ? 0 : 84);
    m_playBar->adjustSize();
}

void MainWindow::tuneTableColumns()
{
    // 窄窗口收起次要列，避免横向滚动条把界面挤花
    const int songHideFrom = m_mode == Portrait ? 4 : (m_mode == Medium ? 4 : 6);
    QTreeWidget *songTables[] = {m_searchTable, m_favTable, m_historyTable};
    for (QTreeWidget *t : songTables) {
        if (!t) continue;
        for (int col = 0; col < t->columnCount(); ++col) {
            const bool album = col == 2;
            t->setColumnHidden(col, album ? (m_mode != Wide) : (col >= songHideFrom));
        }
        t->setColumnWidth(0, m_mode == Portrait ? 170 : 250);
    }
    if (m_accountTable) {
        // 窄屏下"状态"与"登录"内容重复，留一列就够
        m_accountTable->setColumnHidden(2, m_mode == Portrait);
        m_accountTable->setColumnWidth(0, m_mode == Portrait ? 120 : 160);
    }
    QTreeWidget *listTables[] = {m_rankTable, m_myPlaylistTable};
    for (QTreeWidget *t : listTables) {
        if (!t) continue;
        for (int col = 0; col < t->columnCount(); ++col) {
            const bool keep = (col == 0 || col == 1)
                || (m_mode == Portrait ? col == 2 : (col == 2 || col == 4));
            t->setColumnHidden(col, !keep);
        }
        t->setColumnWidth(0, m_mode == Portrait ? 150 : 250);
    }
}

void MainWindow::setQueueDrawerVisible(bool visible)
{
    if (!m_queuePanel) return;
    m_queueOpen = visible;
    m_queuePanel->setVisible(visible);
    if (m_queueBtn) m_queueBtn->setChecked(visible);
    if (visible) {
        applyLayoutMode();
        m_queuePanel->raise();
    }
}

void MainWindow::rethemeIcons()
{
    const Tokens &t = Theme::current();
    const QList<QPushButton *> buttons = findChildren<QPushButton *>();
    for (QPushButton *b : buttons) {
        const QVariant kindVar = b->property("iconKind");
        if (!kindVar.isValid()) continue;
        const int px = b->property("iconPx").toInt();
        if (px <= 0) continue;
        QColor color = t.subtext;
        if (b->property("onAccent").toBool()) color = t.accentText;
        else if (b->property("activeWhenChecked").toBool() && b->isChecked()) color = t.accent;
        b->setIcon(QIcon(Icons::pixmap(Icons::Kind(kindVar.toInt()), color, px,
                                       b->devicePixelRatioF())));
    }
    // 自绘控件在 paintEvent 里取当前令牌，主题换了只要重绘
    const QList<QWidget *> repaints = findChildren<QWidget *>();
    for (QWidget *w : repaints) {
        if (qobject_cast<Furui::CoverArt *>(w) || qobject_cast<Furui::SeekSlider *>(w))
            w->update();
    }
    if (m_nav) applyLayoutMode();
}

void MainWindow::connectSignals()
{
    if (!app) return;
    connect(app, &Furui::AppController::toast, this, [this](const QString &m) { showMessage(m); });
    connect(app, &Furui::AppController::statusChanged, this, [this]() {
        if (app && m_statusLabel) m_statusLabel->setText(app->statusMessage());
    });
    connect(app, &Furui::AppController::playerChanged, this, [this]() { updatePlayerInfo(); });
    connect(app, &Furui::AppController::playingChanged, this, [this]() { updatePlayButton(); });
    connect(app, &Furui::AppController::positionChanged, this, [this]() { updatePosition(); });
    connect(app, &Furui::AppController::settingsChanged, this, [this]() { updateSettingsControls(); });
    connect(app, &Furui::AppController::searchStarted, this, [this]() {
        showMessage(QStringLiteral("搜索中…"));
    });
    connect(app, &Furui::AppController::searchFinished, this, [this](int count) {
        if (app->searchType() == QLatin1String("songlist")) {
            refreshPlaylistTables();
            m_nav->setCurrentRow(2);
            showMessage(QStringLiteral("歌单搜索完成，共 %1 个歌单").arg(count));
        } else {
            refreshFromResults();
            showMessage(QStringLiteral("搜索完成，共 %1 首").arg(count));
        }
    });
    connect(app, &Furui::AppController::playlistsLoaded, this, [this]() { refreshPlaylistTables(); });
    connect(app, &Furui::AppController::loginStateChanged, this,
            [this](const QString &, bool, const QString &) { refreshAccountsTable(); });
    connect(app, &Furui::AppController::substituteInfo, this, [this](const QString &t) { showMessage(t); });
    connect(app, &Furui::AppController::downloadFinished, this, [this](const QString &p) {
        showMessage(QStringLiteral("下载完成：") + p);
    });

    // 模型信号兜底：任何数据更新都刷新对应视图。
    connect(app->results(), &QAbstractItemModel::modelReset, this, [this]() { refreshFromResults(); });
    connect(app->results(), &QAbstractItemModel::rowsInserted, this, [this]() { refreshFromResults(); });
    connect(app->results(), &QAbstractItemModel::dataChanged, this, [this]() { refreshFromResults(); });
    connect(app->queue(), &QAbstractItemModel::modelReset, this, [this]() { refreshQueue(); });
    connect(app->queue(), &QAbstractItemModel::rowsInserted, this, [this]() { refreshQueue(); });
    connect(app->queue(), &QAbstractItemModel::dataChanged, this, [this]() { refreshQueue(); });
    connect(app->playlists(), &QAbstractItemModel::modelReset, this, [this]() { refreshPlaylistTables(); });
    connect(app->accounts(), &QAbstractItemModel::modelReset, this, [this]() { refreshAccountsTable(); });
}

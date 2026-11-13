#include "SettingsDialog.h"
#include "Theme.h"
#include "PlayerWidgets.h"

#include <QApplication>
#include <QtGlobal>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QScrollArea>

namespace {

// 表单比窗口高时布局会压缩到重叠（网络页在 520 高就切掉了提示文字），
// 所以每页套一层滚动容器，小窗口也能滚到最后一项。
QWidget *wrapScroll(QWidget *page)
{
    QScrollArea *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(page);
    return area;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent),
      app(Furui::AppController::instance()),
      m_quality(nullptr), m_vip(nullptr), m_autoToggle(nullptr),
      m_openApi(nullptr), m_openApiPort(nullptr), m_openApiAddress(nullptr), m_searchType(nullptr),
      m_defaultVolume(nullptr), m_playMode(nullptr),
      m_sourcesTable(nullptr), m_httpName(nullptr), m_httpUrl(nullptr), m_localDir(nullptr),
      m_downloadDir(nullptr), m_proxy(nullptr), m_timeout(nullptr),
      m_accountApiBase(nullptr), m_theme(nullptr)
{
    setWindowTitle(QStringLiteral("设置"));
    resize(560, 460);

    QVBoxLayout *root = new QVBoxLayout(this);
    QTabWidget *tabs = new QTabWidget(this);
    tabs->addTab(wrapScroll(createPlaybackTab()), QStringLiteral("播放"));
    tabs->addTab(wrapScroll(createSourcesTab()), QStringLiteral("自定义音源"));
    tabs->addTab(wrapScroll(createNetworkTab()), QStringLiteral("网络"));
    tabs->addTab(wrapScroll(createAppearanceTab()), QStringLiteral("外观"));
    tabs->addTab(wrapScroll(createAboutTab()), QStringLiteral("关于"));
    root->addWidget(tabs, 1);

    QHBoxLayout *bottom = new QHBoxLayout();
    bottom->addStretch(1);
    QPushButton *close = new QPushButton(QStringLiteral("关闭"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    root->addLayout(bottom);

    connectSignals();
    syncFromSettings();
    refreshSources();
    applyTheme(app->setting(QStringLiteral("theme"), QStringLiteral("light")).toString());
}

QWidget *SettingsDialog::createPlaybackTab()
{
    QWidget *w = new QWidget(this);
    QFormLayout *form = new QFormLayout(w);

    m_quality = new QComboBox(w);
    m_quality->addItem(QStringLiteral("标准"), QStringLiteral("standard"));
    m_quality->addItem(QStringLiteral("高品"), QStringLiteral("high"));
    m_quality->addItem(QStringLiteral("无损"), QStringLiteral("lossless"));
    m_quality->addItem(QStringLiteral("Hi-Res"), QStringLiteral("hires"));
    form->addRow(QStringLiteral("音质"), m_quality);

    m_vip = new Furui::AccentCheckBox(QStringLiteral("VIP 歌曲使用自定义音源替代"), w);
    form->addRow(QString(), m_vip);

    m_autoToggle = new Furui::AccentCheckBox(QStringLiteral("自定义音源失败时自动跨源换源"), w);
    form->addRow(QString(), m_autoToggle);

    m_searchType = new QComboBox(w);
    m_searchType->addItem(QStringLiteral("歌曲"), QStringLiteral("song"));
    m_searchType->addItem(QStringLiteral("歌单"), QStringLiteral("songlist"));
    form->addRow(QStringLiteral("默认搜索类型"), m_searchType);

    m_defaultVolume = new Furui::ValueSlider(0, 100, w);
    m_defaultVolume->setSuffix(QStringLiteral(" %"));
    form->addRow(QStringLiteral("默认音量"), m_defaultVolume);

    m_playMode = new QComboBox(w);
    m_playMode->addItem(QStringLiteral("顺序播放"), 0);
    m_playMode->addItem(QStringLiteral("列表循环"), 1);
    m_playMode->addItem(QStringLiteral("单曲循环"), 2);
    m_playMode->addItem(QStringLiteral("随机播放"), 3);
    form->addRow(QStringLiteral("播放模式"), m_playMode);

    connect(m_quality, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_quality) return;
        app->setSetting(QStringLiteral("quality"), m_quality->itemData(i).toString());
    });
    connect(m_vip, &QCheckBox::toggled, this, [this](bool on) {
        if (app) app->setSetting(QStringLiteral("vipSubstitute"), on);
    });
    connect(m_autoToggle, &QCheckBox::toggled, this, [this](bool on) {
        if (app) app->setSetting(QStringLiteral("autoToggleSource"), on);
    });
    connect(m_searchType, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (app && m_searchType) app->setSearchType(m_searchType->itemData(i).toString());
    });
    connect(m_defaultVolume, &Furui::ValueSlider::valueChanged, this, [this](int v) {
        if (!app) return;
        app->setSetting(QStringLiteral("defaultVolume"), v);
        app->setVolume(v);
    });
    connect(m_playMode, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_playMode) return;
        app->setPlayMode(m_playMode->itemData(i).toInt());
    });
    return w;
}

QWidget *SettingsDialog::createSourcesTab()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(w);

    m_sourcesTable = new QTableWidget(0, 2, w);
    m_sourcesTable->setHorizontalHeaderLabels(QStringList()
                                              << QStringLiteral("ID") << QStringLiteral("名称"));
    m_sourcesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sourcesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sourcesTable->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_sourcesTable, 1);

    QHBoxLayout *add = new QHBoxLayout();
    m_httpName = new QLineEdit(w);
    m_httpName->setPlaceholderText(QStringLiteral("音源名称"));
    m_httpUrl = new QLineEdit(w);
    m_httpUrl->setPlaceholderText(QStringLiteral("HTTP 接口地址 baseUrl"));
    QPushButton *addBtn = new QPushButton(QStringLiteral("添加 HTTP 音源"), w);
    add->addWidget(m_httpName);
    add->addWidget(m_httpUrl, 1);
    add->addWidget(addBtn);
    lay->addLayout(add);
    connect(addBtn, &QPushButton::clicked, this, [this]() {
        if (!app || !m_httpName || !m_httpUrl) return;
        const QString name = m_httpName->text().trimmed();
        const QString url = m_httpUrl->text().trimmed();
        if (name.isEmpty() || url.isEmpty()) return;
        app->addCustomHttpSource(name, url);
        m_httpName->clear();
        m_httpUrl->clear();
    });

    QHBoxLayout *local = new QHBoxLayout();
    QPushButton *pick = new QPushButton(QStringLiteral("选择本地音源目录"), w);
    m_localDir = new QLabel(QStringLiteral("未设置"), w);
    QPushButton *refresh = new QPushButton(QStringLiteral("刷新"), w);
    local->addWidget(pick);
    local->addWidget(m_localDir, 1);
    local->addWidget(refresh);
    lay->addLayout(local);
    connect(pick, &QPushButton::clicked, this, [this]() {
        if (!app) return;
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择本地音源目录"));
        if (!dir.isEmpty()) app->setCustomLocalDirectory(dir);
    });
    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (app) app->refreshCustomSources();
    });

    QHBoxLayout *del = new QHBoxLayout();
    del->addStretch(1);
    QPushButton *delBtn = new QPushButton(QStringLiteral("删除选中音源"), w);
    del->addWidget(delBtn);
    lay->addLayout(del);
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        if (!app || !m_sourcesTable) return;
        const int row = m_sourcesTable->currentRow();
        if (row < 0) return;
        QTableWidgetItem *idItem = m_sourcesTable->item(row, 0);
        if (idItem) app->removeCustomSource(idItem->text());
    });
    return w;
}

QWidget *SettingsDialog::createNetworkTab()
{
    QWidget *w = new QWidget(this);
    QFormLayout *form = new QFormLayout(w);

    QHBoxLayout *dirLay = new QHBoxLayout();
    m_downloadDir = new QLineEdit(w);
    QPushButton *browse = new QPushButton(QStringLiteral("浏览"), w);
    dirLay->addWidget(m_downloadDir, 1);
    dirLay->addWidget(browse);
    form->addRow(QStringLiteral("下载目录"), dirLay);
    connect(browse, &QPushButton::clicked, this, [this]() {
        if (!m_downloadDir) return;
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择下载目录"));
        if (!dir.isEmpty()) m_downloadDir->setText(dir);
    });

    m_proxy = new QLineEdit(w);
    m_proxy->setPlaceholderText(QStringLiteral("例如 http://127.0.0.1:8080"));
    form->addRow(QStringLiteral("HTTP 代理"), m_proxy);

    m_timeout = new QSpinBox(w);
    m_timeout->setRange(1, 120);
    m_timeout->setSuffix(QStringLiteral(" 秒"));
    m_timeout->setButtonSymbols(QAbstractSpinBox::NoButtons);
    form->addRow(QStringLiteral("网络超时"), m_timeout);

    m_accountApiBase = new QLineEdit(w);
    m_accountApiBase->setPlaceholderText(QStringLiteral("例如 http://127.0.0.1:9764"));
    form->addRow(QStringLiteral("账号 API 服务"), m_accountApiBase);
    QLabel *accountTip = new QLabel(QStringLiteral(
        "QQ 音乐 / 酷狗 / 酷我 / 咪咕 的用户歌单与收藏需要自建合规账号 API 服务；"
        "填写地址后按统一契约调用（示例实现 src/tools/account_api_server.py）。"
        "网易云为内置真实实现，无需此项。"), w);
    accountTip->setWordWrap(true);
    form->addRow(QString(), accountTip);

    m_openApi = new Furui::AccentCheckBox(QStringLiteral("启用开放 API 服务（本机 HTTP）"), w);
    form->addRow(QString(), m_openApi);
    m_openApiPort = new QSpinBox(w);
    m_openApiPort->setRange(1024, 65535);
    m_openApiPort->setButtonSymbols(QAbstractSpinBox::NoButtons);
    form->addRow(QStringLiteral("开放 API 端口"), m_openApiPort);
    m_openApiAddress = new QLabel(QStringLiteral("未启动"), w);
    form->addRow(QStringLiteral("服务地址"), m_openApiAddress);
    connect(m_openApi, &QCheckBox::toggled, this, [this](bool on) {
        if (app) app->setSetting(QStringLiteral("openApi"), on);
        if (m_openApiAddress && app)
            m_openApiAddress->setText(app->openApiAddress().isEmpty()
                                          ? QStringLiteral("未启动") : app->openApiAddress());
    });
    connect(m_openApiPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (app) app->setSetting(QStringLiteral("openApiPort"), v);
    });

    QPushButton *save = new QPushButton(QStringLiteral("保存网络设置"), w);
    form->addRow(QString(), save);
    connect(save, &QPushButton::clicked, this, [this]() {
        if (!app || !m_downloadDir || !m_proxy || !m_timeout) return;
        app->setSetting(QStringLiteral("downloadDir"), m_downloadDir->text());
        app->setSetting(QStringLiteral("proxy"), m_proxy->text());
        app->setSetting(QStringLiteral("timeout"), m_timeout->value());
        if (m_accountApiBase)
            app->setSetting(QStringLiteral("accountApiBase"), m_accountApiBase->text().trimmed());
    });
    return w;
}

QWidget *SettingsDialog::createAppearanceTab()
{
    QWidget *w = new QWidget(this);
    QFormLayout *form = new QFormLayout(w);
    m_theme = new QComboBox(w);
    m_theme->addItem(QStringLiteral("亮色"), QStringLiteral("light"));
    m_theme->addItem(QStringLiteral("暗色"), QStringLiteral("dark"));
    form->addRow(QStringLiteral("主题"), m_theme);
    QLabel *tip = new QLabel(QStringLiteral("切换后立即生效"), w);
    form->addRow(QString(), tip);
    connect(m_theme, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!app || !m_theme) return;
        const QString key = m_theme->itemData(i).toString();
        app->setSetting(QStringLiteral("theme"), key);
        applyTheme(key);
    });
    return w;
}

QWidget *SettingsDialog::createAboutTab()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(w);
    QLabel *name = new QLabel(app->appName() + QStringLiteral("  ") + app->version(), w);
    name->setAlignment(Qt::AlignCenter);
    lay->addWidget(name);
    QLabel *desc = new QLabel(QStringLiteral("仅用于学习研究；自定义音源请使用你拥有合法授权的内容。"), w);
    desc->setWordWrap(true);
    desc->setAlignment(Qt::AlignCenter);
    lay->addWidget(desc);
    lay->addStretch(1);
    return w;
}

void SettingsDialog::refreshSources()
{
    if (!app || !m_sourcesTable) return;
    const QStringList ids = app->customSourceIds();
    const QStringList names = app->customSourceNames();
    m_sourcesTable->setRowCount(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        m_sourcesTable->setItem(i, 0, new QTableWidgetItem(ids.at(i)));
        m_sourcesTable->setItem(i, 1, new QTableWidgetItem(i < names.size() ? names.at(i) : QString()));
    }
    if (m_localDir) {
        const QString dir = app->customLocalDirectory();
        m_localDir->setText(dir.isEmpty() ? QStringLiteral("未设置") : dir);
    }
}

void SettingsDialog::syncFromSettings()
{
    if (!app) return;
    if (m_quality) {
        QSignalBlocker blocker(m_quality);
        const int i = m_quality->findData(app->quality());
        m_quality->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_vip) {
        QSignalBlocker blocker(m_vip);
        m_vip->setChecked(app->vipSubstituteEnabled());
    }
    if (m_autoToggle) {
        QSignalBlocker blocker(m_autoToggle);
        m_autoToggle->setChecked(app->autoToggleSource());
    }
    if (m_searchType) {
        QSignalBlocker blocker(m_searchType);
        const int i = m_searchType->findData(app->searchType());
        m_searchType->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_openApi) {
        QSignalBlocker blocker(m_openApi);
        m_openApi->setChecked(app->openApiEnabled());
    }
    if (m_openApiPort) {
        QSignalBlocker blocker(m_openApiPort);
        m_openApiPort->setValue(app->setting(QStringLiteral("openApiPort"), 23330).toInt());
    }
    if (m_openApiAddress)
        m_openApiAddress->setText(app->openApiAddress().isEmpty()
                                      ? QStringLiteral("未启动") : app->openApiAddress());
    if (m_defaultVolume) {
        QSignalBlocker blocker(m_defaultVolume);
        m_defaultVolume->setValue(app->setting(QStringLiteral("defaultVolume"), app->volume()).toInt());
    }
    if (m_playMode) {
        QSignalBlocker blocker(m_playMode);
        const int i = m_playMode->findData(app->playMode());
        m_playMode->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (m_downloadDir)
        m_downloadDir->setText(app->setting(QStringLiteral("downloadDir"), QString()).toString());
    if (m_proxy)
        m_proxy->setText(app->setting(QStringLiteral("proxy"), QString()).toString());
    if (m_timeout) {
        QSignalBlocker blocker(m_timeout);
        m_timeout->setValue(app->setting(QStringLiteral("timeout"), 15).toInt());
    }
    if (m_accountApiBase) {
        QSignalBlocker blocker(m_accountApiBase);
        m_accountApiBase->setText(app->accountApiBase());
    }
    if (m_theme) {
        QSignalBlocker blocker(m_theme);
        const int i = m_theme->findData(app->setting(QStringLiteral("theme"), QStringLiteral("light")).toString());
        m_theme->setCurrentIndex(i >= 0 ? i : 0);
    }
}

void SettingsDialog::connectSignals()
{
    if (!app) return;
    connect(app, &Furui::AppController::customSourcesChanged, this, [this]() { refreshSources(); });
    connect(app, &Furui::AppController::settingsChanged, this, [this]() {
        syncFromSettings();
        refreshSources();
    });
}

void SettingsDialog::applyTheme(const QString &key)
{
    Furui::Theme::apply(key);
}

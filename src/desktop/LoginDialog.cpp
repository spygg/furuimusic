#include "LoginDialog.h"

#include "Theme.h"

#include <QTabWidget>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPair>
#include <QTabBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QAbstractItemModel>
#include <QtGlobal>

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent),
      app(Furui::AppController::instance()),
      m_tabs(nullptr),
      m_hint(nullptr)
{
    setWindowTitle(QStringLiteral("账号登录"));
    resize(540, 440);

    QVBoxLayout *root = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setUsesScrollButtons(false);
    m_tabs->tabBar()->setExpanding(true);
    const QStringList ids = app->sourceIds();
    const QStringList names = app->sourceNames();
    for (int i = 0; i < qMin(ids.size(), names.size()); ++i) {
        const QString sid = ids.at(i);
        const QStringList methods = app->loginMethods(sid);
        QString label = names.at(i);
        // 页签一行要放五个平台，"某某音乐"统一收成"某某"
        if (label.endsWith(QStringLiteral("音乐")))
            label.chop(2);
        m_tabs->addTab(createTab(sid, methods), label);
    }
    root->addWidget(m_tabs, 1);

    m_hint = new QLabel(QStringLiteral("提示：登录相关消息会显示在这里"), this);
    m_hint->setObjectName(QStringLiteral("hintLabel"));
    m_hint->setWordWrap(true);
    root->addWidget(m_hint);

    QHBoxLayout *bottom = new QHBoxLayout();
    bottom->addStretch(1);
    QPushButton *close = new QPushButton(QStringLiteral("关闭"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    root->addLayout(bottom);

    connectSignals();
    app->refreshAccounts();
    refreshStatus();
    refreshApiStatus();
    refreshQr();
    // 打开登录页、切平台页签都直接出码：用户进来就是为了扫码，不该先让他按一次按钮
    connect(m_tabs, &QTabWidget::currentChanged, this, &LoginDialog::showQrTab);
    showQrTab(m_tabs->currentIndex());
}

QWidget *LoginDialog::createTab(const QString &sourceId, const QStringList &methods)
{
    // 登录页内容比窗口高，直接堆会把底部按钮顶出屏幕；每页一个滚动容器
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QWidget *w = new QWidget(scroll);
    scroll->setWidget(w);
    QVBoxLayout *lay = new QVBoxLayout(w);
    lay->setSpacing(10);
    QList<QPair<QString, QWidget *> > pages;

    TabUi ui;
    ui.sourceId = sourceId;
    ui.status = nullptr;
    ui.apiStatus = nullptr;
    ui.qrImage = nullptr;
    ui.qrState = nullptr;
    ui.qrGet = nullptr;
    ui.status = new QLabel(QStringLiteral("读取账号状态中…"), w);
    lay->addWidget(ui.status);
    ui.apiStatus = new QLabel(w);
    ui.apiStatus->setWordWrap(true);
    ui.apiStatus->setObjectName(QStringLiteral("subLabel"));
    lay->addWidget(ui.apiStatus);

    if (!methods.contains(QStringLiteral("qrcode")) && app) {
        // 页签上没有扫码入口时要把原因写出来，否则用户以为功能坏了
        QLabel *why = new QLabel(app->qrUnavailableReason(sourceId), w);
        why->setWordWrap(true);
        why->setObjectName(QStringLiteral("subLabel"));
        lay->addWidget(why);
    }

    if (methods.contains(QStringLiteral("qrcode"))) {
        QGroupBox *box = new QGroupBox(w);
        box->setTitle(QString());
        box->setObjectName(QStringLiteral("card"));
        QVBoxLayout *bl = new QVBoxLayout(box);
        QHBoxLayout *acts = new QHBoxLayout();
        QPushButton *get = new QPushButton(QStringLiteral("获取二维码"), box);
        QPushButton *cancel = new QPushButton(QStringLiteral("取消"), box);
        acts->addWidget(get);
        acts->addWidget(cancel);
        acts->addStretch(1);
        bl->addLayout(acts);

        // C++ 里写死的尺寸不吃主题缩放（QSS 才会放大），这里手动乘一次 pxScale，
        // 否则高分屏上二维码周围控件都变大了，只有码还是 180，手机贴着屏幕扫不到
        const int qrSide = qRound(180 * Furui::Theme::pxScale());
        ui.qrImage = new QLabel(box);
        ui.qrImage->setFixedSize(qrSide, qrSide);
        ui.qrImage->setAlignment(Qt::AlignCenter);
        ui.qrImage->setText(QStringLiteral("暂无二维码"));
        bl->addWidget(ui.qrImage, 0, Qt::AlignHCenter);

        ui.qrState = new QLabel(box);
        ui.qrState->setWordWrap(true);
        bl->addWidget(ui.qrState);
        pages.append(qMakePair(QStringLiteral("扫码登录"), static_cast<QWidget *>(box)));
        ui.qrGet = get;
        connect(get, &QPushButton::clicked, this, [this, sourceId]() {
            if (app) app->startQrLogin(sourceId);
        });
        connect(cancel, &QPushButton::clicked, this, [this]() {
            if (app) app->cancelQrLogin();
        });
    }

    if (methods.contains(QStringLiteral("cookie"))) {
        QGroupBox *box = new QGroupBox(w);
        box->setTitle(QString());
        box->setObjectName(QStringLiteral("card"));
        QVBoxLayout *bl = new QVBoxLayout(box);
        const bool canSync = app && app->userApiAvailable(sourceId);
        QLabel *tip = new QLabel(app ? app->cookieLoginHint(sourceId) : QString(), box);
        tip->setWordWrap(true);
        bl->addWidget(tip);
        QPlainTextEdit *edit = new QPlainTextEdit(box);
        edit->setPlaceholderText(QStringLiteral("粘贴该平台的 Cookie"));
        edit->setFixedHeight(70);
        bl->addWidget(edit);
        // 按钮不能承诺它做不到的事：没有账号接口的平台只验证 Cookie
        QPushButton *login = new QPushButton(canSync ? QStringLiteral("验证 Cookie 并同步歌单")
                                                    : QStringLiteral("验证 Cookie 并保存登录态"), box);
        bl->addWidget(login);
        pages.append(qMakePair(QStringLiteral("Cookie 登录"), static_cast<QWidget *>(box)));

        connect(login, &QPushButton::clicked, this, [this, sourceId, edit]() {
            if (!app || !edit) return;
            app->loginByCookie(sourceId, edit->toPlainText().trimmed());
        });
    }

    if (methods.contains(QStringLiteral("password"))) {
        QGroupBox *box = new QGroupBox(w);
        box->setTitle(QString());
        box->setObjectName(QStringLiteral("card"));
        QVBoxLayout *bl = new QVBoxLayout(box);
        QLabel *tip = new QLabel(QStringLiteral(
            "支持手机号或邮箱登录，系统会根据是否包含 @ 自动判断。"
            "密码以 MD5 提交，不会以明文离开本机。"), box);
        tip->setWordWrap(true);
        bl->addWidget(tip);
        QLineEdit *account = new QLineEdit(box);
        account->setPlaceholderText(QStringLiteral("手机号 或 邮箱"));
        bl->addWidget(account);
        QLineEdit *pwd = new QLineEdit(box);
        pwd->setPlaceholderText(QStringLiteral("登录密码"));
        pwd->setEchoMode(QLineEdit::Password);
        bl->addWidget(pwd);
        QPushButton *login = new QPushButton(QStringLiteral("登录并同步歌单"), box);
        bl->addWidget(login);
        pages.append(qMakePair(QStringLiteral("密码登录"), static_cast<QWidget *>(box)));

        connect(login, &QPushButton::clicked, this, [this, sourceId, account, pwd]() {
            if (!app || !account || !pwd) return;
            app->loginByPassword(sourceId, account->text().trimmed(), pwd->text());
        });
    }

    if (methods.contains(QStringLiteral("phone"))) {
        QGroupBox *box = new QGroupBox(w);
        box->setTitle(QString());
        box->setObjectName(QStringLiteral("card"));
        QVBoxLayout *bl = new QVBoxLayout(box);
        QLineEdit *phone = new QLineEdit(box);
        phone->setPlaceholderText(QStringLiteral("手机号"));
        bl->addWidget(phone);
        QHBoxLayout *codeLay = new QHBoxLayout();
        QLineEdit *code = new QLineEdit(box);
        code->setPlaceholderText(QStringLiteral("验证码"));
        QPushButton *send = new QPushButton(QStringLiteral("发送验证码"), box);
        codeLay->addWidget(code, 1);
        codeLay->addWidget(send);
        bl->addLayout(codeLay);
        QPushButton *login = new QPushButton(QStringLiteral("登录"), box);
        bl->addWidget(login);
        pages.append(qMakePair(QStringLiteral("短信登录"), static_cast<QWidget *>(box)));

        connect(send, &QPushButton::clicked, this, [this, sourceId, phone]() {
            if (!app || !phone) return;
            app->sendPhoneCode(sourceId, phone->text().trimmed());
        });
        connect(login, &QPushButton::clicked, this, [this, sourceId, phone, code]() {
            if (!app || !phone || !code) return;
            app->loginByPhone(sourceId, phone->text().trimmed(), code->text().trimmed());
        });
    }

    m_tabUis.append(ui);
    const int tabIndex = m_tabUis.size() - 1;
    if (!pages.isEmpty()) {
        QHBoxLayout *pills = new QHBoxLayout();
        pills->setSpacing(8);
        QStackedWidget *stack = new QStackedWidget(w);
        // QStackedWidget 默认双向 Expanding：横向会顶掉 AlignHCenter，
        // 纵向会把卡片拉出一段空白，所以两轴都改成 Preferred，下方留白交给 stretch
        stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        QButtonGroup *group = new QButtonGroup(w);
        group->setExclusive(true);
        for (int i = 0; i < pages.size(); ++i) {
            QPushButton *pill = new QPushButton(pages.at(i).first, w);
            pill->setProperty("role", "pill");
            pill->setCheckable(true);
            pill->setChecked(i == 0);
            group->addButton(pill, i);
            QWidget *methodPage = pages.at(i).second;
            methodPage->setMaximumWidth(430);
            // 各方式宽度不同，直接放进 stack 会让窄卡片贴左，这里再包一层居中
            QWidget *holder = new QWidget(w);
            QHBoxLayout *hl = new QHBoxLayout(holder);
            hl->setContentsMargins(0, 0, 0, 0);
            hl->addWidget(methodPage, 0, Qt::AlignHCenter);
            stack->addWidget(holder);
            pills->addWidget(pill);
            // 逐颗 pill 直连：QButtonGroup::idClicked 在这里实测不触发切页
            connect(pill, &QPushButton::clicked, stack, [this, stack, i, tabIndex]() {
                stack->setCurrentIndex(i);
                // 点开「扫码登录」就出码，不用再手动按一次「获取二维码」
                if (i == 0) showQrTab(tabIndex);
            });
        }
        pills->addStretch(1);
        lay->addLayout(pills);
        lay->addWidget(stack, 0, Qt::AlignHCenter);
        lay->addStretch(1);
    } else {
        lay->addStretch(1);
    }

    QHBoxLayout *ops = new QHBoxLayout();
    QPushButton *logout = new QPushButton(QStringLiteral("退出登录"), w);
    QPushButton *sync = new QPushButton(QStringLiteral("同步歌单与收藏"), w);
    ops->addWidget(logout);
    ops->addWidget(sync);
    ops->addStretch(1);
    lay->addLayout(ops);

    connect(logout, &QPushButton::clicked, this, [this, sourceId]() {
        if (app) app->logout(sourceId);
    });
    connect(sync, &QPushButton::clicked, this, [this, sourceId]() {
        if (app) app->syncUserLibrary(sourceId);
    });

    // 必须返回滚动容器：返回 w 的话 addTab 会把 w 从 scroll 里接走，滚动条就成了孤儿
    return scroll;
}

void LoginDialog::refreshApiStatus()
{
    if (!app) return;
    for (TabUi &ui : m_tabUis) {
        if (ui.apiStatus)
            ui.apiStatus->setText(QStringLiteral("账号接口：") + app->accountApiStatus(ui.sourceId));
    }
}

void LoginDialog::refreshStatus()
{
    if (!app) return;
    const QVector<Furui::Account> &list = app->accounts()->accounts();
    for (TabUi &ui : m_tabUis) {
        if (!ui.status) continue;
        QString text = QStringLiteral("未登录");
        for (const Furui::Account &a : list) {
            if (a.source != ui.sourceId) continue;
            if (a.logged) {
                const QString who = a.nickname.isEmpty() ? a.uid : a.nickname;
                text = QStringLiteral("已登录：") + who;
                if (!a.status.isEmpty()) text += QStringLiteral("  ") + a.status;
            } else {
                text = a.status.isEmpty() ? QStringLiteral("未登录") : a.status;
            }
            break;
        }
        ui.status->setText(QStringLiteral("当前状态：") + text);
    }
}

void LoginDialog::refreshQr()
{
    if (!app) return;
    const QString path = app->qrImagePath();
    const QString state = app->qrStatus();
    const QString hint = app->qrHint();
    const QString source = app->qrSource();
    // 会话进了终态（成功/过期/失败）就不再算「活着」：
    // 继续显示旧码会让人去扫一张已经作废的二维码，也不会在切回该页时重新出码
    const bool live = app->qrActive();
    const bool anyActive = live && !source.isEmpty();
    for (TabUi &ui : m_tabUis) {
        if (!ui.qrImage || !ui.qrState) continue;
        // 同一时刻只有一套扫码会话，状态与图片都只属于发起扫码的那个平台
        const bool mine = (source == ui.sourceId);
        if (mine && live && !path.isEmpty()) {
            QPixmap pm(path);
            if (!pm.isNull()) {
                // 二维码是二值图，平滑插值会把黑白模块糊出灰边，手机反而更难识别
                ui.qrImage->setPixmap(pm.scaled(ui.qrImage->size(), Qt::KeepAspectRatio,
                                                Qt::FastTransformation));
                ui.qrImage->setText(QString());
            } else {
                ui.qrImage->setPixmap(QPixmap());
                ui.qrImage->setText(QStringLiteral("二维码地址：") + path);
            }
        } else {
            ui.qrImage->setPixmap(QPixmap());
            ui.qrImage->setText(anyActive ? QStringLiteral("正在其他平台扫码")
                                          : QStringLiteral("暂无二维码"));
        }
        ui.qrState->setText(mine ? state + (hint.isEmpty() ? QString()
                                                            : QStringLiteral("  ") + hint)
                                 : QString());
    }
}

void LoginDialog::showQrTab(int tabIndex)
{
    if (!app || tabIndex < 0 || tabIndex >= m_tabUis.size()) return;
    const TabUi &ui = m_tabUis.at(tabIndex);
    if (!ui.qrGet) return;
    // 会话还在轮询（出码中/等待扫码/已扫码）就交给轮询，不要重复申请新码；
    // 已经进了终态（过期/成功/失败）则重新出一张，否则用户只能自己找按钮
    if (app->qrActive() && app->qrSource() == ui.sourceId) return;
    ui.qrGet->click();
}

void LoginDialog::connectSignals()
{
    if (!app) return;
    connect(app, &Furui::AppController::loginStateChanged, this,
            [this](const QString &, bool, const QString &msg) {
                if (!app) return;
                if (m_hint) m_hint->setText(msg);
                refreshStatus();
            });
    connect(app, &Furui::AppController::qrChanged, this, [this]() { refreshQr(); });
    connect(app, &Furui::AppController::settingsChanged, this, [this]() { refreshApiStatus(); });
    connect(app, &Furui::AppController::toast, this, [this](const QString &msg) {
        if (m_hint) m_hint->setText(msg);
    });
    connect(app->accounts(), &QAbstractItemModel::modelReset, this, [this]() { refreshStatus(); });
}

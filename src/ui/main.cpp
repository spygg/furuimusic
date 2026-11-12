// 福瑞音乐 - 桌面与移动共用的 QML 入口
#include "core/QtCompat.h"
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <QtGlobal>

#include "../core/AppController.h"
#include "../core/AuthManager.h"
#include "../core/HttpClient.h"
#include "GlyphImageProvider.h"
#include "PlatformAdapter.h"
#include "ThemeManager.h"
#ifdef FURUI_HAVE_FFMPEG
#include "FfmpegVideoPlayer.h"
#endif

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("福瑞音乐"));
    QGuiApplication::setOrganizationName(QStringLiteral("Furui"));

    // 代理策略必须在 QML 引擎创建前确定：QML 的 Image / 音频走 Qt 默认的
    // QNetworkAccessManager，只认应用级代理。这里与 HttpClient 保持完全一致，
    // 设置为空时强制 NoProxy，避免 Android 上系统代理探测返回无效结果时
    // 封面全部报 “Proxy connection refused”。
    Furui::HttpClient::applyApplicationProxyFromSettings();

    // Qt 6 在 Windows 默认使用原生样式，会忽略 QML 里自定义的 background/contentItem/handle；
    // 统一固定 Basic 样式，保证 Qt 5 / Qt 6 及各平台外观一致，也让控件完全可定制。
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // 自绘无边框窗口不会自动继承 exe 图标：显式设置窗口图标，任务栏与 Alt+Tab 才能显示
    QIcon appIcon;
    static const int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
    for (int size : kIconSizes)
        appIcon.addFile(QStringLiteral(":/icons/app-%1.png").arg(size));
    QGuiApplication::setWindowIcon(appIcon);
    // Linux Wayland 用 desktop file 名匹配应用图标（与 CI AppImage 的 FuruiMusic.desktop 一致）
    QGuiApplication::setDesktopFileName(QStringLiteral("FuruiMusic"));

    // 应用名与数据目录确定后再创建门面单例
    Furui::PlatformAdapter platform;
    Furui::ThemeManager theme;
    // 截图/自检用：FURUI_THEME=light|dark 只影响本次运行，不写回设置
    if (!qEnvironmentVariableIsEmpty("FURUI_THEME"))
        theme.previewMode(qEnvironmentVariable("FURUI_THEME"));
    QQmlApplicationEngine engine;
    // Qt5 + 自编译 FFmpeg 后端才注册；Qt6 用官方 QtMultimedia 插件，不链接 FFmpeg。
    // MvMedia5Ffmpeg.qml 只会在 platform.ffmpegBackend 为 true 时被 Loader 加载。
#ifdef FURUI_HAVE_FFMPEG
    qmlRegisterType<Furui::FfmpegVideoPlayer>("FuruiMusic", 1, 0, "FfmpegVideoPlayer");
#endif
    // 图标统一由 C++ 侧栅格化，QML 里用 image://glyph/... 引用（见 GlyphIcon.qml）
    engine.addImageProvider(QStringLiteral("glyph"), new Furui::GlyphImageProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                             Furui::AppController::instance());
    engine.rootContext()->setContextProperty(QStringLiteral("platform"), &platform);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    // QML 组件加载失败时 Qt 只在部分路径打印警告；排障时打开 FURUI_QML_WARN=1 可确保全量输出，
    // 否则 Loader 静默变成空壳（status=Error）很难定位。
    if (!qEnvironmentVariableIsEmpty("FURUI_QML_WARN")) {
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [](const QList<QQmlError> &ws) {
            for (const QQmlError &w : ws)
                fprintf(stderr, "[qml] %s\n", w.toString().toUtf8().constData());
        });
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    // 无人值守截图校验：设置 FURUI_CAPTURE=<png 路径> 时抓取窗口内容后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_CAPTURE")) {
        const QString path = qEnvironmentVariable("FURUI_CAPTURE");
        const QString size = qEnvironmentVariable("FURUI_WINDOW_SIZE");
        QTimer::singleShot(1200, &engine, [&engine, size] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
                if (!size.isEmpty()) {
                    const QStringList wh = size.split(QLatin1Char('x'));
                    if (wh.size() == 2)
                        window->resize(wh.at(0).toInt(), wh.at(1).toInt());
                }
                // FURUI_CAPTURE_SOURCE=kuwo：截图前把账号页切到指定平台。
                // 账号页默认选 sourceIds[0]（网易云），没有这个钩子就截不到别的平台。
                const QString wantSource = qEnvironmentVariable("FURUI_CAPTURE_SOURCE");
                if (!wantSource.isEmpty() && window->contentItem()) {
                    std::function<bool(QQuickItem *)> pick = [&pick, wantSource](QQuickItem *it) {
                        if (it->property("selectedSource").isValid()) {
                            it->setProperty("selectedSource", wantSource);
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (pick(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (pick(child)) break;
                }
                // FURUI_CAPTURE_METHOD=phone / qrcode / password：再切一下登录方式 Tab
                const QString wantMethod = qEnvironmentVariable("FURUI_CAPTURE_METHOD");
                if (!wantMethod.isEmpty() && window->contentItem()) {
                    std::function<bool(QQuickItem *)> pick = [&pick, wantMethod](QQuickItem *it) {
                        if (it->property("method").isValid()
                            && it->property("methods").isValid()) {
                            it->setProperty("method", wantMethod);
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (pick(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (pick(child)) break;
                }
                // FURUI_CAPTURE_QR=kuwo：截图前直接开一次扫码会话（验证出码链路，省去点击按钮）
                const QString qrSource = qEnvironmentVariable("FURUI_CAPTURE_QR");
                if (!qrSource.isEmpty()) {
                    if (QObject *app = engine.rootContext()->contextProperty("app")
                                           .value<QObject *>())
                        QMetaObject::invokeMethod(app, "startQrLogin", Q_ARG(QString, qrSource));
                }
                // FURUI_CAPTURE_QUEUE=1：打开外壳的播放队列（窄屏是底部面板），
                // 用来证明「队列按钮不是空信号」。放在这一步而不是紧贴截图前，
                // 是因为面板 y 有 220ms 动画，卡在动画中间帧只会拍到还没升起来的位置。
                if (qEnvironmentVariableIntValue("FURUI_CAPTURE_QUEUE") > 0
                    && window->contentItem()) {
                    std::function<bool(QQuickItem *)> seekQueue = [&seekQueue](QQuickItem *it) {
                        if (it->property("queueOpen").isValid()) {
                            it->setProperty("queueOpen", true);
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (seekQueue(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (seekQueue(child)) break;
                }
                // FURUI_CAPTURE_SEARCH=<kw>：截图前发起一次搜索，专门用来截「结果还没回来」
                // 那一段的骨架屏。搜索是异步的，所以抓图延时要和这一拍配合。
                const QString wantSearch = qEnvironmentVariable("FURUI_CAPTURE_SEARCH");
                if (!wantSearch.isEmpty()) {
                    QObject *a = engine.rootContext()
                            ->contextProperty(QStringLiteral("app")).value<QObject *>();
                    if (a)
                        QMetaObject::invokeMethod(a, "search",
                                                  Q_ARG(QString, wantSearch), Q_ARG(QString, QString()));
                }
                // FURUI_CAPTURE_TAB_EARLY=<n>：在拉数据之前就把音乐库切到目标 Tab。
                // 抓图那一刻才切 Tab 会让隐藏页面里的 Image 来不及完成异步加载，
                // 用它来区分「网络取不到」和「页面隐藏时没加载」两种情况。
                const QString earlyTab = qEnvironmentVariable("FURUI_CAPTURE_TAB_EARLY");
                if (!earlyTab.isEmpty() && window->contentItem()) {
                    std::function<bool(QQuickItem *)> seekEarly = [&seekEarly, earlyTab](QQuickItem *it) {
                        if (it->property("tabIndex").isValid()
                            && it->property("tabTitles").isValid()) {
                            it->setProperty("tabIndex", earlyTab.toInt());
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (seekEarly(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (seekEarly(child)) break;
                }
                // FURUI_CAPTURE_MV=1：提前拉一次 MV 榜单，配合 FURUI_CAPTURE_TAB=5
                // 与较长的 FURUI_CAPTURE_MS 截到真实封面网格，而不是空态
                if (qEnvironmentVariableIntValue("FURUI_CAPTURE_MV") > 0) {
                    if (QObject *a = engine.rootContext()
                            ->contextProperty(QStringLiteral("app")).value<QObject *>())
                        QMetaObject::invokeMethod(a, "loadMvList",
                                                  Q_ARG(QString, QStringLiteral("全部")),
                                                  Q_ARG(bool, false));
                }
                // FURUI_MV_PLAY_URL=<url>：无人值守验证 MV 播放后端（Qt5 自编译
                // FFmpeg / Qt6 官方插件）。直接发 mvOpened，外壳会切到沉浸式
                // MV 页并调用 open(url)；配 FURUI_CAPTURE_MS 等到解出画面再抓图。
                const QString mvPlayUrl = qEnvironmentVariable("FURUI_MV_PLAY_URL");
                if (!mvPlayUrl.isEmpty()) {
                    if (QObject *a = engine.rootContext()
                            ->contextProperty(QStringLiteral("app")).value<QObject *>()) {
                        QVariantMap mv;
                        mv.insert(QStringLiteral("url"), mvPlayUrl);
                        mv.insert(QStringLiteral("title"), QStringLiteral("FFmpeg 后端验证"));
                        mv.insert(QStringLiteral("artist"), QStringLiteral("FuruiMusic"));
                        mv.insert(QStringLiteral("cover"), QString());
                        mv.insert(QStringLiteral("hasPrev"), false);
                        mv.insert(QStringLiteral("hasNext"), false);
                        QMetaObject::invokeMethod(a, "mvOpened", Q_ARG(QVariantMap, mv));
                    }
                }
                // FURUI_CAPTURE_TOAST=<text>：发一条带语义色的通知，用来截统一 Toast
                const QString toastText = qEnvironmentVariable("FURUI_CAPTURE_TOAST");
                if (!toastText.isEmpty()) {
                    if (QObject *a = engine.rootContext()
                            ->contextProperty(QStringLiteral("app")).value<QObject *>())
                        QMetaObject::invokeMethod(a, "notify",
                                                  Q_ARG(QString, toastText),
                                                  Q_ARG(QString, QStringLiteral("success")));
                }
                // FURUI_CAPTURE_PLAY=<kw>：搜索并把第一条结果播起来，用来截「有歌词的播放页」。
                // 播放页的歌词渐隐、扫光都得有歌才看得见，光切 pageIndex 只能截到空态。
                const QString wantPlay = qEnvironmentVariable("FURUI_CAPTURE_PLAY");
                if (!wantPlay.isEmpty()) {
                    QObject *a = engine.rootContext()
                            ->contextProperty(QStringLiteral("app")).value<QObject *>();
                    if (a) {
                        QMetaObject::invokeMethod(a, "search",
                                                  Q_ARG(QString, wantPlay), Q_ARG(QString, QString()));
                        // 搜索结果要等网络回来才有，第二轮再点第一首（playResult 自己会取流）
                        QTimer::singleShot(3200, a, [a] {
                            QMetaObject::invokeMethod(a, "playResult", Q_ARG(int, 0));
                        });
                    }
                }
            }
        });
        // 抓图延时可用 FURUI_CAPTURE_MS 覆盖（二维码这类异步内容需要等网络回来）
        const int captureMs = qEnvironmentVariableIntValue("FURUI_CAPTURE_MS") > 0
                              ? qEnvironmentVariableIntValue("FURUI_CAPTURE_MS") : 1900;
        QTimer::singleShot(captureMs, &engine, [&engine, path] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
                // FURUI_CAPTURE_PAGE 可在截图前直接切到指定页面（0 曲库 / 1 播放 /
                // 2 账号 / 3 设置，4 是 MV 覆盖页），免得为了截播放页去模拟点击侧栏
                const QString page = qEnvironmentVariable("FURUI_CAPTURE_PAGE");
                if (!page.isEmpty() && window->contentItem()) {
                    // 页面索引在 MainDesktop / MainMobile 里，外壳是经 Loader 载入的，
                    // 所以要递归找带 pageIndex 属性的节点，只扫第一层是找不到的
                    std::function<bool(QQuickItem *)> seek = [&seek, page](QQuickItem *it) {
                        if (it->property("pageIndex").isValid()) {
                            it->setProperty("pageIndex", page.toInt());
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (seek(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (seek(child)) break;
                }
                // FURUI_CAPTURE_TAB 可在截图前把音乐库切到指定 Tab（4 = 发现榜单）
                const QString tab = qEnvironmentVariable("FURUI_CAPTURE_TAB");
                if (!tab.isEmpty() && window->contentItem()) {
                    std::function<bool(QQuickItem *)> seekTab = [&seekTab, tab](QQuickItem *it) {
                        if (it->property("tabIndex").isValid()
                            && it->property("tabTitles").isValid()) {
                            it->setProperty("tabIndex", tab.toInt());
                            return true;
                        }
                        for (QQuickItem *c : it->childItems())
                            if (seekTab(c)) return true;
                        return false;
                    };
                    for (QQuickItem *child : window->contentItem()->childItems())
                        if (seekTab(child)) break;
                }
                window->grabWindow().save(path);
                // 同时把场景树尺寸打到 stderr：截图空白时能立刻分清是「没加载」还是「尺寸为 0」
                if (!qEnvironmentVariableIsEmpty("FURUI_CAPTURE_TREE")) {
                    // absBottom 用于直接定位「溢出到窗口外」的项：矮视口下
                    // 页面列布局撑破底边，靠肉眼看截图半行很难判断是谁挤的
                    const qreal winH = window->height();
                    std::function<void(QQuickItem *, int, qreal, qreal)> dump =
                        [&dump, winH](QQuickItem *it, int depth, qreal offX, qreal offY) {
                        QString line(depth * 2, QLatin1Char(' '));
                        const qreal absTop = offY + it->y();
                        const qreal absBottom = absTop + it->height();
                        line += QStringLiteral("%1 %2x%3 @%4,%5 absY=%6..%7%8")
                                    .arg(QString::fromUtf8(it->metaObject()->className()))
                                    .arg(int(it->width())).arg(int(it->height()))
                                    .arg(int(it->x())).arg(int(it->y()))
                                    .arg(int(absTop)).arg(int(absBottom))
                                    .arg(absBottom > winH + 1 ? QStringLiteral("  <<OVERFLOW") : QString());
                        const QVariant txt = it->property("text");
                        if (txt.isValid() && txt.userType() == QMetaType::QString) {
                            const QString s = txt.toString().trimmed();
                            if (!s.isEmpty())
                                line += QStringLiteral("  \"%1\"").arg(s.left(18));
                        }
                        fprintf(stderr, "%s\n", line.toUtf8().constData());
                        if (it->property("status").isValid())
                            fprintf(stderr, "  ^ loader status=%d item=%p source=%s\n",
                                    it->property("status").toInt(),
                                    it->property("item").value<QObject *>(),
                                    qPrintable(it->property("source").toString()));
                        if (depth > 14) return;
                        for (QQuickItem *c : it->childItems())
                            dump(c, depth + 1, offX + it->x(), absTop);
                    };
                    dump(window->contentItem(), 0, 0, 0);
                }
            }
            QCoreApplication::quit();
        });
    }

    // 逐行扫光探针：FURUI_PROBE_LYRIC=<source>:<keyword>，配 FURUI_PROBE_OUT=<png 前缀>。
    // 搜索 → 播第一首 → 等歌词回来 → 切到播放页，间隔两次采样 karaoke 与 position。
    // 单看一张静态图分不清「扫光在推进」和「一直是 0」，所以必须比对两次采样。
    if (!qEnvironmentVariableIsEmpty("FURUI_PROBE_LYRIC")) {
        const QString probe = qEnvironmentVariable("FURUI_PROBE_LYRIC");
        const int sep = probe.indexOf(QLatin1Char(':'));
        const QString probeSrc = sep > 0 ? probe.left(sep) : QStringLiteral("netease");
        const QString probeKw = sep > 0 ? probe.mid(sep + 1) : probe;
        const QString probeOut = qEnvironmentVariable("FURUI_PROBE_OUT");
        QObject *appObj = engine.rootContext()->contextProperty(QStringLiteral("app")).value<QObject *>();
        auto *probeWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));

        auto showPlayer = [probeWindow] {
            std::function<bool(QQuickItem *)> toPlayer = [&toPlayer](QQuickItem *it) {
                if (it->property("pageIndex").isValid()) {
                    it->setProperty("pageIndex", 1);
                    return true;
                }
                for (QQuickItem *c : it->childItems()) if (toPlayer(c)) return true;
                return false;
            };
            if (!probeWindow || !probeWindow->contentItem())
                return;
            for (QQuickItem *child : probeWindow->contentItem()->childItems())
                if (toPlayer(child)) break;
            // 竖屏的歌词在 SwipeView 第 2 页，不翻过去截到的永远是封面。只认类名，
            // 免得把别的 currentIndex（Repeater 等）一起改了。
            std::function<bool(QQuickItem *)> toLyric = [&toLyric](QQuickItem *it) {
                if (QString::fromLatin1(it->metaObject()->className()).contains(QLatin1String("SwipeView"))) {
                    it->setProperty("currentIndex", 1);
                    return true;
                }
                for (QQuickItem *c : it->childItems()) if (toLyric(c)) return true;
                return false;
            };
            toLyric(probeWindow->contentItem());
        };

        auto sample = [appObj, probeWindow, probeOut, showPlayer](int shot) {
            QStringList karaoke;
            int rowIdx = 0;
            int tickMs = -1;
            if (probeWindow && probeWindow->contentItem()) {
                showPlayer();
                std::function<void(QQuickItem *)> walk = [&walk, &karaoke, &rowIdx, &tickMs](QQuickItem *it) {
                    if (it->property("tickMs").isValid()) {
                        tickMs = it->property("tickMs").toInt();
                        karaoke << QStringLiteral("view[h=%1 contentY=%2 ch=%3]")
                                       .arg(int(it->height()))
                                       .arg(it->property("contentY").toDouble(), 0, 'f', 1)
                                       .arg(it->property("contentHeight").toDouble(), 0, 'f', 1);
                    }
                    if (it->property("karaoke").isValid()) {
                        // 委托按 model 顺序遍历，累加的序号就是行号；只打非 0 的那些，
                        // 否则一屏几十个 0.000 会把有效信息完全埋掉
                        const double v = it->property("karaoke").toDouble();
                        const bool act = it->property("active").toBool();
                        ++rowIdx;
                        if (act || (v > 0.0005 && karaoke.size() < 4)) {
                            QString sweep = QStringLiteral("(no sweep item)");
                            std::function<bool(QQuickItem *)> find =
                                    [&find, &sweep](QQuickItem *c) {
                                if (c->property("inset").isValid()) {
                                    // scale/opacity 要问 lineBox（父层），委托本身永远是 1.0：
                                    // 早先读错了一层，据此得出的「Behavior 冻结」结论是假的
                                    QQuickItem *box = c->parentItem();
                                    sweep = QStringLiteral("sweep[%1x%2 inset=%3] textW=%4 boxSc=%5 boxOp=%6")
                                                    .arg(int(c->width())).arg(int(c->height()))
                                                    .arg(c->property("inset").toDouble(), 0, 'f', 1)
                                                    .arg(box->property("textW").toDouble(), 0, 'f', 1)
                                                    .arg(box->scale(), 0, 'f', 3)
                                                    .arg(box->opacity(), 0, 'f', 2);
                                    return true;
                                }
                                for (QQuickItem *g : c->childItems()) if (find(g)) return true;
                                return false;
                            };
                            find(it);
                            karaoke << QStringLiteral("%1%2:%3 %4 row[y=%5 h=%6 vis=%7]")
                                           .arg(act ? QStringLiteral("ACT r") : QStringLiteral("r"))
                                           .arg(rowIdx - 1).arg(v, 0, 'f', 3).arg(sweep)
                                           .arg(int(it->y())).arg(int(it->height()))
                                           .arg(it->isVisible() ? 1 : 0);
                        }
                    }
                    for (QQuickItem *c : it->childItems()) walk(c);
                };
                walk(probeWindow->contentItem());
                if (!probeOut.isEmpty())
                    probeWindow->grabWindow().save(probeOut + QStringLiteral("-%1.png").arg(shot));
            }
            const QStringList lines = appObj
                ? appObj->property("lyricLines").toStringList() : QStringList();
            const QVariantList times = appObj
                ? appObj->property("lyricTimes").toList() : QVariantList();
            // Qt6 的 size() 是 qsizetype（long long），用 %d 打出来是垃圾值
            fprintf(stderr, "[lyric] shot=%d lines=%d timed=%d idx=%d pos=%lld dur=%lld tick=%d karaoke=%s status=%s\n",
                    shot, static_cast<int>(lines.size()), static_cast<int>(times.size()),
                    appObj ? appObj->property("currentLyricIndex").toInt() : -1,
                    appObj ? static_cast<long long>(appObj->property("position").toLongLong()) : -1,
                    appObj ? static_cast<long long>(appObj->property("duration").toLongLong()) : -1,
                    tickMs,
                    qPrintable(karaoke.join(QLatin1Char(','))),
                    qPrintable(appObj ? appObj->property("statusMessage").toString() : QString()));
        };

        QTimer::singleShot(1400, &engine, [appObj, probeSrc, probeKw, probeWindow] {
            // 探针自己负责尺寸：FURUI_WINDOW_SIZE 只有截图回归那段会读，这里不 resize
            // 就只能拿到默认窗口，验证不了宽/窄屏两种歌词排版。
            const QStringList wh = qEnvironmentVariable("FURUI_WINDOW_SIZE")
                                       .split(QLatin1Char('x'), Qt::SkipEmptyParts);
            if (probeWindow && wh.size() == 2)
                probeWindow->resize(wh.at(0).toInt(), wh.at(1).toInt());
            if (appObj) QMetaObject::invokeMethod(appObj, "search",
                                                  Q_ARG(QString, probeKw), Q_ARG(QString, probeSrc));
        });
        QTimer::singleShot(6000, &engine, [appObj] {
            if (appObj) QMetaObject::invokeMethod(appObj, "playResult", Q_ARG(int, 0));
        });
        // 播放页要提前切好：和采样同一轮事件循环才切的话，重新居中的 80ms 定时器还没跑，
        // 量到的 contentY 还是旧布局的，看起来就像「当前行贴在顶边」。
        QTimer::singleShot(9000, &engine, [showPlayer] { showPlayer(); });
        // 取链被 VIP 拒掉时播放器一直是 0，扫光永远看不到；直接 seek 一个中间位置，
        // 让「数据链路（times/position）」和「渲染链路（clip 对齐）」分开验证。
        QTimer::singleShot(10000, &engine, [appObj] {
            if (appObj) QMetaObject::invokeMethod(appObj, "seek", Q_ARG(qint64, 65000));
        });
        QTimer::singleShot(13000, &engine, [sample] { sample(0); });
        QTimer::singleShot(15000, &engine, [appObj] {
            if (appObj) QMetaObject::invokeMethod(appObj, "seek", Q_ARG(qint64, 78000));
        });
        QTimer::singleShot(17000, &engine, [sample] { sample(1); });
        QTimer::singleShot(18000, &engine, [] { QCoreApplication::quit(); });
    }

    // 形态切换回归：FURUI_SEQ=<png 前缀> 配 FURUI_SEQ_SIZES=1280x720,640x720,...
    // 每步先 resize、等外壳稳定后截图，并把「当前外壳 + 页面序号」打到 stderr。
    // 车机的横竖屏/分屏来回切必须既不丢页面也不反复重建，只看单张静态图发现不了抖动。
    if (!qEnvironmentVariableIsEmpty("FURUI_SEQ")) {
        const QString prefix = qEnvironmentVariable("FURUI_SEQ");
        const QStringList sizes = qEnvironmentVariable("FURUI_SEQ_SIZES")
                .split(QLatin1Char(','), Qt::SkipEmptyParts);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        if (window && !sizes.isEmpty()) {
            // 外壳在 Loader 里，pageIndex 在其子节点上，逐层找到才拿得到真实状态
            auto probe = [window, &platform]() -> QString {
                QString source = QStringLiteral("(none)");
                QString page = QStringLiteral("-");
                QString swaps = QStringLiteral("-");
                std::function<void(QQuickItem *)> walk = [&walk, &source, &page, &swaps](QQuickItem *it) {
                    // 只认第一个匹配的 Loader：页面里还有别的 Loader，后写会盖掉外壳的信息
                    if (source == QLatin1String("(none)")
                        && it->property("status").isValid() && it->property("source").isValid()) {
                        source = it->property("source").toString().section(QLatin1Char('/'), -1);
                        if (it->property("swaps").isValid())
                            swaps = QString::number(it->property("swaps").toInt());
                    }
                    if (page == QLatin1String("-") && it->property("pageIndex").isValid())
                        page = QString::number(it->property("pageIndex").toInt());
                    for (QQuickItem *c : it->childItems())
                        walk(c);
                };
                if (window->contentItem())
                    for (QQuickItem *child : window->contentItem()->childItems())
                        walk(child);
                return QStringLiteral("%1x%2 shell=%3 page=%4 swaps=%5 insets=%6,%7,%8,%9")
                        .arg(int(window->width())).arg(int(window->height())).arg(source, page, swaps)
                        .arg(platform.insetLeft()).arg(platform.insetTop())
                        .arg(platform.insetRight()).arg(platform.insetBottom());
            };

            for (int i = 0; i < sizes.size(); ++i) {
                const QString size = sizes.at(i);
                const int at = 700 + i * 1200;
                QTimer::singleShot(at, window, [window, size] {
                    const QStringList wh = size.split(QLatin1Char('x'));
                    if (wh.size() == 2)
                        window->resize(wh.at(0).toInt(), wh.at(1).toInt());
                });
                // FURUI_SEQ_PAGE：第一步就把页面切到指定序号，之后靠截图确认换外壳没被重置回首页
                if (i == 0 && !qEnvironmentVariableIsEmpty("FURUI_SEQ_PAGE")) {
                    const int page = qEnvironmentVariableIntValue("FURUI_SEQ_PAGE");
                    QTimer::singleShot(at + 300, window, [window, page] {
                        std::function<bool(QQuickItem *)> seek = [&seek, page](QQuickItem *it) {
                            if (it->property("pageIndex").isValid()) {
                                it->setProperty("pageIndex", page);
                                return true;
                            }
                            for (QQuickItem *c : it->childItems())
                                if (seek(c)) return true;
                            return false;
                        };
                        if (window->contentItem())
                            for (QQuickItem *child : window->contentItem()->childItems())
                                if (seek(child)) break;
                    });
                }
                // 600ms 远大于 QML 侧 170ms 的去抖窗口，截到的是稳定态而不是过渡帧
                QTimer::singleShot(at + 600, window, [window, prefix, size, i, probe] {
                    fprintf(stderr, "[seq] step=%d want=%s %s\n", i, qPrintable(size),
                            qPrintable(probe()));
                    window->grabWindow().save(prefix + QStringLiteral("_%1_").arg(i) + size + QStringLiteral(".png"));
                });
            }
            QTimer::singleShot(700 + sizes.size() * 1200 + 800, window, [] { QCoreApplication::quit(); });
        }
    }

    // 无边框与主题自检：设置 FURUI_SELFTEST=<日志路径> 时切换窗口状态并记录几何，结束后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_SELFTEST")) {
        const QString logPath = qEnvironmentVariable("FURUI_SELFTEST");
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        const auto report = [window, logPath, &platform](const char *tag) {
            // QML 载入失败或 offscreen 平台下 rootObjects 可能为空，screen 也可能为 null；
            // 这里不判空就会让自检直接崩溃，反而看不到它本来要报的那一行
            if (!window) return;
            const QRect g = window->geometry();
            const QRect a = window->screen() ? window->screen()->availableGeometry() : QRect();
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                // insets 是 QML 在 visibility 变化时回灌的：最大化时必须为 0（Windows 最大化
                // 本来就避开任务栏），非 0 说明安全区公式错了、界面会平白多出一圈留白
                f.write(QStringLiteral("%1 geometry=%2,%3 %4x%5 available=%6,%7 %8x%9 state=%10 insets=%11,%12,%13,%14\n")
                            .arg(QString::fromLatin1(tag))
                            .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                            .arg(a.x()).arg(a.y()).arg(a.width()).arg(a.height())
                            .arg(int(furuiWindowStates(window)))
                            .arg(platform.insetLeft()).arg(platform.insetTop())
                            .arg(platform.insetRight()).arg(platform.insetBottom())
                            .toUtf8());
                f.close();
            }
        };
        QTimer::singleShot(900, window, [report] { report("normal"); });
        QTimer::singleShot(1100, window, [window, &platform] { platform.toggleMaximize(window); });
        QTimer::singleShot(1700, window, [report] { report("maximized"); });
        QTimer::singleShot(1900, window, [window, &platform] { platform.toggleMaximize(window); });
        QTimer::singleShot(2400, window, [report] { report("restored"); });
        // 自检只改本次运行的外观：setMode 会写回 settings.ini，把用户的主题改掉
        QTimer::singleShot(2600, window, [&theme] { theme.previewMode(QStringLiteral("dark")); });
        QTimer::singleShot(3000, window, [&theme, logPath] {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("theme mode=%1 dark=%2\n")
                            .arg(theme.mode()).arg(int(theme.isDark())).toUtf8());
                f.close();
            }
        });
        QTimer::singleShot(3200, &engine, [] { QCoreApplication::quit(); });
    }

    // 扫码链路自检：FURUI_QR_DIAG=<日志路径> 时自动发起网易云扫码，逐次记录状态后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_QR_DIAG")) {
        const QString logPath = qEnvironmentVariable("FURUI_QR_DIAG");
        auto *auth = Furui::AuthManager::instance();
        auto *ctl = Furui::AppController::instance();
        QObject::connect(auth, &Furui::AuthManager::qrChanged, &engine, [auth, logPath] {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("status=%1 active=%2 image=%3 hint=%4\n")
                            .arg(auth->qrStatus()).arg(int(auth->qrActive()))
                            .arg(auth->qrImagePath()).arg(auth->qrHint()).toUtf8());
                f.close();
            }
        });
        QObject::connect(auth, &Furui::AuthManager::loginStateChanged, &engine,
                         [logPath](const QString &id, bool ok, const QString &msg) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("loginState %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg).toUtf8());
                f.close();
            }
        });
        QObject::connect(auth, &Furui::AuthManager::librarySynced, &engine,
                         [logPath](const QString &id, bool ok, const QString &msg) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("librarySynced %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg).toUtf8());
                f.close();
            }
        });
        // 默认自检网易云，FURUI_QR_SOURCE 可改成 migu / qq / kugou
        const QString qrSource = qEnvironmentVariableIsEmpty("FURUI_QR_SOURCE")
                                 ? QStringLiteral("netease")
                                 : qEnvironmentVariable("FURUI_QR_SOURCE");
        QTimer::singleShot(600, ctl, [ctl, qrSource] { ctl->startQrLogin(qrSource); });
        QTimer::singleShot(qEnvironmentVariableIntValue("FURUI_QR_DIAG_MS") > 0
                               ? qEnvironmentVariableIntValue("FURUI_QR_DIAG_MS") : 45000,
                           &engine, [] { QCoreApplication::quit(); });
    }

    // 密码登录自检：FURUI_PWD_DIAG=<日志路径> + FURUI_PWD_ACCOUNT / FURUI_PWD_PASSWORD，
    // 用假账号也能验证「协议是否被服务端接受」——真账号回 502 之前会先回 501/502，
    // 而协议/加密写错时网易云一律回 400 或 8821。
    if (!qEnvironmentVariableIsEmpty("FURUI_PWD_DIAG")) {
        const QString logPath = qEnvironmentVariable("FURUI_PWD_DIAG");
        auto *auth = Furui::AuthManager::instance();
        auto *ctl = Furui::AppController::instance();
        QObject::connect(auth, &Furui::AuthManager::loginStateChanged, &engine,
            [logPath](const QString &src, bool ok, const QString &msg) {
                QFile f(logPath);
                if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                    f.write(QStringLiteral("login source=%1 ok=%2 msg=%3\n")
                                .arg(src).arg(ok).arg(msg).toUtf8());
                    f.close();
                }
            });
        const QString pwdSource = qEnvironmentVariableIsEmpty("FURUI_PWD_SOURCE")
                                  ? QStringLiteral("netease")
                                  : qEnvironmentVariable("FURUI_PWD_SOURCE");
        // 不传账号时用一组假号码：只验证「服务端是否接受这套协议」（预期回 502 账号或密码错误，
        // 而不是 400 参数错误 / 8821），不需要真实凭据
        const QString acc = qEnvironmentVariableIsEmpty("FURUI_PWD_ACCOUNT")
                                ? QStringLiteral("13800000000")
                                : qEnvironmentVariable("FURUI_PWD_ACCOUNT");
        const QString pwd = qEnvironmentVariableIsEmpty("FURUI_PWD_PASSWORD")
                                ? QStringLiteral("protocol-probe")
                                : qEnvironmentVariable("FURUI_PWD_PASSWORD");
        QTimer::singleShot(600, ctl, [ctl, pwdSource, acc, pwd] {
            ctl->loginByPassword(pwdSource, acc, pwd, qEnvironmentVariable("FURUI_PWD_CAPTCHA"));
        });
        QTimer::singleShot(qEnvironmentVariableIntValue("FURUI_PWD_DIAG_MS") > 0
                               ? qEnvironmentVariableIntValue("FURUI_PWD_DIAG_MS") : 20000,
                           &engine, [] { QCoreApplication::quit(); });
    }

    return app.exec();
}
